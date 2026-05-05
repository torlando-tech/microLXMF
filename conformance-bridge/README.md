# microLXMF conformance bridge

JSON-RPC stdio bridge for [lxmf-conformance](https://github.com/torlando-tech/lxmf-conformance), mirroring the swift bridge at `LXMF-swift/Sources/LXMFConformanceBridge/main.swift` and the kotlin bridge at `LXMF-kt/conformance-bridge/`.

## Status

**Phase 0 — skeleton only.** The bridge binary builds, prints `READY`, accepts JSON commands, and dispatches to a command registry. The eight `lxmf_*` command handlers required by the [bridge protocol](https://github.com/torlando-tech/lxmf-conformance/blob/main/README.md#bridge-protocol) currently return `"not implemented yet"`. A `ping` command is wired up as a liveness check.

## Build

```bash
cd ~/repos/microLXMF/conformance-bridge
cmake -S . -B build
cmake --build build
./build/microLXMFBridge
# Should print: READY
# Then accept JSON commands on stdin.
```

Test the skeleton:

```bash
echo '{"id":"1","command":"ping"}' | ./build/microLXMFBridge
# Expected:
# READY
# {"id":"1","result":{"pong":true},"success":true}
```

## Roadmap to an actual interop-ready bridge

The skeleton is in place; the work to make it *functionally interoperate* with the python LXMF reference is non-trivial. In rough order:

1. **microReticulum host build wiring.** Pull in `attermann/Crypto`, `ArduinoJson`, `microStore`, and the microReticulum sources via `FetchContent` (same recipe as `reticulum-conformance/impls/microreticulum/CMakeLists.txt`). microReticulum currently has a `native` PlatformIO env that builds 71/71 unit tests on host, so the core compiles without ESP-IDF — the work is wiring up the include paths + missing transport pieces.
2. **Host FileSystem adapter for microStore.** `microStore::Adapters::SPIFFSFileSystem` is ESP-IDF-only. Need a `POSIXFileSystem` adapter (or an in-memory adapter) so `Identity::recall` / `Identity::save` work on host without SPIFFS.
3. **microLXMF host build.** `MessageStore.cpp` uses `ArduinoJson` (works on host) and `LXStamper.cpp`'s ESP-only includes are already gated by `#ifdef ESP_PLATFORM` — should compile clean once microReticulum host build is sound.
4. **TCP transport.** Phase-1 of lxmf-conformance uses TCP loopback. Pyxis ships a `TCPClientInterface` already POSIX-aware via `#ifdef ARDUINO`; vendor it into this bridge or upstream it into microReticulum proper. **Pyxis has no `TCPServerInterface`** — that's net-new code for the bridge to act as the receiving end of phase-1 tests.
5. **`RNS::Reticulum` singleton + Transport thread.** Spin up a background thread that drives `Transport::process_outbound`/`process_inbound` while the JSON-RPC main thread services commands.
6. **Inbound message queue.** Wire `LXMRouter::register_delivery_callback` to push into an in-memory deque keyed by monotonic seq (matches the bridge protocol's `seq` semantics in `lxmf_get_received_messages`).
7. **Command handlers proper.** `lxmf_init` / `lxmf_announce` / `lxmf_send_opportunistic` / `lxmf_get_received_messages` / `lxmf_get_message_state` / `lxmf_shutdown` (already trivially complete) / both interface commands.

Until at least items 1–7 land, this bridge is observation-grade only.

## Architecture (target shape)

```
┌───────────────────────────────────────────────────┐
│ microLXMFBridge (one process per node)            │
│                                                   │
│ ┌─────────────────────┐   ┌─────────────────────┐ │
│ │ Main thread         │   │ Transport thread    │ │
│ │ - stdin JSON-RPC    │   │ - drives RNS loop   │ │
│ │ - command registry  │   │ - services TCP IF   │ │
│ │ - mutates state     │◄──│ - delivers to       │ │
│ │ - writes responses  │   │   inbound queue     │ │
│ └─────────────────────┘   └─────────────────────┘ │
│                                                   │
│ State: Identity, LXMRouter, [TCPClient/Server]    │
│        InboundQueue<seq, LXMessage>               │
│        OutboundStateMap<msg_hash, State>          │
└───────────────────────────────────────────────────┘
```

## Why this isn't shipped end-to-end yet

The reticulum-conformance microReticulum bridge took multiple iterations because microReticulum's surface area is broad and host-side gaps surface one at a time (PKCS7 padding, HMAC double-feed, X25519 clamping — three upstream Cryptography fixes shipped on `pyxis-fixes-on-0.3.0`). Doing the same exercise for the *transport + link + resource* layers — which the LXMF bridge needs — is a separate multi-day effort. This PR establishes the skeleton; the wiring is queued for follow-up.
