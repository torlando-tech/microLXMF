// LXMF command handlers.
//
// Wired to the bridge::Runtime singleton. Each handler's command name
// matches lxmf-conformance/README.md "Bridge protocol".

#include "../bridge.h"
#include "../runtime/Runtime.h"
#include "../runtime/MsgPackUtil.h"

#include <Bytes.h>
#include <Identity.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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
    double timestamp = bridge::double_param_or(p, "timestamp", 0.0);
    auto hash = rt.send_opportunistic(to_rns(dest_hash), content, title, fields, timestamp);
    return bridge::json{{"message_hash", bridge::to_hex(from_rns(hash))}};
})

REGISTER_COMMAND(lxmf_send_direct, {
    auto& rt = bridge::Runtime::instance();
    auto dest_hash = bridge::hex_param(p, "destination_hash");
    std::string content = bridge::str_param(p, "content");
    std::string title = bridge::str_param_or(p, "title", "");
    auto fields = p.contains("fields") ? decode_fields(p["fields"])
                                       : bridge::Runtime::FieldList();
    double timestamp = bridge::double_param_or(p, "timestamp", 0.0);
    auto hash = rt.send_direct(to_rns(dest_hash), content, title, fields, timestamp);
    return bridge::json{{"message_hash", bridge::to_hex(from_rns(hash))}};
})

