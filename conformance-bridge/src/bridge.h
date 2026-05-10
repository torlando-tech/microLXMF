// microLXMF bridge — JSON-RPC stdio harness for lxmf-conformance.
//
// Each bridge process represents one running LXMF node. Unlike the
// reticulum-conformance bridge (which is stateless and exposes byte-level
// crypto primitives), this bridge is *stateful*: it owns an Identity, an
// LXMRouter, network interfaces, and an inbound message queue.
//
// Protocol matches lxmf-conformance/README.md "Bridge protocol":
//   - Print exactly "READY\n" on stdout when ready
//   - Read newline-delimited JSON request lines from stdin
//   - Write newline-delimited JSON response lines to stdout
//   - One request, one response. Binary fields are lowercase hex.
//   - Errors throw and become {"success": false, "error": ...}.

#pragma once

#include "json.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace bridge {

using nlohmann::json;
using Bytes = std::vector<uint8_t>;
using Handler = std::function<json(const json&)>;

class Registry {
public:
    static Registry& instance() {
        static Registry r;
        return r;
    }

    void add(const std::string& name, Handler handler) {
        _commands[name] = std::move(handler);
    }

    const Handler* find(const std::string& name) const {
        auto it = _commands.find(name);
        return it == _commands.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, Handler> _commands;
};

#define REGISTER_COMMAND(name, ...)                                            \
    namespace {                                                                \
        struct _reg_##name {                                                   \
            _reg_##name() {                                                    \
                ::bridge::Registry::instance().add(                            \
                    #name,                                                     \
                    [](const ::bridge::json& p) -> ::bridge::json __VA_ARGS__  \
                );                                                             \
            }                                                                  \
        };                                                                     \
        static _reg_##name _reg_##name##_instance;                             \
    }

// Hex helpers
std::string to_hex(const Bytes& b);
std::string to_hex(const uint8_t* data, size_t n);
Bytes from_hex(const std::string& s);

// Param accessors. Throw std::runtime_error("Missing param: <key>") if absent.
Bytes hex_param(const json& p, const char* key);
Bytes hex_param_or_empty(const json& p, const char* key);
int int_param(const json& p, const char* key);
int int_param_or(const json& p, const char* key, int def);
std::string str_param(const json& p, const char* key);
std::string str_param_or(const json& p, const char* key, const std::string& def);
bool bool_param(const json& p, const char* key);
double double_param_or(const json& p, const char* key, double def);

}  // namespace bridge
