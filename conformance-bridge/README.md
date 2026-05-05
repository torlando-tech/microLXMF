# microLXMF conformance bridge

JSON-RPC stdio bridge for [lxmf-conformance](https://github.com/torlando-tech/lxmf-conformance), mirroring the swift bridge at `LXMF-swift/Sources/LXMFConformanceBridge/main.swift` and the kotlin bridge at `LXMF-kt/conformance-bridge/`.

## Status

**Phase 1 — functionally interoperable on the announce + opportunistic surface.**

| Test suite | Results | Notes |
|---|---|---|
| `test_announce_discovery` | **4/4 pass** (all 4 cross-impl pairs) | Proves wire-format parity for LXMF announces |
| `test_opportunistic` | **4/4 pass** (all 4 cross-impl pairs) | Proves single-packet LXMF encrypt/decrypt + proof handling end-to-end |
| `test_direct` | 2/4 pass (`python→*`) | `microlxmf→*` fails: pre-existing Link layer bug in microLXMF |
| `test_direct_large` | 1/4 pass (`python→python` only) | Resource-over-Link transfer broken in microLXMF |
| `test_attachments` | not yet | Needs `fields` parameter support in send commands |
| `test_combined` | not yet | Needs `fields` parameter support |
| `test_propagation` | not yet | Needs `lxmd` propagation node integration + `lxmf_sync_inbound` |

The 8 cross-impl announce + opportunistic pairs are the canonical interop proof per the [lxmf-conformance README](https://github.com/torlando-tech/lxmf-conformance#phase-1-status). microLXMF's matrix on this surface is **strictly stronger than the swift bridge's** (which currently has `swift→python` and `swift→swift` failing).

## Build

```bash
cd ~/repos/microLXMF/conformance-bridge
cmake -S . -B build
cmake --build build --target microLXMFBridge
./build/microLXMFBridge
# Should print: READY
# Then accept JSON commands on stdin.
```

Quick smoke test:

```bash
echo '{"id":"1","command":"ping"}' | ./build/microLXMFBridge
# READY
# {"id":"1","result":{"pong":true},"success":true}
```

Run conformance:

```bash
cd ~/repos/lxmf-conformance
pytest tests/test_announce_discovery.py tests/test_opportunistic.py --impls=python,microlxmf -v
# Expect: 8 passed
```

## Architecture

```
┌───────────────────────────────────────────────────┐
│ microLXMFBridge (one process per node)            │
│                                                   │
│ ┌─────────────────────┐   ┌─────────────────────┐ │
│ │ Main thread         │   │ Worker thread       │ │
│ │ - stdin JSON-RPC    │   │ - reticulum.loop()  │ │
│ │ - command registry  │   │ - reticulum.jobs()  │ │
│ │ - mutates Runtime   │◄──│ - router.process_*  │ │
│ │ - writes responses  │   │ - delivers callback │ │
│ └─────────────────────┘   └─────────────────────┘ │
│                                                   │
│ ┌─────────────────────┐   ┌─────────────────────┐ │
│ │ TCP reader thread   │   │ TCP accept thread   │ │
│ │ (per CLIENT iface)  │   │ (per SERVER iface)  │ │
│ │ - HDLC deframe      │   │ - accept + handoff  │ │
│ │ - InterfaceImpl::   │   │   to reader_loop    │ │
│ │   handle_incoming   │   │                     │ │
│ └─────────────────────┘   └─────────────────────┘ │
│                                                   │
│ State (mutex-guarded):                            │
│   Identity, Reticulum, LXMRouter, microStore::FS  │
│   PosixTCPInterface vector                        │
│   InboundQueue<seq, ReceivedMsg>                  │
│   OutboundStateMap<msg_hash, State>               │
└───────────────────────────────────────────────────┘
```

### Stack

| Layer | Source | Notes |
|---|---|---|
| JSON-RPC dispatch | `src/main.cpp`, `src/bridge.{h,cpp}` | Same shape as reticulum-conformance microreticulum bridge |
| Command handlers | `src/commands/lxmf.cpp` | 12 commands incl. `ping`, `lxmf_init`, `lxmf_send_*`, `lxmf_set_outbound_propagation_node` |
| Runtime state | `src/runtime/Runtime.{h,cpp}` | Singleton owning Reticulum + LXMRouter + worker thread |
| TCP transport | `src/runtime/PosixTCPInterface.{h,cpp}` + `HDLC.h` | Single-peer (accept-one) server + client modes |
| microLXMF | `../src/LXMF/` | Linked as static lib `MicroLXMFLib` |
| microReticulum | `torlando-tech/microReticulum:pyxis-fixes-on-0.3.0` via FetchContent | Bundles 3 PR-ready Cryptography fixes |
| microStore | `attermann/microStore:master` via FetchContent | `USTORE_USE_POSIXFS` selects the host backend |
| MsgPack stack | `hideakitai/MsgPack@v0.4.2` + ArxContainer + ArxTypeTraits + DebugLog | Header-only |
| ArduinoJson | `bblanchon/ArduinoJson:v7.4.2` | For LXMF MessageStore JSON-on-disk |
| attermann/Crypto | pinned commit | SHA/HMAC/AES/X25519/Ed25519 — same primitives microReticulum uses |
| TLSF | microReticulum/src/Utilities/tlsf.c | Pool allocator; built standalone since microReticulum's CMake glob misses .c |

### Notable implementation choices

- **Logs to stderr.** microReticulum and microLXMF log via `RNS::set_log_callback`. The bridge protocol requires stdout to be JSON-only, so the callback redirects to stderr and `loglevel = LOG_ERROR` keeps pytest capture small. Override via env if you need debug noise.
- **Single-peer TCP.** Phase-1 lxmf-conformance uses 2-bridge topologies, so a server interface that accepts ONE peer is sufficient. Multi-peer support is straightforward (one `PosixTCPInterface` per accepted peer) but unnecessary for Phase 1.
- **`desired_method` patch.** microLXMF's pyxis-derived `LXMRouter::process_outbound` previously auto-selected OPPORTUNISTIC for any message under 159 bytes (LoRa-friendly default). Patched to respect explicit DIRECT / PROPAGATED requests so cross-impl tests can exercise both paths.
- **Storage layout.** Identity persists at `${storage_path}/identity` (raw 64-byte private key). microStore-backed pieces live at `${storage_path}/storage` and `${storage_path}/cache`. Each bridge invocation in the conformance harness gets its own tmpdir, so the storage path is throwaway.

## Roadmap to remaining tests

Per the matrix above, the gap from "8 pass" to "all pass" requires:

1. **microLXMF Link layer fix.** `microlxmf→*` DIRECT sends fail because the Link-establishment-then-resource-send path doesn't complete on loopback in time. Likely a state-machine ordering bug in `LXMRouter::send_via_link` or upstream `RNS::Link`. Same root cause as `test_direct_large[python→microlxmf]` failing — this is a microLXMF protocol bug, not a bridge bug. Fixing it also helps pyxis's `announces-not-showing` issue (pyxis uses the same Link code).
2. **Fields support.** `LXMessage::fields_set(key_bytes, value_bytes)` is the C++ API; the bridge wire format uses `dict[str-int-key, tagged-value]` (`{"bytes":hex}`/`{"str":...}` etc per `reference/lxmf_python.py::_decode_field_value_from_params`). Need to encode/decode the tagged JSON values into the LXMF msgpack `dict[int, Any]` shape on send, and the inverse on receive.
3. **Propagation.** `lxmd` runs the propagation node externally; microLXMF's role is just to set `_outbound_propagation_node`, generate the propagation stamp, and use `LXMRouter::send_propagated`. The `set_outbound_propagation_node` command handler is in place but the test also requires `lxmf_sync_inbound` (pull queued messages from PN over a Link) — not yet wired.

Each of these is a discrete chunk; the harness round-trip is solid.
