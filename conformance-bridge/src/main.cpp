// JSON-RPC stdio loop for the microLXMF conformance bridge.
//
// Mirrors reticulum-conformance/impls/microreticulum/src/main.cpp. Each
// command handler is registered via REGISTER_COMMAND() in
// commands/<topic>.cpp; the loop dispatches by name.

#include "bridge.h"

#include <Log.h>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

// microReticulum and microLXMF log to a callback. Redirect everything to
// stderr so it doesn't pollute the bridge's stdout JSON-RPC stream.
static void log_to_stderr(const char* msg, RNS::LogLevel level) {
    (void)level;
    std::fprintf(stderr, "%s\n", msg);
}

int main() {
    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf);

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

    std::cout << "READY\n";
    std::cout.flush();

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

        std::cout << response.dump() << "\n";
        std::cout.flush();
    }
    return 0;
}
