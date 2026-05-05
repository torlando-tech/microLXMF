// POSIX-only single-peer TCP interface for the microLXMF conformance bridge.
//
// Supports two modes:
//   - SERVER: bind + listen on a local port, accept the first connecting peer
//     and serve as that peer's interface. Phase-1 lxmf-conformance is a
//     2-bridge topology, so a single accepted peer is sufficient.
//   - CLIENT: connect to a remote host:port and serve as that connection's
//     interface.
//
// Owns:
//   - one socket fd
//   - one reader thread (deframes HDLC, calls handle_incoming)
//   - one write mutex (send_outgoing is called from the bridge command
//     thread; reader thread doesn't write)
//
// HDLC wire format matches Python RNS TCPInterface.

#pragma once

#include <Interface.h>
#include <Bytes.h>
#include <Log.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

namespace bridge {

class PosixTCPInterface : public RNS::InterfaceImpl {

public:
    enum Mode { CLIENT, SERVER };

    static constexpr uint32_t BITRATE_GUESS = 10 * 1000 * 1000;  // 10 Mbps
    static constexpr uint16_t HW_MTU = 1064;

    PosixTCPInterface(const char* name, Mode mode);
    virtual ~PosixTCPInterface();

    // CLIENT-mode setters (call before start()).
    void set_target(const std::string& host, int port) {
        _target_host = host;
        _target_port = port;
    }

    // SERVER-mode setters (call before start()). port=0 means "OS-assigned".
    void set_bind(const std::string& host, int port) {
        _bind_host = host;
        _bind_port = port;
    }

    // After start() in SERVER mode, returns the actually-bound port.
    int bound_port() const { return _bound_port; }

    bool is_online() const { return _online.load(); }

    // RNS::InterfaceImpl override.
    virtual void send_outgoing(const RNS::Bytes& data) override;

    // Externally-callable lifecycle (RNS::InterfaceImpl::start/stop are
    // protected; we expose them publicly for the bridge runtime).
    bool start_iface();
    void stop_iface();

private:
    // Reader thread body — drives recv() loop, deframes HDLC, calls
    // InterfaceImpl::handle_incoming for each complete frame.
    void reader_loop();
    // SERVER mode: accept() loop body. Currently accepts one peer then
    // hands control back to reader_loop.
    void accept_one();

    Mode _mode;
    std::string _target_host;
    int _target_port = 0;
    std::string _bind_host = "127.0.0.1";
    int _bind_port = 0;
    int _bound_port = 0;

    std::atomic<bool> _online{false};
    std::atomic<bool> _stopping{false};

    int _listen_socket = -1;  // SERVER mode only
    int _data_socket = -1;
    std::mutex _write_mutex;
    std::thread _reader_thread;
};

}  // namespace bridge
