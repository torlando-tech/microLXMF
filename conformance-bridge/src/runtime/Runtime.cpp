#include "Runtime.h"
#include "PosixTCPInterface.h"

#include <Cryptography/Random.h>
#include <Destination.h>
#include <Log.h>
#include <Utilities/OS.h>

#include <chrono>
#include <stdexcept>
#include <sys/stat.h>

// Don't `using namespace LXMF` — both LXMF and RNS define a nested `Type`
// namespace and unqualified Type:: usage becomes ambiguous.
using namespace RNS;

namespace bridge {

Runtime& Runtime::instance() {
    static Runtime r;
    return r;
}

void Runtime::init(const std::string& storage_path, const std::string& display_name) {
    std::lock_guard<std::mutex> lock(_lifecycle_mutex);
    if (_initialized.load()) {
        throw std::runtime_error("Runtime already initialized");
    }
    _storage_path = storage_path.empty() ? "./microlxmf-state" : storage_path;
    _display_name = display_name;

    // Make sure storage dirs exist (PosixFileSystem doesn't auto-mkdir).
    ::mkdir(_storage_path.c_str(), 0700);
    std::string ident_dir = _storage_path + "/storage";
    ::mkdir(ident_dir.c_str(), 0700);
    std::string cache_dir = _storage_path + "/cache";
    ::mkdir(cache_dir.c_str(), 0700);

    {
        microStore::Adapters::PosixFileSystem stack_fs(_storage_path.c_str());
        _fs = stack_fs;  // shared_ptr copy
    }
    Utilities::OS::register_filesystem(_fs);

    // Reticulum singleton — its constructor sets up RNG, paths.
    _reticulum.reset(new Reticulum());
    Reticulum::transport_enabled(false);

    // Identity. Try to load a saved one; on miss, generate.
    std::string ident_file = "identity";
    Bytes priv;
    bool loaded = false;
    try {
        if (Utilities::OS::file_exists(ident_file.c_str())) {
            Utilities::OS::read_file(ident_file.c_str(), priv);
            if (priv.size() == 64) loaded = true;
        }
    } catch (const std::exception&) {
        // First boot — file doesn't exist.
    }
    if (loaded) {
        _identity = Identity(false);
        _identity.load_private_key(priv);
    } else {
        _identity = Identity();  // generates fresh key pair
        const Bytes& pk = _identity.get_private_key();
        Utilities::OS::write_file(ident_file.c_str(), pk);
    }

    // Start Reticulum AFTER interfaces are added — but the router needs
    // it so we start now and register interfaces later. (Mirrors
    // pyxis/src/main.cpp ordering.)
    _reticulum->start();

    // LXMRouter.
    _router = std::make_shared<LXMF::LXMRouter>(_identity, _storage_path,
                                                /*announce_at_start=*/false);
    _router->set_display_name(_display_name);
    _router->register_delivery_callback(
        [this](LXMF::LXMessage& m) { this->on_delivery(m); });
    // Track outbound state transitions so lxmf_get_message_state reflects
    // SENT / DELIVERED / FAILED, not just the OUTBOUND we set at send time.
    _router->register_sent_callback([this](LXMF::LXMessage& m) {
        std::lock_guard<std::mutex> g(_outbound_mutex);
        _outbound_states[m.hash()] = LXMF::Type::Message::SENT;
    });
    _router->register_delivered_callback([this](LXMF::LXMessage& m) {
        std::lock_guard<std::mutex> g(_outbound_mutex);
        _outbound_states[m.hash()] = LXMF::Type::Message::DELIVERED;
    });
    _router->register_failed_callback([this](LXMF::LXMessage& m) {
        std::lock_guard<std::mutex> g(_outbound_mutex);
        _outbound_states[m.hash()] = LXMF::Type::Message::FAILED;
    });

    // Worker thread.
    _stopping.store(false);
    _worker_thread = std::thread(&Runtime::worker_loop, this);

    _initialized.store(true);
}

void Runtime::shutdown() {
    std::lock_guard<std::mutex> lock(_lifecycle_mutex);
    if (!_initialized.exchange(false)) return;
    _stopping.store(true);
    if (_worker_thread.joinable()) _worker_thread.join();

    // Stop interfaces.
    for (auto& iface : _interfaces) {
        if (iface) iface->stop_iface();
    }
    _iface_handles.clear();
    _interfaces.clear();
    _router.reset();
    _reticulum.reset();
    _fs.clear();
}

void Runtime::worker_loop() {
    using namespace std::chrono;
    while (!_stopping.load()) {
        try {
            if (_reticulum) {
                _reticulum->loop();
                _reticulum->jobs();
            }
            if (_router) {
                _router->process_outbound();
                _router->process_inbound();
            }
        } catch (const std::exception& e) {
            ERROR(std::string("Runtime worker loop exception: ") + e.what());
        }
        std::this_thread::sleep_for(milliseconds(20));
    }
}

int Runtime::add_tcp_server_interface(const std::string& name, int port) {
    if (!_initialized.load()) throw std::runtime_error("Runtime not initialized");
    auto impl = std::make_shared<PosixTCPInterface>(
        name.c_str(), PosixTCPInterface::SERVER);
    impl->set_bind("127.0.0.1", port);
    if (!impl->start_iface()) {
        throw std::runtime_error("PosixTCPInterface(SERVER): start failed");
    }
    int bound = impl->bound_port();
    // Register as RNS interface. Interface(InterfaceImpl*) takes
    // ownership via shared_ptr — but our impl is already in a shared_ptr,
    // so we hand a raw aliased pointer over and Interface's shared_ptr
    // will reference the same control block.
    auto handle = std::unique_ptr<Interface>(new Interface(impl.get()));
    Transport::register_interface(*handle);
    _interfaces.push_back(impl);
    _iface_handles.push_back(std::move(handle));
    return bound;
}

void Runtime::add_tcp_client_interface(const std::string& name,
                                       const std::string& host, int port) {
    if (!_initialized.load()) throw std::runtime_error("Runtime not initialized");
    auto impl = std::make_shared<PosixTCPInterface>(
        name.c_str(), PosixTCPInterface::CLIENT);
    impl->set_target(host, port);
    if (!impl->start_iface()) {
        throw std::runtime_error("PosixTCPInterface(CLIENT): start failed");
    }
    auto handle = std::unique_ptr<Interface>(new Interface(impl.get()));
    Transport::register_interface(*handle);
    _interfaces.push_back(impl);
    _iface_handles.push_back(std::move(handle));
}

void Runtime::announce() {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");
    _router->announce();
}

Bytes Runtime::send_opportunistic(const Bytes& dest_hash,
                                  const std::string& content,
                                  const std::string& title) {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");

    // Look up recipient identity if known. For OPPORTUNISTIC, LXMessage's
    // "from hashes" constructor is sufficient — the router will route by
    // path table.
    Identity recipient_identity = Identity::recall(dest_hash);
    Destination dest{RNS::Type::NONE};
    if (recipient_identity) {
        dest = Destination(recipient_identity, RNS::Type::Destination::OUT,
                           RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    }

    Bytes content_b{(const uint8_t*)content.data(), content.size()};
    Bytes title_b{(const uint8_t*)title.data(), title.size()};

    // Use the LXMRouter's already-registered IN delivery destination as
    // the source — constructing a fresh one here would double-register.
    LXMF::LXMessage m(dest, _router->delivery_destination(), content_b,
                      title_b, LXMF::Type::Message::OPPORTUNISTIC);
    if (!dest) {
        m = LXMF::LXMessage(dest_hash, _identity.hash(), content_b, title_b,
                            LXMF::Type::Message::OPPORTUNISTIC);
    }
    m.pack();
    Bytes hash = m.hash();
    {
        std::lock_guard<std::mutex> g(_outbound_mutex);
        _outbound_states[hash] = LXMF::Type::Message::OUTBOUND;
    }
    _router->handle_outbound(m);
    return hash;
}

std::vector<Runtime::ReceivedMsg> Runtime::get_received_messages(
    uint64_t since_seq, uint64_t& last_seq_out) {
    std::lock_guard<std::mutex> g(_inbound_mutex);
    std::vector<ReceivedMsg> out;
    for (const auto& m : _inbound) {
        if (m.seq > since_seq) out.push_back(m);
    }
    last_seq_out = _inbound_seq_counter;
    return out;
}

std::string Runtime::get_message_state(const Bytes& message_hash) {
    std::lock_guard<std::mutex> g(_outbound_mutex);
    auto it = _outbound_states.find(message_hash);
    if (it == _outbound_states.end()) return "unknown";
    return state_to_string(it->second);
}

Bytes Runtime::identity_hash() const {
    return _identity ? _identity.hash() : Bytes();
}

Bytes Runtime::delivery_destination_hash() const {
    // The LXMRouter constructs the IN/lxmf:delivery destination and
    // exposes it via delivery_destination(). Constructing a second one
    // here would trigger Transport's "already registered" guard.
    if (!_router) return Bytes();
    return _router->delivery_destination().hash();
}

void Runtime::on_delivery(LXMF::LXMessage& msg) {
    ReceivedMsg rm;
    {
        std::lock_guard<std::mutex> g(_inbound_mutex);
        rm.seq = ++_inbound_seq_counter;
    }
    rm.message_hash = msg.hash();
    rm.source_hash = msg.source_hash();
    rm.destination_hash = msg.destination_hash();
    {
        const Bytes& t = msg.title();
        rm.title = std::string((const char*)t.data(), t.size());
        const Bytes& c = msg.content();
        rm.content = std::string((const char*)c.data(), c.size());
    }
    rm.method = "opportunistic";  // Phase-1 only supports opportunistic
    rm.ack_status = "received";
    rm.received_at_ms = (uint64_t)(Utilities::OS::time() * 1000.0);

    std::lock_guard<std::mutex> g(_inbound_mutex);
    _inbound.push_back(std::move(rm));
}

std::string Runtime::state_to_string(LXMF::Type::Message::State s) {
    switch (s) {
        case LXMF::Type::Message::GENERATING: return "generating";
        case LXMF::Type::Message::OUTBOUND:   return "outbound";
        case LXMF::Type::Message::SENDING:    return "sending";
        case LXMF::Type::Message::SENT:       return "sent";
        case LXMF::Type::Message::DELIVERED:  return "delivered";
        case LXMF::Type::Message::REJECTED:   return "rejected";
        case LXMF::Type::Message::CANCELLED:  return "cancelled";
        case LXMF::Type::Message::FAILED:     return "failed";
    }
    return "unknown";
}

}  // namespace bridge
