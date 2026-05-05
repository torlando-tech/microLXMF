#include "bridge.h"

#include <stdexcept>
#include <sstream>

namespace bridge {

std::string to_hex(const uint8_t* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

std::string to_hex(const Bytes& b) {
    return to_hex(b.data(), b.size());
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

Bytes from_hex(const std::string& s) {
    if (s.size() % 2 != 0) {
        throw std::runtime_error("Hex string has odd length");
    }
    Bytes out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hex_nibble(s[i]);
        int lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("Invalid hex char");
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

Bytes hex_param(const json& p, const char* key) {
    if (!p.contains(key) || p[key].is_null()) {
        throw std::runtime_error(std::string("Missing param: ") + key);
    }
    return from_hex(p[key].get<std::string>());
}

Bytes hex_param_or_empty(const json& p, const char* key) {
    if (!p.contains(key) || p[key].is_null()) return Bytes{};
    return from_hex(p[key].get<std::string>());
}

int int_param(const json& p, const char* key) {
    if (!p.contains(key) || p[key].is_null()) {
        throw std::runtime_error(std::string("Missing param: ") + key);
    }
    return p[key].get<int>();
}

int int_param_or(const json& p, const char* key, int def) {
    if (!p.contains(key) || p[key].is_null()) return def;
    return p[key].get<int>();
}

std::string str_param(const json& p, const char* key) {
    if (!p.contains(key) || p[key].is_null()) {
        throw std::runtime_error(std::string("Missing param: ") + key);
    }
    return p[key].get<std::string>();
}

std::string str_param_or(const json& p, const char* key, const std::string& def) {
    if (!p.contains(key) || p[key].is_null()) return def;
    return p[key].get<std::string>();
}

bool bool_param(const json& p, const char* key) {
    if (!p.contains(key) || p[key].is_null()) {
        throw std::runtime_error(std::string("Missing param: ") + key);
    }
    return p[key].get<bool>();
}

}  // namespace bridge
