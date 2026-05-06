// Hand-written msgpack encoder/decoder for the bridge's `fields` wire
// format conversion (tagged JSON ↔ msgpack bytes).
//
// LXMF on the wire stores fields as msgpack `dict[int, Any]`. The bridge
// JSON-RPC API uses tagged objects per `lxmf-conformance/reference/
// lxmf_python.py::_decode_field_value_from_params`:
//   {"bytes": "<hex>"} | {"str": "..."} | {"int": N} | {"bool": true|false}
//   plus JSON arrays for lists (recursive), bare JSON primitives passthrough.
// Bridge converts on send side (encode_value) and receive side
// (decode_value).

#pragma once

#include "../json.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bridge {

using json = nlohmann::json;

// Hex helpers — local copy so we don't transitively need bridge.h.
inline std::string mp_to_hex(const uint8_t* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}
inline std::vector<uint8_t> mp_from_hex(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("Invalid hex");
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

// ============================================================================
// Encoder: tagged JSON → msgpack bytes
// ============================================================================
class MsgPackEncoder {
public:
    std::vector<uint8_t> encode_value(const json& v) {
        std::vector<uint8_t> out;
        encode_into(v, out);
        return out;
    }

    std::vector<uint8_t> encode_int_key(int64_t k) {
        std::vector<uint8_t> out;
        encode_int(k, out);
        return out;
    }

private:
    static void put_u8(std::vector<uint8_t>& out, uint8_t b) { out.push_back(b); }
    static void put_be16(std::vector<uint8_t>& out, uint16_t v) {
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)v);
    }
    static void put_be32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back((uint8_t)(v >> 24));
        out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)v);
    }
    static void put_be64(std::vector<uint8_t>& out, uint64_t v) {
        for (int i = 7; i >= 0; --i) out.push_back((uint8_t)(v >> (i * 8)));
    }

    void encode_into(const json& v, std::vector<uint8_t>& out) {
        if (v.is_null()) { put_u8(out, 0xc0); return; }
        if (v.is_boolean()) { put_u8(out, v.get<bool>() ? 0xc3 : 0xc2); return; }
        if (v.is_object()) {
            if (v.contains("bytes")) {
                auto bs = mp_from_hex(v["bytes"].get<std::string>());
                encode_bin(bs.data(), bs.size(), out);
                return;
            }
            if (v.contains("str")) { encode_str(v["str"].get<std::string>(), out); return; }
            if (v.contains("int")) { encode_int(v["int"].get<int64_t>(), out); return; }
            if (v.contains("bool")) {
                put_u8(out, v["bool"].get<bool>() ? 0xc3 : 0xc2);
                return;
            }
            // Untagged map: keys-as-strings.
            size_t n = v.size();
            encode_map_size(n, out);
            for (auto it = v.begin(); it != v.end(); ++it) {
                encode_str(it.key(), out);
                encode_into(it.value(), out);
            }
            return;
        }
        if (v.is_array()) {
            size_t n = v.size();
            encode_array_size(n, out);
            for (const auto& e : v) encode_into(e, out);
            return;
        }
        if (v.is_string()) { encode_str(v.get<std::string>(), out); return; }
        if (v.is_number_integer()) { encode_int(v.get<int64_t>(), out); return; }
        if (v.is_number_float()) {
            put_u8(out, 0xcb);
            uint64_t bits;
            double d = v.get<double>();
            std::memcpy(&bits, &d, sizeof(bits));
            put_be64(out, bits);
            return;
        }
        throw std::runtime_error("Unsupported JSON type in field value");
    }

    void encode_int(int64_t n, std::vector<uint8_t>& out) {
        if (n >= 0 && n <= 127) { put_u8(out, (uint8_t)n); return; }
        if (n >= -32 && n < 0) { put_u8(out, (uint8_t)(0xe0 | (uint8_t)(n & 0x1f))); return; }
        if (n >= 0 && n <= 0xff) { put_u8(out, 0xcc); put_u8(out, (uint8_t)n); return; }
        if (n >= 0 && n <= 0xffff) { put_u8(out, 0xcd); put_be16(out, (uint16_t)n); return; }
        if (n >= 0 && n <= 0xffffffffLL) { put_u8(out, 0xce); put_be32(out, (uint32_t)n); return; }
        if (n >= 0) { put_u8(out, 0xcf); put_be64(out, (uint64_t)n); return; }
        if (n >= -128) { put_u8(out, 0xd0); put_u8(out, (uint8_t)n); return; }
        if (n >= -32768) { put_u8(out, 0xd1); put_be16(out, (uint16_t)n); return; }
        if (n >= -2147483648LL) { put_u8(out, 0xd2); put_be32(out, (uint32_t)n); return; }
        put_u8(out, 0xd3); put_be64(out, (uint64_t)n);
    }

    void encode_str(const std::string& s, std::vector<uint8_t>& out) {
        size_t n = s.size();
        if (n <= 31) put_u8(out, (uint8_t)(0xa0 | n));
        else if (n <= 0xff) { put_u8(out, 0xd9); put_u8(out, (uint8_t)n); }
        else if (n <= 0xffff) { put_u8(out, 0xda); put_be16(out, (uint16_t)n); }
        else { put_u8(out, 0xdb); put_be32(out, (uint32_t)n); }
        out.insert(out.end(), s.begin(), s.end());
    }

    void encode_bin(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
        if (n <= 0xff) { put_u8(out, 0xc4); put_u8(out, (uint8_t)n); }
        else if (n <= 0xffff) { put_u8(out, 0xc5); put_be16(out, (uint16_t)n); }
        else { put_u8(out, 0xc6); put_be32(out, (uint32_t)n); }
        out.insert(out.end(), p, p + n);
    }

    void encode_array_size(size_t n, std::vector<uint8_t>& out) {
        if (n <= 15) put_u8(out, (uint8_t)(0x90 | n));
        else if (n <= 0xffff) { put_u8(out, 0xdc); put_be16(out, (uint16_t)n); }
        else { put_u8(out, 0xdd); put_be32(out, (uint32_t)n); }
    }

    void encode_map_size(size_t n, std::vector<uint8_t>& out) {
        if (n <= 15) put_u8(out, (uint8_t)(0x80 | n));
        else if (n <= 0xffff) { put_u8(out, 0xde); put_be16(out, (uint16_t)n); }
        else { put_u8(out, 0xdf); put_be32(out, (uint32_t)n); }
    }
};


