#include "PosixTCPInterface.h"
#include "HDLC.h"

#include <microReticulum/Type.h>
#include <microReticulum/Transport.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <cstring>
#include <vector>

using namespace RNS;

namespace bridge {

PosixTCPInterface::PosixTCPInterface(const char* name, Mode mode)
    : RNS::InterfaceImpl(name), _mode(mode) {
    _IN = true;
    _OUT = true;
    _bitrate = BITRATE_GUESS;
    _HW_MTU = HW_MTU;
}

PosixTCPInterface::~PosixTCPInterface() {
    stop_iface();
}

bool PosixTCPInterface::start_iface() {
    if (_mode == CLIENT) {
        struct in_addr addr;
        int parsed = inet_aton(_target_host.c_str(), &addr);
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(_target_port);
        if (parsed) {
            server_addr.sin_addr.s_addr = addr.s_addr;
        } else {
            struct hostent* he = gethostbyname(_target_host.c_str());
            if (!he || !he->h_addr_list[0]) {
                ERROR("PosixTCPInterface: gethostbyname failed for " + _target_host);
                return false;
            }
            std::memcpy(&server_addr.sin_addr.s_addr, he->h_addr_list[0], sizeof(in_addr_t));
        }
        _data_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (_data_socket < 0) {
            ERROR("PosixTCPInterface: socket() failed");
            return false;
        }
        if (::connect(_data_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            ERROR("PosixTCPInterface: connect failed: " + std::string(std::strerror(errno)));
            ::close(_data_socket);
            _data_socket = -1;
            return false;
        }
        int flag = 1;
        setsockopt(_data_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        _online = true;
        _reader_thread = std::thread(&PosixTCPInterface::reader_loop, this);
        return true;
    } else {
        // SERVER mode: bind + listen, then spawn accept loop in a worker
        // thread. The accept thread morphs into the reader thread once a
        // peer connects.
        _listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (_listen_socket < 0) {
            ERROR("PosixTCPInterface: socket() failed");
            return false;
        }
        int reuse = 1;
        setsockopt(_listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(_bind_port);
        bind_addr.sin_addr.s_addr = inet_addr(_bind_host.c_str());
        if (::bind(_listen_socket, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            ERROR("PosixTCPInterface: bind failed: " + std::string(std::strerror(errno)));
            ::close(_listen_socket);
            _listen_socket = -1;
            return false;
        }
        sockaddr_in actual{};
        socklen_t alen = sizeof(actual);
        getsockname(_listen_socket, reinterpret_cast<sockaddr*>(&actual), &alen);
        _bound_port = ntohs(actual.sin_port);
        if (listen(_listen_socket, 4) < 0) {
            ERROR("PosixTCPInterface: listen failed");
            ::close(_listen_socket);
            _listen_socket = -1;
            return false;
        }
        _reader_thread = std::thread(&PosixTCPInterface::accept_one, this);
        return true;
    }
}

void PosixTCPInterface::stop_iface() {
    if (_stopping.exchange(true)) return;
    _online = false;
    if (_data_socket >= 0) {
        ::shutdown(_data_socket, SHUT_RDWR);
        ::close(_data_socket);
        _data_socket = -1;
    }
    if (_listen_socket >= 0) {
        ::shutdown(_listen_socket, SHUT_RDWR);
        ::close(_listen_socket);
        _listen_socket = -1;
    }
    if (_reader_thread.joinable()) _reader_thread.join();
}

void PosixTCPInterface::accept_one() {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    int fd = ::accept(_listen_socket, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (fd < 0) {
        if (!_stopping.load()) {
            ERROR("PosixTCPInterface: accept failed: " + std::string(std::strerror(errno)));
        }
        return;
    }
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    _data_socket = fd;
    _online = true;
    INFO("PosixTCPInterface: accepted peer on port " + std::to_string(_bound_port));
    reader_loop();
}

void PosixTCPInterface::reader_loop() {
    Bytes frame_buffer;
    std::vector<uint8_t> recv_buf(4096);

    while (!_stopping.load()) {
        int fd = _data_socket;
        if (fd < 0) break;

        ssize_t n = ::recv(fd, recv_buf.data(), recv_buf.size(), 0);
        if (n == 0) {
            DEBUG("PosixTCPInterface: peer closed connection");
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (_stopping.load()) break;
            ERROR("PosixTCPInterface: recv error: " + std::string(std::strerror(errno)));
            break;
        }
        frame_buffer.append(recv_buf.data(), n);

        // Extract complete HDLC frames.
        while (true) {
            if (frame_buffer.size() == 0) break;
            int start = -1;
            for (size_t i = 0; i < frame_buffer.size(); ++i) {
                if (frame_buffer.data()[i] == HDLC::FLAG) { start = (int)i; break; }
            }
            if (start < 0) {
                frame_buffer.clear();
                break;
            }
            if (start > 0) frame_buffer = frame_buffer.mid(start);
            int end = -1;
            for (size_t i = 1; i < frame_buffer.size(); ++i) {
                if (frame_buffer.data()[i] == HDLC::FLAG) { end = (int)i; break; }
            }
            if (end < 0) break;  // wait for more data
            Bytes content = frame_buffer.mid(1, end - 1);
            frame_buffer = frame_buffer.mid(end);
            if (content.size() == 0) continue;
            Bytes unescaped = HDLC::unescape(content);
            if (unescaped.size() == 0) continue;
            if (unescaped.size() < Type::Reticulum::HEADER_MINSIZE) continue;
            // Reader threads must not enter process-global Reticulum state.
            // Runtime drains this bounded queue while holding _router_mutex.
            {
                std::lock_guard<std::mutex> lock(_incoming_mutex);
                if (_incoming.size() >= MAX_PENDING_INBOUND) {
                    WARNING("PosixTCPInterface: inbound queue full; dropping frame");
                } else {
                    _incoming.push_back(unescaped);
                }
            }
        }
    }
    _online = false;
}

void PosixTCPInterface::drain_incoming() {
    for (std::size_t processed = 0; processed < MAX_DRAIN_PER_TICK; ++processed) {
        Bytes frame;
        {
            std::lock_guard<std::mutex> lock(_incoming_mutex);
            if (_incoming.empty()) return;
            frame = _incoming.front();
            _incoming.pop_front();
        }
        try {
            InterfaceImpl::handle_incoming(frame);
        } catch (const std::exception& e) {
            ERROR("PosixTCPInterface: handle_incoming threw: " + std::string(e.what()));
        }
    }
}

bool PosixTCPInterface::send_outgoing(const Bytes& data) {
    if (!_online.load()) return false;
    Bytes framed = HDLC::frame(data);
    std::lock_guard<std::mutex> lock(_write_mutex);
    int fd = _data_socket;
    if (fd < 0) return false;
    size_t total = 0;
    const uint8_t* buf = framed.data();
    size_t len = framed.size();
    while (total < len) {
        ssize_t n = ::send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            ERROR("PosixTCPInterface: send error: " + std::string(std::strerror(errno)));
            _online = false;
            return false;
        }
        total += (size_t)n;
    }
    InterfaceImpl::handle_outgoing(data);
    return true;
}

}  // namespace bridge
