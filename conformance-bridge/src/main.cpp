// JSON-RPC stdio loop for the microLXMF conformance bridge.
//
// Mirrors reticulum-conformance/impls/microreticulum/src/main.cpp. Each
// command handler is registered via REGISTER_COMMAND() in
// commands/<topic>.cpp; the loop dispatches by name.

#include "bridge.h"

#include <microReticulum/Log.h>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

// microReticulum and microLXMF log to a callback. Redirect everything to
// stderr so it doesn't pollute the bridge's stdout JSON-RPC stream.
static void log_to_stderr(const char* msg, RNS::LogLevel level) {
    (void)level;
    std::fprintf(stderr, "%s\n", msg);
}

int main() {
    // Preserve the original stdout exclusively for JSONL responses, then
    // redirect process stdout to stderr. Some embedded dependencies bypass
    // the RNS log callback and call printf() directly (occasionally without
    // a trailing newline); allowing those bytes onto the protocol stream can
    // prefix and invalidate an otherwise-correct JSON response.
    const int json_fd = ::dup(STDOUT_FILENO);
    if (json_fd < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        std::fprintf(stderr, "failed to isolate bridge JSON stream\n");
        return 2;
    }
    FILE* json_out = ::fdopen(json_fd, "w");
    if (!json_out) {
        std::fprintf(stderr, "failed to open bridge JSON stream\n");
        ::close(json_fd);
        return 2;
    }

    RNS::set_log_callback(log_to_stderr);
    // Default to ERROR-only on stderr. Bump via MICROLXMF_BRIDGE_LOGLEVEL
    // env var (0=NONE, 1=CRITICAL, 2=ERROR, 3=WARNING, 4=NOTICE, 5=INFO,
    // 6=VERBOSE, 7=DEBUG, 8=TRACE) to debug interop issues.
    {
        const char* env_level = std::getenv("MICROLXMF_BRIDGE_LOGLEVEL");
        RNS::LogLevel lvl = RNS::LOG_ERROR;
        if (env_level) lvl = static_cast<RNS::LogLevel>(std::atoi(env_level));
        RNS::loglevel(lvl);
    }

    std::fprintf(json_out, "READY\n");
    std::fflush(json_out);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::string request_id = "parse_error";
        bridge::json response;

        try {
            auto request = bridge::json::parse(line);
            request_id = request.value("id", std::string("unknown"));
            std::string command = request.at("command").get<std::string>();
            const auto& params = request.contains("params") && !request["params"].is_null()
                ? request["params"]
                : bridge::json::object();

            const auto* handler = bridge::Registry::instance().find(command);
            if (!handler) {
                throw std::runtime_error("Unknown command: " + command);
            }
            auto result = (*handler)(params);

            response = {
                {"id", request_id},
                {"success", true},
                {"result", result},
            };
        } catch (const std::exception& e) {
            response = {
                {"id", request_id},
                {"success", false},
                {"error", e.what()},
            };
        } catch (...) {
            response = {
                {"id", request_id},
                {"success", false},
                {"error", "unknown exception"},
            };
        }

        const std::string encoded = response.dump();
        std::fprintf(json_out, "%s\n", encoded.c_str());
        std::fflush(json_out);
    }
    std::fclose(json_out);
    return 0;
}
