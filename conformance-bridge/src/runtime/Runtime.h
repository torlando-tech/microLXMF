// Bridge runtime — owns the long-lived microReticulum + microLXMF state
// for one node.

#include "../json.hpp"
//
// One process represents one LXMF node. Runtime is a singleton because
// microReticulum's `Reticulum`, `Transport`, `Identity::recall` are
// process-global static state — there is no clean way to host two RNS
// stacks in one binary.
//
// Threading model:
//   - main thread: bridge::JSON-RPC dispatch (runs `commands/lxmf.cpp`
//     handlers, mutates Runtime via the `lock()` accessor)
//   - worker thread: drives Reticulum::loop / Reticulum::jobs and
//     LXMRouter::process_outbound / process_inbound at ~50 Hz
//   - LXMRouter delivery callback fires on the worker thread; it mutexes
//     the inbound deque so the command thread can drain it safely

#pragma once

#include <Bytes.h>
#include <Identity.h>
#include <Reticulum.h>
#include <Transport.h>
#include <microStore/Adapters/PosixFileSystem.h>

#include "../../../src/LXMF/LXMRouter.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bridge {

class PosixTCPInterface;

class Runtime {
public:
    struct ReceivedMsg {
        uint64_t seq = 0;
        RNS::Bytes message_hash;
        RNS::Bytes source_hash;
        RNS::Bytes destination_hash;
        std::string title;
        std::string content;
        std::string method;     // "opportunistic", "direct", "propagated"
        std::string ack_status; // "received"
        uint64_t received_at_ms = 0;
        // LXMF fields decoded from msgpack into the harness's "inbox"
        // shape: {"<int_key>": <python-encoded value>}. Bytes-typed
        // fields are bare hex strings; nested arrays/maps recurse.
        nlohmann::json fields = nlohmann::json::object();
    };

    static Runtime& instance();

    // Lifecycle.
    void init(const std::string& storage_path, const std::string& display_name);
    void shutdown();
    bool is_initialized() const { return _initialized.load(); }

    // Interfaces. Returns the bound port for the server case; throws on
    // failure for both.
    int add_tcp_server_interface(const std::string& name, int port);
    void add_tcp_client_interface(const std::string& name,
                                  const std::string& host, int port);

    // LXMF actions. `fields` is a vector of (key, value) byte pairs —
    // each entry is a pre-encoded msgpack key-bytes / value-bytes pair
    // that LXMessage::pack splices raw into the fields map.
    using FieldList = std::vector<std::pair<RNS::Bytes, RNS::Bytes>>;

    void announce();
    RNS::Bytes send_opportunistic(const RNS::Bytes& dest_hash,
                                  const std::string& content,
                                  const std::string& title,
                                  const FieldList& fields = {});
    RNS::Bytes send_direct(const RNS::Bytes& dest_hash,
                           const std::string& content,
                           const std::string& title,
                           const FieldList& fields = {});
    RNS::Bytes send_propagated(const RNS::Bytes& dest_hash,
                               const std::string& content,
                               const std::string& title,
                               const FieldList& fields = {});

    // Phase-1 propagation helpers. The bridge harness configures an
    // outbound propagation node via these before sending PROPAGATED
    // messages.
    void set_outbound_propagation_node(const RNS::Bytes& node_hash, uint8_t stamp_cost);

    // Path queries.
    void request_path(const RNS::Bytes& destination_hash);
    bool has_path(const RNS::Bytes& destination_hash);

    // Inbound queue.
    std::vector<ReceivedMsg> get_received_messages(uint64_t since_seq,
                                                   uint64_t& last_seq_out);

    // Outbound state map.
    std::string get_message_state(const RNS::Bytes& message_hash);

    // Identity hashes (LXMF semantics).
    RNS::Bytes identity_hash() const;
    RNS::Bytes delivery_destination_hash() const;

private:
    Runtime() = default;
    ~Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void worker_loop();
    void on_delivery(LXMF::LXMessage& msg);
    static std::string state_to_string(LXMF::Type::Message::State s);

    std::atomic<bool> _initialized{false};
    std::atomic<bool> _stopping{false};

    // microStore::FileSystem wraps shared_ptr<FileSystemImpl> internally,
    // so passing-by-value retains lifetime. PosixFileSystem disables
    // operator new — we construct on the stack and copy into _fs.
    microStore::FileSystem _fs;

    // RNS pieces.
    std::unique_ptr<RNS::Reticulum> _reticulum;
    RNS::Identity _identity{RNS::Type::NONE};
    std::string _storage_path;
    std::string _display_name;

    // LXMF router. shared_ptr because LXMRouter::Ptr exists for
    // co-ownership patterns.
    std::shared_ptr<LXMF::LXMRouter> _router;

    // Interfaces — keep them alive.
    std::vector<std::shared_ptr<PosixTCPInterface>> _interfaces;
    std::vector<std::unique_ptr<RNS::Interface>> _iface_handles;

    // Worker.
    std::thread _worker_thread;

    // Inbound state.
    std::mutex _inbound_mutex;
    std::deque<ReceivedMsg> _inbound;
    uint64_t _inbound_seq_counter = 0;

    // Outbound state map: message_hash -> state.
    std::mutex _outbound_mutex;
    std::map<RNS::Bytes, LXMF::Type::Message::State> _outbound_states;

    // Top-level mutex around init/shutdown + interface registration.
    std::mutex _lifecycle_mutex;
};

}  // namespace bridge