REGISTER_COMMAND(lxmf_send_propagated, {
    auto& rt = bridge::Runtime::instance();
    auto dest_hash = bridge::hex_param(p, "destination_hash");
    std::string content = bridge::str_param(p, "content");
    std::string title = bridge::str_param_or(p, "title", "");
    auto fields = p.contains("fields") ? decode_fields(p["fields"])
                                       : bridge::Runtime::FieldList();
    double timestamp = bridge::double_param_or(p, "timestamp", 0.0);
    auto hash = rt.send_propagated(to_rns(dest_hash), content, title, fields, timestamp);
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

// Return the raw app_data bytes (hex) most recently learned for this
// destination via Identity::recall_app_data. Used by the
// test_announce_app_data conformance test to verify microReticulum
// strips the 32-byte X25519 ratchet prefix from announces with
// `packet.context_flag == FLAG_SET`. If the ratchet leaked into
// app_data, byte 0 would be a random byte instead of an msgpack
// array marker (0x90-0x9f or 0xdc).
REGISTER_COMMAND(lxmf_recall_app_data, {
    auto dest = bridge::hex_param(p, "destination_hash");
    RNS::Bytes app_data = RNS::Identity::recall_app_data(to_rns(dest));
    return bridge::json{
        {"size", (uint64_t)app_data.size()},
        {"hex",  bridge::to_hex(from_rns(app_data))},
    };
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

// Returns the resource-transfer progress for the given message in
// [0.0, 1.0], or -1.0 if no progress has been recorded for it (the
// PACKET path doesn't tick progress; small messages go straight from
// SENT to DELIVERED). Mirrors python LXMF's `LXMessage.progress`.
REGISTER_COMMAND(lxmf_get_message_progress, {
    auto& rt = bridge::Runtime::instance();
    auto hash = bridge::hex_param(p, "message_hash");
    return bridge::json{{"progress", rt.get_message_progress(to_rns(hash))}};
})

// =====================================================================
// lxmf_decode_bytes — pure byte-level decoder, no router required.
//
// Mirrors the python reference command in lxmf-conformance/reference/
// lxmf_python.py:cmd_lxmf_decode_bytes. Used by tests/test_payload_format.py
// to round-trip wire-format payload shapes (4-element vs 5-element with
// stamp, nil fields, non-UTF-8 title/content, msgpack edge cases).
//
// Key contract differences from LXMessage::unpack_from_bytes:
//   1. Does NOT validate the signature — bytes can be crafted with a
//      dummy signature for protocol-shape testing.
//   2. NEVER throws — returns {"decode_error": <message>} on any failure
//      (truncated header, msgpack parse failure, wrong shape, etc).
//   3. Surfaces fields_was_nil distinctly from fields_count = 0.
//
// We hand-roll the msgpack walk rather than reusing LXMessage's decoder
// because:
//   - The decoder throws on errors; tests want structured error strings.
//   - Tests poke nil-fields and 5-element-with-stamp cases that need
//     special handling at re-pack-for-hash time.
// =====================================================================

// Skip a single msgpack value at `cursor`. Recurses through nested
// arrays/maps. On success advances cursor past the value; on failure
// leaves err populated. File-scope static (the REGISTER_COMMAND macro
// already wraps each command in `namespace { ... }` so we can't nest
// our own anonymous namespace here).
static bool mp_skip(const uint8_t* buf, size_t buf_size,
                    size_t& cursor, std::string& err) {
    if (cursor >= buf_size) { err = "truncated: skip past end"; return false; }
    uint8_t b = buf[cursor++];
    auto read_be = [&](size_t n, uint64_t& out) -> bool {
        if (cursor + n > buf_size) { err = "truncated: int payload"; return false; }
        out = 0;
        for (size_t i = 0; i < n; ++i) out = (out << 8) | buf[cursor++];
        return true;
    };
    // fixint / nil / bool
    if (b <= 0x7f || b >= 0xe0) return true;
    if (b == 0xc0 || b == 0xc2 || b == 0xc3) return true;
    // fixstr
    if ((b & 0xe0) == 0xa0) {
        size_t n = b & 0x1f;
        if (cursor + n > buf_size) { err = "truncated: fixstr body"; return false; }
        cursor += n;
        return true;
    }
    // fixarray / fixmap
    if ((b & 0xf0) == 0x90) {
        size_t n = b & 0x0f;
        for (size_t i = 0; i < n; ++i) if (!mp_skip(buf, buf_size, cursor, err)) return false;
        return true;
    }
    if ((b & 0xf0) == 0x80) {
        size_t n = b & 0x0f;
        for (size_t i = 0; i < 2 * n; ++i) if (!mp_skip(buf, buf_size, cursor, err)) return false;
        return true;
    }
    uint64_t len = 0;
    switch (b) {
        case 0xcc: case 0xd0: return read_be(1, len);
        case 0xcd: case 0xd1: return read_be(2, len);
        case 0xce: case 0xd2: return read_be(4, len);
        case 0xcf: case 0xd3: return read_be(8, len);
        case 0xca: cursor += 4; if (cursor > buf_size) { err = "truncated: float32"; return false; } return true;
        case 0xcb: cursor += 8; if (cursor > buf_size) { err = "truncated: float64"; return false; } return true;
        case 0xc4: case 0xd9: if (!read_be(1, len)) return false; if (cursor + len > buf_size) { err = "truncated: bin8/str8"; return false; } cursor += len; return true;
        case 0xc5: case 0xda: if (!read_be(2, len)) return false; if (cursor + len > buf_size) { err = "truncated: bin16/str16"; return false; } cursor += len; return true;
        case 0xc6: case 0xdb: if (!read_be(4, len)) return false; if (cursor + len > buf_size) { err = "truncated: bin32/str32"; return false; } cursor += len; return true;
        case 0xdc: if (!read_be(2, len)) return false; for (uint64_t i = 0; i < len; ++i) if (!mp_skip(buf, buf_size, cursor, err)) return false; return true;
        case 0xdd: if (!read_be(4, len)) return false; for (uint64_t i = 0; i < len; ++i) if (!mp_skip(buf, buf_size, cursor, err)) return false; return true;
        case 0xde: if (!read_be(2, len)) return false; for (uint64_t i = 0; i < 2 * len; ++i) if (!mp_skip(buf, buf_size, cursor, err)) return false; return true;
        case 0xdf: if (!read_be(4, len)) return false; for (uint64_t i = 0; i < 2 * len; ++i) if (!mp_skip(buf, buf_size, cursor, err)) return false; return true;
        default:
            err = std::string("unsupported msgpack byte 0x") +
                  std::string(1, "0123456789abcdef"[(b >> 4) & 0xf]) +
                  std::string(1, "0123456789abcdef"[b & 0xf]);
            return false;
    }
}

// Hex character → nibble. Returns -1 on non-hex.
static int hex_nyb(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Decode hex string into a byte vector. On failure clears `out` and
// sets `err`.
static std::vector<uint8_t> hex_decode(const std::string& hex, std::string& err) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) { err = "odd-length input"; return out; }
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = hex_nyb(hex[i]);
        int lo = hex_nyb(hex[i + 1]);
        if (hi < 0 || lo < 0) { err = "non-hex char"; out.clear(); return out; }
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

// Bytes → lowercase hex string.
static std::string bytes_to_hex(const uint8_t* data, size_t n) {
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(HEX[(data[i] >> 4) & 0xf]);
        out.push_back(HEX[data[i] & 0xf]);
    }
    return out;
}

// Read a msgpack bin element starting at cursor. Returns "" on success
// and populates `out_hex`; returns non-empty error string otherwise.
// Treats msgpack Nil (0xc0) as an empty bin (matches python's
// fallback `if x is None: x = b""` for title/content).
static std::string mp_read_bin_hex(const uint8_t* buf, size_t buf_size,
                                   size_t& cursor, std::string& out_hex,
                                   const char* label) {
    if (cursor >= buf_size) return std::string("truncated: ") + label + " header";
    uint8_t h = buf[cursor++];
    if (h == 0xc0) { out_hex.clear(); return ""; }
    size_t len = 0;
    if (h == 0xc4) {
        if (cursor + 1 > buf_size) return std::string("truncated: ") + label + " bin8 size";
        len = buf[cursor++];
    } else if (h == 0xc5) {
        if (cursor + 2 > buf_size) return std::string("truncated: ") + label + " bin16 size";
        len = ((size_t)buf[cursor] << 8) | buf[cursor + 1];
        cursor += 2;
    } else if (h == 0xc6) {
        if (cursor + 4 > buf_size) return std::string("truncated: ") + label + " bin32 size";
        len = ((size_t)buf[cursor] << 24) | ((size_t)buf[cursor + 1] << 16) |
              ((size_t)buf[cursor + 2] << 8) | buf[cursor + 3];
        cursor += 4;
    } else {
        return std::string(label) + " not a msgpack bin";
    }
    if (cursor + len > buf_size) return std::string("truncated: ") + label + " body";
    out_hex = bytes_to_hex(buf + cursor, len);
    cursor += len;
    return "";
}

REGISTER_COMMAND(lxmf_decode_bytes, {
    constexpr size_t DEST_LEN = 16;
    constexpr size_t SIG_LEN = 64;

    std::string hex_str = bridge::str_param(p, "lxmf_bytes");
    std::string err;
    std::vector<uint8_t> bytes = hex_decode(hex_str, err);
    if (!err.empty()) return bridge::json{{"decode_error", "hex parse: " + err}};

    if (bytes.size() < 2 * DEST_LEN + SIG_LEN) {
        return bridge::json{{"decode_error",
            "lxmf_bytes too short: " + std::to_string(bytes.size())}};
    }

    std::string dest_hex = bytes_to_hex(bytes.data(), DEST_LEN);
    std::string source_hex = bytes_to_hex(bytes.data() + DEST_LEN, DEST_LEN);
    std::string sig_hex = bytes_to_hex(bytes.data() + 2 * DEST_LEN, SIG_LEN);

    const uint8_t* payload = bytes.data() + 2 * DEST_LEN + SIG_LEN;
    size_t payload_size = bytes.size() - 2 * DEST_LEN - SIG_LEN;
    if (payload_size == 0) return bridge::json{{"decode_error", "payload is empty"}};

    size_t cursor = 0;
    uint8_t hdr = payload[cursor++];
    size_t arr_size = 0;
    size_t orig_hdr_len = 1;
    if ((hdr & 0xf0) == 0x90) {
        arr_size = hdr & 0x0f;
    } else if (hdr == 0xdc) {
        if (cursor + 2 > payload_size) return bridge::json{{"decode_error", "truncated: array16 size"}};
        arr_size = ((size_t)payload[cursor] << 8) | payload[cursor + 1];
        cursor += 2;
        orig_hdr_len = 3;
    } else if (hdr == 0xdd) {
        if (cursor + 4 > payload_size) return bridge::json{{"decode_error", "truncated: array32 size"}};
        arr_size = ((size_t)payload[cursor] << 24) | ((size_t)payload[cursor + 1] << 16) |
                   ((size_t)payload[cursor + 2] << 8) | payload[cursor + 3];
        cursor += 4;
        orig_hdr_len = 5;
    } else {
        return bridge::json{{"decode_error", "payload not a msgpack array"}};
    }
    if (arr_size < 4) {
        return bridge::json{{"decode_error",
            "payload array too short: " + std::to_string(arr_size)}};
    }

    if (cursor >= payload_size) return bridge::json{{"decode_error", "truncated: timestamp header"}};
    uint8_t ts_hdr = payload[cursor++];
    double timestamp = 0.0;
    if (ts_hdr == 0xcb) {
        if (cursor + 8 > payload_size) return bridge::json{{"decode_error", "truncated: float64 body"}};
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | payload[cursor++];
        memcpy(&timestamp, &bits, 8);
    } else if (ts_hdr == 0xca) {
        if (cursor + 4 > payload_size) return bridge::json{{"decode_error", "truncated: float32 body"}};
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) bits = (bits << 8) | payload[cursor++];
        float f;
        memcpy(&f, &bits, 4);
        timestamp = (double)f;
    } else if (ts_hdr == 0xc0) {
        return bridge::json{{"decode_error", "field extraction: timestamp is nil"}};
    } else {
        return bridge::json{{"decode_error", "timestamp not a float"}};
    }

    std::string title_hex, content_hex;
    err = mp_read_bin_hex(payload, payload_size, cursor, title_hex, "title");
    if (!err.empty()) return bridge::json{{"decode_error", err}};
    err = mp_read_bin_hex(payload, payload_size, cursor, content_hex, "content");
    if (!err.empty()) return bridge::json{{"decode_error", err}};

    bool fields_was_nil = false;
    size_t fields_count = 0;
    if (cursor >= payload_size) return bridge::json{{"decode_error", "truncated: fields header"}};
    uint8_t fields_hdr = payload[cursor];
    if (fields_hdr == 0xc0) {
        fields_was_nil = true;
        cursor++;
    } else if ((fields_hdr & 0xf0) == 0x80) {
        fields_count = fields_hdr & 0x0f;
        cursor++;
    } else if (fields_hdr == 0xde) {
        if (cursor + 3 > payload_size) return bridge::json{{"decode_error", "truncated: map16 size"}};
        fields_count = ((size_t)payload[cursor + 1] << 8) | payload[cursor + 2];
        cursor += 3;
    } else if (fields_hdr == 0xdf) {
        if (cursor + 5 > payload_size) return bridge::json{{"decode_error", "truncated: map32 size"}};
        fields_count = ((size_t)payload[cursor + 1] << 24) | ((size_t)payload[cursor + 2] << 16) |
                       ((size_t)payload[cursor + 3] << 8) | payload[cursor + 4];
        cursor += 5;
    } else {
        return bridge::json{{"decode_error", "fields slot not nil or map"}};
    }
    if (!fields_was_nil) {
        for (size_t i = 0; i < 2 * fields_count; ++i) {
            std::string skip_err;
            if (!mp_skip(payload, payload_size, cursor, skip_err)) {
                return bridge::json{{"decode_error", "fields skip: " + skip_err}};
            }
        }
    }
    size_t fields_end = cursor;

    bridge::json stamp_value = nullptr;
    if (arr_size > 4) {
        std::string stamp_hex;
        err = mp_read_bin_hex(payload, payload_size, cursor, stamp_hex, "stamp");
        if (!err.empty()) return bridge::json{{"decode_error", err}};
        stamp_value = stamp_hex;
    }

    std::vector<uint8_t> hash_input;
    hash_input.insert(hash_input.end(), bytes.begin(), bytes.begin() + 2 * DEST_LEN);
    if (arr_size == 4) {
        hash_input.insert(hash_input.end(), payload, payload + payload_size);
    } else {
        hash_input.push_back(0x94);
        hash_input.insert(hash_input.end(), payload + orig_hdr_len, payload + fields_end);
    }
    RNS::Bytes message_hash = RNS::Identity::full_hash(
        RNS::Bytes(hash_input.data(), hash_input.size()));

    return bridge::json{
        {"destination_hash", dest_hex},
        {"source_hash", source_hex},
        {"signature", sig_hex},
        {"timestamp", timestamp},
        {"title_hex", title_hex},
        {"content_hex", content_hex},
        {"fields_was_nil", fields_was_nil},
        {"fields_count", (int)fields_count},
        {"stamp", stamp_value},
        {"message_hash", bytes_to_hex(message_hash.data(), message_hash.size())},
    };
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
