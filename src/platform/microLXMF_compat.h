#pragma once

// Platform compatibility shim for microLXMF.
//
// On Arduino-ESP32 targets the standard ESP-IDF / FreeRTOS headers exist and
// expose `esp_task_wdt_reset()` and `vTaskDelay()` directly. On host (native /
// POSIX) targets — where the conformance bridge runs — those headers do not
// exist, so we provide no-op replacements gated on `MICROLXMF_NATIVE`.
//
// LXMF code calls these only from `LXStamper.cpp` during stamp proof-of-work
// to feed the watchdog; on host PCs there is no watchdog to feed.

#if defined(MICROLXMF_NATIVE) || defined(NATIVE) || defined(LIBRARY_TEST)

#include <thread>
#include <chrono>

inline void esp_task_wdt_reset() {}
inline void vTaskDelay(int ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

#else

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_task_wdt.h"

#endif
