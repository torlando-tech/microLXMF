// LXMF command handlers.
//
// Wired to the bridge::Runtime singleton. Each handler's command name
// matches lxmf-conformance/README.md "Bridge protocol".

#include "../bridge.h"
#include "../runtime/Runtime.h"
#include "../runtime/MsgPackUtil.h"

#include <Bytes.h>

#include <stdexcept>

namespace {

inline RNS::Bytes to_rns(const bridge::Bytes& v) {
    return RNS::Bytes(v.data(), v.size());
}
inline bridge::Bytes from_rns(const RNS::Bytes& b) {
    return bridge::Bytes(b.data(), b.data() + b.size());
}

// Decode the harness's `fields` JSON shape into the FieldList format
// the runtime expects (each entry is (key_msgpack_bytes, value_msgpack_bytes)).
// `fields_param` is a JSON object with int-stringified keys and tagged
// values, e.g. {"5": [{"str":"f"}, {"bytes":"abcd"}]}.
inline bridge::Runtime::FieldList decode_fields(const bridge::json& fields_param) {
    bridge::Runtime::FieldList out;
    if (!fields_param.is_object()) return out;
    bridge::MsgPackEncoder enc;
    for (auto it = fields_param.begin(); it != fields_param.end(); ++it) {
        int64_t k_int = std::stoll(it.key());
        auto k_msgpack = enc.encode_int_key(k_int);
        auto v_msgpack = enc.encode_value(it.value());
        RNS::Bytes k_b(k_msgpack.data(), k_msgpack.size());
        RNS::Bytes v_b(v_msgpack.data(), v_msgpack.size());
        out.push_back({k_b, v_b});
    }
    return out;
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
    auto fields = p.contains("fields") ? decode_fields(p["fields"])
                                       : bridge::Runtime::FieldList();
    auto hash = rt.send_opportunistic(to_rns(dest_hash), content, title, fields);
    return bridge::json{{"message_hash", bridge::to_hex(from_rns(hash))}};
})

REGISTER_COMMAND(lxmf_send_direct, {
    auto& rt = bridge::Runtime::instance();
    auto dest_hash = bridge::hex_param(p, "destination_hash");
    std::string content = bridge::str_param(p, "content");
    std::string title = bridge::str_param_or(p, "title", "");
    auto fields = p.contains("fields") ? decode_fields(p["fields"])
                                       : bridge::Runtime::FieldList();
    auto hash = rt.send_direct(to_rns(dest_hash), content, title, fields);
    return bridge::json{{"message_hash", bridge::to_hex(from_rns(hash))}};
})

REGISTER_COMMAND(lxmf_send_propagated, {
    auto& rt = bridge::Runtime::instance();
    auto dest_hash = bridge::hex_param(p, "destination_hash");
    std::string content = bridge::str_param(p, "content");
    std::string title = bridge::str_param_or(p, "title", "");
    auto fields = p.contains("fields") ? decode_fields(p["fields"])
                                       : bridge::Runtime::FieldList();
    auto hash = rt.send_propagated(to_rns(dest_hash), content, title, fields);
    return bridge::json{{"message_hash", bridge::to_hex(from_rns(hash))}};
})

REGISTER_COMMAND(lxmf_set_outbound_propagation_node, {
    auto& rt = bridge::Runtime::instance();
    auto node_hash = bridge::hex_param(p, "destination_hash");
    int stamp_cost = bridge::int_param_or(p, "stamp_cost", 0);
    rt.set_outbound_propagation_node(to_rns(node_hash),
                                     static_cast<uint8_t>(stamp_cost));
    return bridge::json::object();
})

REGISTER_COMMAND(lxmf_sync_inbound, {
    auto& rt = bridge::Runtime::instance();
    double timeout_sec = 30.0;
    if (p.contains("timeout_sec")) timeout_sec = p["timeout_sec"].get<double>();
    auto result = rt.sync_inbound(timeout_sec);
    return bridge::json{
        {"final_state", result.final_state},
        {"messages_received", result.messages_received},
    };
})

REGISTER_COMMAND(lxmf_request_path, {
    auto& rt = bridge::Runtime::instance();
    auto dest = bridge::hex_param(p, "destination_hash");
    rt.request_path(to_rns(dest));
    return bridge::json::object();
})

REGISTER_COMMAND(lxmf_has_path, {
    auto& rt = bridge::Runtime::instance();
    auto dest = bridge::hex_param(p, "destination_hash");
    return bridge::json{{"has_path", rt.has_path(to_rns(dest))}};
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
            {"fields", m.fields},
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
