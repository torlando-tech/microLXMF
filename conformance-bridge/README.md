# microLXMF conformance bridge

JSON-RPC stdio bridge for [lxmf-conformance](https://github.com/torlando-tech/lxmf-conformance), mirroring the swift bridge at `LXMF-swift/Sources/LXMFConformanceBridge/main.swift` and the kotlin bridge at `LXMF-kt/conformance-bridge/`.

## Status

**Full conformance suite passing on the python ↔ microlxmf matrix locally** (86/86 in ~7m). Issue #1 (Resource transfer to lxmd doesn't conclude) was resolved transitively by the microReticulum progress-callback wiring fix in PR #2 — all 4 propagation trios pass on a developer machine. Propagation is currently excluded in CI under a slow-runner timing flake (separate from #1); the `python->lxmd_pn->microlxmf` receive-sync variant hangs past pytest's 360s timeout on 2-core GitHub runners. Tracked separately.

| Test suite | Results | Status |
|---|---|---|
| `test_announce_discovery` | **4/4 pass** | Wire-format parity for LXMF announces |
| `test_announce_app_data` | **4/4 pass** | RNS ratchet stripping in `recall_app_data` (cross-impl matrix) |
| `test_opportunistic` | **4/4 pass** | Single-packet encrypt/decrypt + proof handling end-to-end |
| `test_direct` | **4/4 pass** | Link establishment + small-msg link delivery + link proof |
| `test_direct_large` | **4/4 pass** | Resource transfer end-to-end (single-segment), incl. sender `delivered` state |
| `test_attachments` | **3/3 pass** | LXMF fields wire format (`dict[int, Any]`) round-trip |
| `test_combined` | **3/3 pass** | Resource transfer + fields together |
| `test_propagation` | **4/4 pass locally**; CI-excluded | PROPAGATED upload to lxmd + receiver sync — CI flake on slow runners |

## Build

```bash
cd ~/repos/microLXMF/conformance-bridge
cmake -S . -B build
cmake --build build --target microLXMFBridge
./build/microLXMFBridge
# READY → accepts JSON-RPC commands on stdin
```

```bash
cd ~/repos/lxmf-conformance
pytest tests/ --impls=python,microlxmf -v
# Full suite passes locally (including propagation).
# CI excludes test_propagation under a separate slow-runner flake.
```

Optional: `MICROLXMF_BRIDGE_LOGLEVEL=N` env var (0..8) controls bridge stderr verbosity (CRITICAL..TRACE). Default ERROR keeps pytest capture small.

## Bugs surfaced + fixed during conformance bring-up

The conformance harness drove out 7 substantive bugs in microReticulum, microLXMF, and the bridge runtime — all listed below are fixed:

1. **Path table never initialized.** `Reticulum::transport_enabled` was off (pyxis's leaf-node default), and the gate ALSO blocks `Transport::start` from initializing `_path_store` (microStore-backed path table). Without it, every `_new_path_table.put` on inbound announces returned false silently — no destinations were ever learned. **This is identical to pyxis's runtime "announces don't show in UI" issue** — the conformance harness produced a deterministic reproducer.

2. **Two bridges in same CWD shared identity.** `microStore::Adapters::PosixFileSystem` accepts a `_basepath` constructor arg but never prepends it to `::open`/`::stat`/`::opendir` — every file op happens at process CWD. Two bridges spawned by the conformance harness shared CWD, so they shared identity files, path stores, message stores. Workaround: per-process `mktemp` tmpdir + `chdir` into it on `lxmf_init`.

3. **Deterministic identity generation.** `attermann/Crypto`'s `RNG.begin("Reticulum")` is fully deterministic on host (ChaCha20 seeded with a hardcoded constant + tag string, no `/dev/urandom` mixing on non-Arduino builds). Two bridge processes generated identical X25519+Ed25519 keypairs, collided identity hashes. Bypass RNG entirely for the initial keypair: read 64 bytes from `/dev/urandom` and pass to `Identity::load_private_key`.

4. **Link-proof delivery callback hardcoded off.** `PacketReceipt::validate_link_proof` had its `link.validate(signature, _hash)` call commented out (replaced with `if (false)`). DIRECT-via-link delivery proofs were received, hash-matched, signature-extracted, then the callback was silently dropped. Restored — fired on signature pass.

5. **Direct-link PACKET path didn't register proof callback.** `LXMRouter::send_via_link`'s small-message PACKET branch sent the packet, transitioned state to SENT, never registered a `PacketReceipt::set_delivery_callback`. Result: sender stayed in SENT forever even after the receiver acked. Added the proof tracking that matched the OPPORTUNISTIC path.

6. **Resource-concluded callback was a no-op.** `static_resource_concluded_callback` in `LXMRouter.cpp` was a stub that just logged an error (the pyxis fork's pre-graft version used `Resource::link()` to find the owning router; vanilla 0.3.0 doesn't expose that getter). Implemented for the single-router-per-process case by iterating the router registry and dispatching to the first registered router's `on_resource_concluded`.

7. **Inbound message seq race.** `Runtime::on_delivery` advanced the seq counter under one mutex, then pushed the message under another. A concurrent `lxmf_get_received_messages(since_seq=N)` could observe `last_seq=N` but find `_inbound` empty, then skip seq=N on the next drain. Combined into a single locked block.

## Known gaps

- **CI flake on `python->lxmd_pn->microlxmf` propagation receive-sync.** Passes locally 4/4 (~30s/trio); on 2-core GitHub-hosted runners the bridge's receive-side sync hangs past pytest's 360s timeout. Suspected CPU-bound race in the worker loop's interaction with RNS Transport / Link callbacks under load. Investigated separately from issue #1 (which was about correctness — that's resolved). CI currently excludes `tests/test_propagation.py`.

## Architecture

```
┌───────────────────────────────────────────────────┐
│ microLXMFBridge (one process per node)            │
│                                                   │
│ Main thread (JSON-RPC dispatch) ⇄ Worker thread   │
│      ┌──────────────────┐    (drives             │
│      │ command registry │    Reticulum.loop /    │
│      └──────────────────┘    jobs / process_*)   │
│                                                   │
│ TCP reader threads (per CLIENT iface)             │
│ TCP accept thread → reader (per SERVER iface)     │
│                                                   │
│ State (mutex-guarded):                            │
│   Identity, Reticulum, LXMRouter, microStore::FS  │
│   PosixTCPInterface vector                        │
│   InboundQueue<seq, ReceivedMsg>                  │
│   OutboundStateMap<msg_hash, State>               │
└───────────────────────────────────────────────────┘
```

## Stack

| Layer | Source | Notes |
|---|---|---|
| JSON-RPC dispatch | `src/main.cpp`, `src/bridge.{h,cpp}` | Same shape as reticulum-conformance microreticulum bridge |
| Command handlers | `src/commands/lxmf.cpp` | 14 commands incl. `ping`, `lxmf_init`, `lxmf_send_*`, `lxmf_request_path`, `lxmf_has_path`, `lxmf_set_outbound_propagation_node` |
| Runtime state | `src/runtime/Runtime.{h,cpp}` | Singleton owning Reticulum + LXMRouter + worker thread |
| TCP transport | `src/runtime/PosixTCPInterface.{h,cpp}` + `HDLC.h` | Single-peer (accept-one) server + client modes |
| microLXMF | `../src/LXMF/` | Linked as static lib `MicroLXMFLib` |
| microReticulum | `torlando-tech/microReticulum:pyxis-fixes-on-0.3.0` via FetchContent | 4 PR-ready Cryptography + 1 link-proof fix |
| microStore | `attermann/microStore:master` | `USTORE_USE_POSIXFS` selects host backend |
| MsgPack stack | `hideakitai/MsgPack@v0.4.2` + ArxContainer + ArxTypeTraits + DebugLog | Header-only |
| ArduinoJson | `bblanchon/ArduinoJson:v7.4.2` | LXMF MessageStore JSON-on-disk |
| attermann/Crypto | pinned commit | SHA / HMAC / AES / X25519 / Ed25519 |
| TLSF | microReticulum/src/Utilities/tlsf.c | Pool allocator; built standalone since microReticulum's CMake glob misses .c |