// ============================================================================
// Decoder: msgpack bytes → JSON in lxmf-conformance "inbox" shape
// (per python's _encode_field_value_for_inbox: bytes → hex string,
//  scalars passthrough). For LXMF map keys this returns a plain JSON
// integer; consumers stringify when assembling the outer fields dict.
// ============================================================================
class MsgPackDecoder {
public:
    MsgPackDecoder(const uint8_t* data, size_t len) : _p(data), _end(data + len) {}

    json read_value() {
        if (_p >= _end) throw std::runtime_error("msgpack: truncated");
        uint8_t b = *_p++;
        if (b <= 0x7f) return (int64_t)b;                          // pos fixint
        if ((b & 0xf0) == 0x80) return read_map(b & 0x0f);         // fixmap
        if ((b & 0xf0) == 0x90) return read_array(b & 0x0f);       // fixarray
        if ((b & 0xe0) == 0xa0) return read_str(b & 0x1f);         // fixstr
        if (b >= 0xe0) return (int64_t)(int8_t)b;                  // neg fixint
        switch (b) {
            case 0xc0: return nullptr;
            case 0xc2: return false;
            case 0xc3: return true;
            case 0xc4: return read_bin(read_u8());
            case 0xc5: return read_bin(read_u16());
            case 0xc6: return read_bin(read_u32());
            case 0xca: { uint32_t v = read_u32(); float f; std::memcpy(&f,&v,4); return (double)f; }
            case 0xcb: { uint64_t v = read_u64(); double d; std::memcpy(&d,&v,8); return d; }
            case 0xcc: return (int64_t)read_u8();
            case 0xcd: return (int64_t)read_u16();
            case 0xce: return (int64_t)read_u32();
            case 0xcf: return (int64_t)read_u64();
            case 0xd0: return (int64_t)(int8_t)read_u8();
            case 0xd1: return (int64_t)(int16_t)read_u16();
            case 0xd2: return (int64_t)(int32_t)read_u32();
            case 0xd3: return (int64_t)(int64_t)read_u64();
            case 0xd9: return read_str(read_u8());
            case 0xda: return read_str(read_u16());
            case 0xdb: return read_str(read_u32());
            case 0xdc: return read_array(read_u16());
            case 0xdd: return read_array(read_u32());
            case 0xde: return read_map(read_u16());
            case 0xdf: return read_map(read_u32());
        }
        throw std::runtime_error("msgpack: unsupported type byte 0x" + std::to_string((int)b));
    }

    bool eof() const { return _p >= _end; }

private:
    uint8_t read_u8() {
        if (_p >= _end) throw std::runtime_error("msgpack: trunc u8");
        return *_p++;
    }
    uint16_t read_u16() {
        if (_p + 2 > _end) throw std::runtime_error("msgpack: trunc u16");
        uint16_t v = ((uint16_t)_p[0] << 8) | _p[1];
        _p += 2;
        return v;
    }
    uint32_t read_u32() {
        if (_p + 4 > _end) throw std::runtime_error("msgpack: trunc u32");
        uint32_t v = ((uint32_t)_p[0] << 24) | ((uint32_t)_p[1] << 16)
                   | ((uint32_t)_p[2] << 8) | _p[3];
        _p += 4;
        return v;
    }
    uint64_t read_u64() {
        if (_p + 8 > _end) throw std::runtime_error("msgpack: trunc u64");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | _p[i];
        _p += 8;
        return v;
    }
    json read_str(size_t n) {
        if (_p + n > _end) throw std::runtime_error("msgpack: trunc str");
        std::string s((const char*)_p, n);
        _p += n;
        return s;  // bare JSON string (untagged)
    }
    json read_bin(size_t n) {
        if (_p + n > _end) throw std::runtime_error("msgpack: trunc bin");
        std::string hex = mp_to_hex(_p, n);
        _p += n;
        return hex;  // python's _encode_field_value_for_inbox emits bytes as
                     // bare hex string in inbox shape
    }
    json read_array(size_t n) {
        json arr = json::array();
        for (size_t i = 0; i < n; ++i) arr.push_back(read_value());
        return arr;
    }
    json read_map(size_t n) {
        json obj = json::object();
        for (size_t i = 0; i < n; ++i) {
            json k = read_value();
            json v = read_value();
            std::string ks;
            if (k.is_string()) ks = k.get<std::string>();
            else if (k.is_number_integer()) ks = std::to_string(k.get<int64_t>());
            else ks = k.dump();
            obj[ks] = v;
        }
        return obj;
    }

    const uint8_t* _p;
    const uint8_t* _end;
};

}  // namespace bridge
