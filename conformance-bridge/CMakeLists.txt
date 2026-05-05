cmake_minimum_required(VERSION 3.14)
project(microLXMFBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Bridge skeleton — Phase 0
# ============================================================================
# This first cut builds ONLY the JSON-RPC stdio loop + command registry +
# stubs that throw "not implemented". The actual microReticulum + microLXMF
# runtime is not linked yet because the host build of microReticulum +
# microLXMF needs more infrastructure than this commit provides.
#
# Roadmap to a working bridge:
#   1. (this commit) skeleton compiles, READY prints, ping responds, all
#      lxmf_* commands return "not implemented".
#   2. Pull in attermann/Crypto, ArduinoJson, microStore (same recipe as
#      reticulum-conformance/impls/microreticulum/CMakeLists.txt).
#   3. Pull in attermann/microReticulum @ 0.3.0 source — but only the host-
#      compatible pieces. Need a host POSIX FileSystem adapter for microStore
#      since SPIFFSFileSystem only exists on ESP-IDF.
#   4. Add microLXMF sources from ../src/LXMF.
#   5. Vendor or link pyxis's TCPClientInterface (already POSIX-aware via
#      `#ifdef ARDUINO`). Implement TCPServerInterface — pyxis is client-only
#      today. Phase-1 of lxmf-conformance needs both ends.
#   6. Spin up an `RNS::Reticulum` singleton + Transport thread inside the
#      bridge process. Wire LXMRouter delivery callback to push received
#      messages into an in-memory queue served by lxmf_get_received_messages.
#   7. Implement the 8 command handlers proper.
# ============================================================================

file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/commands/*.cpp
)

add_executable(microLXMFBridge ${BRIDGE_SOURCES})
target_include_directories(microLXMFBridge PRIVATE src)
target_compile_options(microLXMFBridge PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)
target_compile_definitions(microLXMFBridge PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)
