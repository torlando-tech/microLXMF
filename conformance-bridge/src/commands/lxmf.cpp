// LXMF command handlers — STUBS.
//
// Phase-1 (per lxmf-conformance/README.md "Bridge protocol"):
//   lxmf_init, lxmf_add_tcp_server_interface, lxmf_add_tcp_client_interface,
//   lxmf_announce, lxmf_send_opportunistic, lxmf_get_received_messages,
//   lxmf_get_message_state, lxmf_shutdown
//
// All handlers below currently throw "not implemented" — the goal of this
// commit is to land the bridge skeleton (CMake build, JSON-RPC dispatch,
// command registry) so wiring to a real LXMRouter + microReticulum
// transport stack can land in a follow-up. See README.md "Roadmap".

#include "../bridge.h"

#include <stdexcept>

REGISTER_COMMAND(lxmf_init, {
    (void)p;
    throw std::runtime_error(
        "lxmf_init: not implemented yet. The microLXMF bridge skeleton is "
        "in place; runtime wiring (Reticulum singleton, Identity persistence, "
        "LXMRouter instantiation, host POSIX FileSystem adapter) is the next "
        "milestone. See microLXMF/conformance-bridge/README.md.");
})

REGISTER_COMMAND(lxmf_add_tcp_server_interface, {
    (void)p;
    throw std::runtime_error("lxmf_add_tcp_server_interface: not implemented yet");
})

REGISTER_COMMAND(lxmf_add_tcp_client_interface, {
    (void)p;
    throw std::runtime_error("lxmf_add_tcp_client_interface: not implemented yet");
})

REGISTER_COMMAND(lxmf_announce, {
    (void)p;
    throw std::runtime_error("lxmf_announce: not implemented yet");
})

REGISTER_COMMAND(lxmf_send_opportunistic, {
    (void)p;
    throw std::runtime_error("lxmf_send_opportunistic: not implemented yet");
})

REGISTER_COMMAND(lxmf_get_received_messages, {
    (void)p;
    throw std::runtime_error("lxmf_get_received_messages: not implemented yet");
})

REGISTER_COMMAND(lxmf_get_message_state, {
    (void)p;
    throw std::runtime_error("lxmf_get_message_state: not implemented yet");
})

REGISTER_COMMAND(lxmf_shutdown, {
    (void)p;
    return bridge::json{{"stopped", true}};
})

// Smoke / health-check handler — useful for the lxmf-conformance harness to
// verify the bridge process is alive without doing any LXMF work.
REGISTER_COMMAND(ping, {
    (void)p;
    return bridge::json{{"pong", true}};
})
