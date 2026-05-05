// LXMF command handlers.
//
// Wired to the bridge::Runtime singleton. Each handler's command name
// matches lxmf-conformance/README.md "Bridge protocol".

#include "../bridge.h"
#include "../runtime/Runtime.h"

#include <Bytes.h>

#include <stdexcept>

namespace {

inline RNS::Bytes to_rns(const bridge::Bytes& v) {
    return RNS::Bytes(v.data(), v.size());
}
inline bridge::Bytes from_rns(const RNS::Bytes& b) {
    return bridge::Bytes(b.data(), b.data() + b.size());
}

}  // namespace

REGISTER_COMMAND(lxmf_init, {
    auto& rt = bridge::Runtime::instance();
    std::string storage_path = bridge::str_param_or(p, "storage_path", "");
    std::string display_name = bridge::str_param_or(p, "display_name", "microlxmf-bridge");
    rt.init(storage_path, display_name);
    return bridge::json{
        {"identity_hash", bridge::to_hex(from_rns(rt.identity_hash()))},
        {"delivery_destination_hash",
         bridge::to_hex(from_rns(rt.delivery_destination_hash()))},
        {"config_dir", storage_path},
        {"storage_path", storage_path},
    };
})

REGISTER_COMMAND(lxmf_add_tcp_server_interface, {
    auto& rt = bridge::Runtime::instance();
    std::string name = bridge::str_param_or(p, "name", "tcp_server");
    int port = bridge::int_param_or(p, "bind_port", 0);
    int bound = rt.add_tcp_server_interface(name, port);
    return bridge::json{
        {"port", bound},
        {"interface_name", name},
    };
})

REGISTER_COMMAND(lxmf_add_tcp_client_interface, {
    auto& rt = bridge::Runtime::instance();
    std::string name = bridge::str_param_or(p, "name", "tcp_client");
    std::string host = bridge::str_param_or(p, "target_host", "127.0.0.1");
    int port = bridge::int_param(p, "target_port");
    rt.add_tcp_client_interface(name, host, port);
    return bridge::json{{"interface_name", name}};
})

REGISTER_COMMAND(lxmf_announce, {
    (void)p;
    auto& rt = bridge::Runtime::instance();
    rt.announce();
    return bridge::json{
        {"delivery_destination_hash",
         bridge::to_hex(from_rns(rt.delivery_destination_hash()))},
    };
})

REGISTER_COMMAND(lxmf_send_opportunistic, {
    auto& rt = bridge::Runtime::instance();
    auto dest_hash = bridge::hex_param(p, "destination_hash");
    std::string content = bridge::str_param(p, "content");
    std::string title = bridge::str_param_or(p, "title", "");
    auto hash = rt.send_opportunistic(to_rns(dest_hash), content, title);
    return bridge::json{{"message_hash", bridge::to_hex(from_rns(hash))}};
})

REGISTER_COMMAND(lxmf_get_received_messages, {
    auto& rt = bridge::Runtime::instance();
    uint64_t since_seq = bridge::int_param_or(p, "since_seq", 0);
    uint64_t last_seq = 0;
    auto msgs = rt.get_received_messages(since_seq, last_seq);
    bridge::json arr = bridge::json::array();
    for (const auto& m : msgs) {
        arr.push_back(bridge::json{
            {"seq", m.seq},
            {"message_hash", bridge::to_hex(from_rns(m.message_hash))},
            {"source_hash", bridge::to_hex(from_rns(m.source_hash))},
            {"destination_hash", bridge::to_hex(from_rns(m.destination_hash))},
            {"title", m.title},
            {"content", m.content},
            {"method", m.method},
            {"ack_status", m.ack_status},
            {"received_at_ms", m.received_at_ms},
        });
    }
    return bridge::json{
        {"messages", arr},
        {"last_seq", last_seq},
    };
})

REGISTER_COMMAND(lxmf_get_message_state, {
    auto& rt = bridge::Runtime::instance();
    auto hash = bridge::hex_param(p, "message_hash");
    return bridge::json{{"state", rt.get_message_state(to_rns(hash))}};
})

REGISTER_COMMAND(lxmf_shutdown, {
    (void)p;
    auto& rt = bridge::Runtime::instance();
    if (rt.is_initialized()) rt.shutdown();
    return bridge::json{{"stopped", true}};
})

REGISTER_COMMAND(ping, {
    (void)p;
    return bridge::json{{"pong", true}};
})
