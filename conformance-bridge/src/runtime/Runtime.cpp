#include "Runtime.h"
#include "PosixTCPInterface.h"
#include "MsgPackUtil.h"

#include <microReticulum/Cryptography/Random.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Log.h>
#include <microReticulum/Utilities/OS.h>
#include <RNG.h>

#include <fstream>

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
    if (storage_path.empty()) {
        // Conformance harness invokes lxmf_init without a storage_path
        // and both bridges run in the same CWD. PosixFileSystem doesn't
        // honor its basepath argument either (every file op happens at
        // CWD), so a shared default like "./microlxmf-state" causes the
        // two bridges to share identity, path table, message store —
        // collapsing the topology to a single virtual node. Default to a
        // tmpdir keyed by pid so concurrent bridge processes never
        // collide.
        char tmp[256];
        std::snprintf(tmp, sizeof(tmp), "/tmp/microlxmf-bridge-%d-XXXXXX", (int)getpid());
        char* mk = mkdtemp(tmp);
        if (!mk) throw std::runtime_error("Could not create per-bridge tmpdir");
        _storage_path = mk;
    } else {
        _storage_path = storage_path;
    }
    _display_name = display_name;

    // Make sure storage dirs exist (PosixFileSystem doesn't auto-mkdir).
    ::mkdir(_storage_path.c_str(), 0700);
    std::string ident_dir = _storage_path + "/storage";
    ::mkdir(ident_dir.c_str(), 0700);
    std::string cache_dir = _storage_path + "/cache";
    ::mkdir(cache_dir.c_str(), 0700);

    // microStore::Adapters::PosixFileSystem accepts a basepath in its
    // constructor but does NOT prepend it to ::open/::stat/::opendir
    // calls — every file op happens at the process CWD. The
    // conformance harness spawns multiple bridges from the same CWD,
    // so without chdir'ing they'd all share `identity`, persistent
    // path tables, etc. Chdir into the per-bridge storage path so each
    // process has its own filesystem-rooted view.
    if (::chdir(_storage_path.c_str()) != 0) {
        throw std::runtime_error("chdir into storage_path failed: " + _storage_path);
    }

    {
        microStore::Adapters::UniversalFileSystem stack_fs(".");
        _fs = stack_fs;  // shared_ptr copy
    }
    Utilities::OS::register_filesystem(_fs);

    // Reticulum singleton — its constructor sets up RNG, paths.
    _reticulum.reset(new Reticulum());
    // Enable transport mode. This is misleadingly named — it controls
    // whether the path_store (microStore-backed path table) is
    // initialized in Transport::start, NOT just whether we route for
    // other peers. With transport_enabled=false (pyxis's default since
    // pyxis is a leaf node), the path_store is never init'd and every
    // call to _new_path_table.put returns false. That means the path
    // table never learns from inbound announces, so DIRECT delivery
    // can't find a peer's path and send_opportunistic also fails the
    // recipient identity recall. The conformance bridge always needs
    // its own path table populated, so we always enable it here.
    Reticulum::transport_enabled(true);

    // attermann/Crypto's RNG.begin() is fully deterministic on host:
    // ChaCha20 seeded with a hardcoded constant + tag string "Reticulum".
    // No /dev/urandom or platform-specific entropy is mixed in for non-
    // Arduino builds, so every bridge process produces the SAME identity.
    // Two bridges in the conformance harness then collide on identity hash,
    // breaking opportunistic routing (everything routes locally instead
    // of crossing the TCP link), DIRECT link establishment (you can't
    // open a Link to your own destination), and propagation.
    //
    // RNG.stir() does mix entropy into the ChaCha state but downstream
    // consumers may have already pulled bytes from RNG before stir takes
    // effect. The robust fix: bypass the RNG entirely for the initial
    // keypair by reading 64 bytes from /dev/urandom and using
    // Identity::load_private_key (which sets X25519 + Ed25519 priv bytes
    // directly without invoking RNG).
    auto read_urandom = [](size_t n) -> Bytes {
        Bytes out;
        std::ifstream f("/dev/urandom", std::ios::binary);
        if (!f) return out;
        std::vector<uint8_t> buf(n);
        f.read(reinterpret_cast<char*>(buf.data()), n);
        if ((size_t)f.gcount() != n) return out;
        out.assign(buf.data(), n);
        return out;
    };
    // Stir RNG with 32 fresh bytes anyway so anything else that pulls
    // from RNG (e.g. ephemeral X25519 in Identity::encrypt) is also
    // de-collided.
    {
        Bytes seed = read_urandom(32);
        if (seed.size() == 32) RNG.stir(seed.data(), 32, 256);
    }

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
        Bytes priv_bytes = read_urandom(64);
        if (priv_bytes.size() != 64) {
            throw std::runtime_error("Failed to read 64 bytes from /dev/urandom");
        }
        _identity = Identity(false);
        _identity.load_private_key(priv_bytes);
        Utilities::OS::write_file(ident_file.c_str(), priv_bytes);
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
    _router->register_progress_callback([this](LXMF::LXMessage& m) {
        std::lock_guard<std::mutex> g(_outbound_mutex);
        _outbound_progress[m.hash()] = m.progress();
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
                if (_sync_request_pending.exchange(false)) {
                    _router->request_messages_from_propagation_node();
                }
                _router->process_sync();
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

// Common helper — build, pack, queue an LXMessage via the router. Returns
// the message hash so command handlers can echo it back.
static Bytes send_message_internal(
    LXMF::LXMRouter& router,
    const Identity& self_identity,
    const Bytes& dest_hash,
    const std::string& content,
    const std::string& title,
    const Runtime::FieldList& fields,
    LXMF::Type::Message::Method method,
    std::mutex& outbound_mutex,
    std::map<Bytes, LXMF::Type::Message::State>& outbound_states,
    double timestamp = 0.0)
{
    Identity recipient_identity = Identity::recall(dest_hash);
    Destination dest{RNS::Type::NONE};
    if (recipient_identity) {
        dest = Destination(recipient_identity, RNS::Type::Destination::OUT,
                           RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    }

    Bytes content_b{(const uint8_t*)content.data(), content.size()};
    Bytes title_b{(const uint8_t*)title.data(), title.size()};

    LXMF::LXMessage m(dest, router.delivery_destination(), content_b,
                      title_b, method);
    if (!dest) {
        // Recall miss — use hash-mode constructor; router will route via
        // path table once available.
        m = LXMF::LXMessage(dest_hash, self_identity.hash(), content_b,
                            title_b, method);
    }
    for (const auto& kv : fields) {
        m.fields_set(kv.first, kv.second);
    }
    // Pin the timestamp BEFORE pack(). LXMessage::pack() only assigns
    // OS::time() when _timestamp is still 0.0, so a non-zero pre-set
    // value flows into the hashed-part. Mirrors python bridge's
    // `message.timestamp = float(forced_timestamp)` at lxmf_python.py:735.
    if (timestamp != 0.0) {
        m.timestamp(timestamp);
    }
    m.pack();
    Bytes hash = m.hash();
    {
        std::lock_guard<std::mutex> g(outbound_mutex);
        outbound_states[hash] = LXMF::Type::Message::OUTBOUND;
    }
    router.handle_outbound(m);
    return hash;
}

Bytes Runtime::send_opportunistic(const Bytes& dest_hash,
                                  const std::string& content,
                                  const std::string& title,
                                  const FieldList& fields,
                                  double timestamp) {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");
    return send_message_internal(*_router, _identity, dest_hash, content, title,
                                 fields, LXMF::Type::Message::OPPORTUNISTIC,
                                 _outbound_mutex, _outbound_states, timestamp);
}

Bytes Runtime::send_direct(const Bytes& dest_hash,
                           const std::string& content,
                           const std::string& title,
                           const FieldList& fields,
                           double timestamp) {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");
    return send_message_internal(*_router, _identity, dest_hash, content, title,
                                 fields, LXMF::Type::Message::DIRECT,
                                 _outbound_mutex, _outbound_states, timestamp);
}

Bytes Runtime::send_propagated(const Bytes& dest_hash,
                               const std::string& content,
                               const std::string& title,
                               const FieldList& fields,
                               double timestamp) {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");
    return send_message_internal(*_router, _identity, dest_hash, content, title,
                                 fields, LXMF::Type::Message::PROPAGATED,
                                 _outbound_mutex, _outbound_states, timestamp);
}

void Runtime::request_path(const Bytes& destination_hash) {
    if (!_initialized.load()) throw std::runtime_error("Runtime not initialized");
    Transport::request_path(destination_hash);
}

bool Runtime::has_path(const Bytes& destination_hash) {
    if (!_initialized.load()) return false;
    return Transport::has_path(destination_hash);
}

void Runtime::set_outbound_propagation_node(const Bytes& node_hash, uint8_t stamp_cost) {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");
    _router->set_outbound_propagation_node(node_hash);
    _router->set_outbound_propagation_stamp_cost(stamp_cost);
}

Runtime::SyncResult Runtime::sync_inbound(double timeout_sec) {
    if (!_router) throw std::runtime_error("LXMRouter not initialized");

    // Latch the result via the sync-complete callback. The router fires
    // the callback exactly once on PR_COMPLETE; failures just leave the
    // state at PR_FAILED, which we observe by polling.
    std::atomic<size_t> received{0};
    std::atomic<bool> completed{false};
    _router->register_sync_complete_callback(
        [&](size_t n) { received.store(n); completed.store(true); });

    // Defer the actual request to the worker thread — the LXMRouter
    // sync state machine touches RNS Transport / Link state that must
    // be mutated on the loop thread.
    _sync_request_pending.store(true);

    using namespace std::chrono;
    auto deadline = steady_clock::now() + duration_cast<steady_clock::duration>(
        duration<double>(timeout_sec));
    while (steady_clock::now() < deadline) {
        auto st = _router->get_sync_state();
        if (completed.load() || st == LXMF::LXMRouter::PR_COMPLETE) {
            return {"complete", received.load()};
        }
        if (st == LXMF::LXMRouter::PR_FAILED) {
            return {"failed", 0};
        }
        std::this_thread::sleep_for(milliseconds(50));
    }
    return {"timeout", received.load()};
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

float Runtime::get_message_progress(const Bytes& message_hash) {
    std::lock_guard<std::mutex> g(_outbound_mutex);
    auto it = _outbound_progress.find(message_hash);
    if (it == _outbound_progress.end()) {
        // No progress recorded — either we haven't sent this message,
        // or it took the PACKET path (small payloads, no resource).
        // Convention: -1.0 signals "no progress observed".
        return -1.0f;
    }
    return it->second;
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
    rm.message_hash = msg.hash();
    rm.source_hash = msg.source_hash();
    rm.destination_hash = msg.destination_hash();
    {
        const Bytes& t = msg.title();
        rm.title = std::string((const char*)t.data(), t.size());
        const Bytes& c = msg.content();
        rm.content = std::string((const char*)c.data(), c.size());
    }
    // Decode each field's raw msgpack key+value into the harness
    // "inbox" shape (str-keyed object, python-style values).
    // field_at(i) takes the POOL index, not a sequential index — scan
    // the whole pool and decode only `in_use` entries. Each field's
    // key+value are stored as raw msgpack byte spans (set by
    // LXMessage::unpack); decode each via the bridge's hand-rolled
    // walker into the lxmf-conformance "inbox" shape.
    for (size_t i = 0; i < LXMF::MAX_FIELDS; ++i) {
        const auto* fe = msg.field_at(i);
        if (!fe) continue;
        try {
            MsgPackDecoder kdec(fe->key.data(), fe->key.size());
            json k_json = kdec.read_value();
            std::string k_str;
            if (k_json.is_number_integer()) k_str = std::to_string(k_json.get<int64_t>());
            else if (k_json.is_string()) k_str = k_json.get<std::string>();
            else k_str = k_json.dump();

            MsgPackDecoder vdec(fe->value.data(), fe->value.size());
            json v_json = vdec.read_value();
            rm.fields[k_str] = v_json;
        } catch (const std::exception&) {
            // skip undecodable field
        }
    }
    switch (msg.method()) {
        case LXMF::Type::Message::OPPORTUNISTIC: rm.method = "opportunistic"; break;
        case LXMF::Type::Message::DIRECT:        rm.method = "direct";        break;
        case LXMF::Type::Message::PROPAGATED:    rm.method = "propagated";    break;
        case LXMF::Type::Message::PAPER:         rm.method = "paper";         break;
    }
    rm.ack_status = "received";
    rm.received_at_ms = (uint64_t)(Utilities::OS::time() * 1000.0);

    // Advance seq + push under a SINGLE lock so a concurrent reader can't
    // observe last_seq=N but find _inbound empty (causing it to skip
    // seq=N on the next drain).
    std::lock_guard<std::mutex> g(_inbound_mutex);
    rm.seq = ++_inbound_seq_counter;
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
