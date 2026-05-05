# microLXMF

C++ implementation of [LXMF](https://github.com/markqvist/LXMF) (Lightweight Extensible Message Format) layered on top of [attermann/microReticulum](https://github.com/attermann/microReticulum). Sister library to [LXMF-swift](https://github.com/torlando-tech/LXMF-swift) and [LXMF-kt](https://github.com/torlando-tech/LXMF-kt).

Targets ESP32 (Arduino framework) and host (POSIX) builds; the same source tree builds in both.

## Status

Bootstrap. Sources extracted from [pyxis](https://github.com/torlando-tech/pyxis)' vendored fork (originally derived from `torlando-tech/microReticulum:feat/t-deck`'s LXMF subtree) and being aligned to vanilla `attermann/microReticulum @ 0.3.0`. Conformance bridge under construction; expect interop bugs to surface against the python LXMF reference until the lxmf-conformance suite is green.

## Layout

```
microLXMF/
├── src/
│   ├── LXMF/                # public C++ API
│   │   ├── LXMessage.{h,cpp}
│   │   ├── LXMRouter.{h,cpp}
│   │   ├── LXStamper.{h,cpp}
│   │   ├── MessageStore.{h,cpp}
│   │   ├── PropagationNodeManager.{h,cpp}
│   │   └── Type.h
│   └── platform/            # host/native shims for esp_task_wdt + freertos
├── test/                    # PlatformIO native unit tests
├── conformance-bridge/      # JSON-RPC stdio bridge for lxmf-conformance
└── library.json             # PlatformIO manifest
```

## Dependencies

- [microReticulum](https://github.com/attermann/microReticulum) `>= 0.3.0`
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) `^7.4.2` (for `MessageStore` JSON-on-disk)

## License

Copyright (c) 2026 Torlando Tech LLC. Licensed under the [Mozilla Public License 2.0](LICENSE).
