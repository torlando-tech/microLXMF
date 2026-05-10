# microLXMF

[![Conformance](https://github.com/torlando-tech/microLXMF/actions/workflows/conformance.yml/badge.svg)](https://github.com/torlando-tech/microLXMF/actions/workflows/conformance.yml)

C++ implementation of [LXMF](https://github.com/markqvist/LXMF) (Lightweight Extensible Message Format) layered on top of [attermann/microReticulum](https://github.com/attermann/microReticulum). Sister library to [LXMF-swift](https://github.com/torlando-tech/LXMF-swift) and [LXMF-kt](https://github.com/torlando-tech/LXMF-kt).

Targets ESP32 (Arduino framework) and host (POSIX) builds; the same source tree builds in both.

## Status

Cross-impl conformance against the python LXMF reference passes the announce, opportunistic, direct (small + resource), attachments, and combined surfaces — see [`conformance-bridge/README.md`](conformance-bridge/README.md) for the per-suite breakdown. Propagation-via-`lxmd` is the remaining gap, tracked in [#1](https://github.com/torlando-tech/microLXMF/issues/1).

Sources were extracted from [pyxis](https://github.com/torlando-tech/pyxis)' vendored fork (originally derived from `torlando-tech/microReticulum:feat/t-deck`'s LXMF subtree) and aligned to vanilla `attermann/microReticulum @ 0.3.0`.

## Layout

```
microLXMF/
├── src/
│   └── LXMF/                # public C++ API
│       ├── LXMessage.{h,cpp}
│       ├── LXMRouter.{h,cpp}
│       ├── LXStamper.{h,cpp}
│       ├── MessageStore.{h,cpp}
│       ├── PropagationNodeManager.{h,cpp}
│       └── Type.h
├── conformance-bridge/      # JSON-RPC stdio bridge for lxmf-conformance
│   └── tests/               # native host smoke tests for individual components
├── .github/workflows/       # CI: cross-impl conformance against python reference
├── library.json             # PlatformIO manifest
└── platformio.ini           # for native-host development builds only
```

## Dependencies

- [microReticulum](https://github.com/attermann/microReticulum) `>= 0.3.0`
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) `^7.4.2` (for `MessageStore` JSON-on-disk)

## License

Copyright (c) 2026 Torlando Tech LLC. Licensed under the [GNU General Public License v3.0](LICENSE).
