cmake_minimum_required(VERSION 3.14)
project(microLXMFBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ============================================================================
# microReticulum: pin the same 0.4.1-layout commit used by Pyxis firmware.
# Keeping the native bridge on the firmware dependency makes x86 tests compile
# the same namespaced headers and transport/resource implementation.
#
# microReticulum's own CMakeLists already FetchContents ArduinoJson, MsgPack,
# attermann/Crypto and produces these targets we'll link against:
#   - microReticulum / ReticulumStatic (version-dependent library target)
#   - CryptoLib        (attermann/Crypto compiled)
#   - ArduinoJson      (header-only INTERFACE lib)
#   - MsgPack          (header-only INTERFACE lib)
# So we don't have to re-declare any of them here.
# ============================================================================
# microReticulum's CMakeLists references a `msgpackc-cxx` link target it
# never actually uses (the C++ wire encoding goes through hideakitai/MsgPack
# instead). On a host that doesn't have the system msgpack-c installed,
# the link fails. Define an empty INTERFACE shim BEFORE making microreticulum
# available so its target_link_libraries() resolves cleanly.
add_library(msgpackc-cxx INTERFACE)
# Same story: microReticulum's CMakeLists also link-decls a `crypto` lib
# that's never actually used on host (the real crypto lives in CryptoLib +
# attermann/Crypto sources). Empty shim.
add_library(crypto INTERFACE)
# hideakitai/MsgPack is an Arduino library — `library.properties` only,
# no CMakeLists. FetchContent_MakeAvailable on it doesn't create a CMake
# target. microReticulum's CMakeLists link-decls a `MsgPack` target so we
# need to provide it. Same for `ArduinoJson` — bblanchon/ArduinoJson DOES
# have a CMakeLists, but its target is named `ArduinoJson` already; we
# just need the include path to propagate. We define MsgPack here as a
# header-only INTERFACE lib pointing at the upstream-fetched source.
# (The actual include path is appended below, after FetchContent runs.)
add_library(MsgPack INTERFACE)

# Declare msgpack at the outer scope BEFORE microReticulum's FetchContent
# runs. microReticulum's CMakeLists also `FetchContent_Declare(msgpack ...)`
# at v0.4.2 — the de-dupe skips the inner fetch but only if we've already
# declared with the same name. The critical side effect: ${msgpack_SOURCE_DIR}
# becomes visible at THIS scope. Without this declaration, microReticulum's
# nested call populates the variable only in its own subdirectory scope and
# our patch step below silently no-ops with an empty path.
FetchContent_Declare(msgpack
    GIT_REPOSITORY https://github.com/hideakitai/MsgPack.git
    GIT_TAG v0.4.2
)
FetchContent_MakeAvailable(msgpack)

FetchContent_Declare(microreticulum
    GIT_REPOSITORY https://github.com/torlando-tech/microReticulum.git
    # Pinned to a SHA rather than the `pyxis-fixes-on-0.3.0` branch so CI
    # builds are reproducible AND so a force-fetch is guaranteed when the
    # SHA bumps (CMake FetchContent caches branch refs across configure
    # runs in some setups, which made it tricky to verify the actual
    # commit being compiled). Bump this when landing a new microReticulum
    # patch on the fork. Current pin matches platformio.ini in Pyxis.
    GIT_TAG 6054f6ba82367628a85cd07fcb668b95e947f046
)
set(RNS_USE_FS ON CACHE BOOL "Enable native persistence" FORCE)
set(RNS_PERSIST_PATHS ON CACHE BOOL "Persist native path tables" FORCE)
FetchContent_MakeAvailable(microreticulum)

# The homelab build host does not require a system libbz2-dev package. Build
# microReticulum's vendored library so native simulation stays self-contained.
enable_language(C)
if(NOT TARGET bz2)
    file(GLOB BZ2_SOURCES ${microreticulum_SOURCE_DIR}/lib/libbz2/*.c)
    add_library(bz2 STATIC ${BZ2_SOURCES})
    target_include_directories(bz2 PUBLIC ${microreticulum_SOURCE_DIR}/lib/libbz2)
    target_compile_definitions(bz2 PRIVATE BZ_NO_STDIO)
endif()
target_include_directories(microReticulum PUBLIC ${microreticulum_SOURCE_DIR}/lib/libbz2)

if(TARGET microReticulum)
    set(RETICULUM_TARGET microReticulum)
elseif(TARGET ReticulumStatic)
    set(RETICULUM_TARGET ReticulumStatic)
else()
    message(FATAL_ERROR "microReticulum did not expose a supported CMake target")
endif()

# ============================================================================
# microStore — header-only, POSIX backend.
# microReticulum's Bytes.h includes <microStore/Codec.h>, so even though
# microReticulum's CMakeLists doesn't fetch microStore (it relies on the
# consumer to provide the include path), we must.
# ============================================================================
FetchContent_Declare(microstore
    GIT_REPOSITORY https://github.com/attermann/microStore.git
    GIT_TAG master
)
FetchContent_MakeAvailable(microstore)

# MsgPack's transitive Arduino-library deps. microReticulum's CMakeLists
# doesn't fetch these because under PIO they come from library.properties
# `depends=`. Under raw CMake we have to declare them ourselves.
FetchContent_Declare(arxcontainer
    GIT_REPOSITORY https://github.com/hideakitai/ArxContainer.git
    GIT_TAG main)
FetchContent_MakeAvailable(arxcontainer)

FetchContent_Declare(arxtypetraits
    GIT_REPOSITORY https://github.com/hideakitai/ArxTypeTraits.git
    GIT_TAG main)
FetchContent_MakeAvailable(arxtypetraits)

FetchContent_Declare(debuglog
    GIT_REPOSITORY https://github.com/hideakitai/DebugLog.git
    GIT_TAG main)
FetchContent_MakeAvailable(debuglog)

# Make these visible to microReticulum's translation units AND to consumers
# (microLXMF, the bridge). microReticulum links ArduinoJson + MsgPack as
# PRIVATE deps — they're a transitive build dep but not exported. Bytes.h
# includes <ArduinoJson.h> as part of its public surface so consumers need
# them too.
target_include_directories(${RETICULUM_TARGET} PUBLIC
    ${arxcontainer_SOURCE_DIR}
    ${arxtypetraits_SOURCE_DIR}
    ${debuglog_SOURCE_DIR}
    ${arduinojson_SOURCE_DIR}/src
    ${msgpack_SOURCE_DIR}
    ${crypto_SOURCE_DIR}
)
# Now that FetchContent has populated MsgPack, point our shim INTERFACE
# lib at the source so consumers can `target_link_libraries(... MsgPack)`
# and pick up the headers.
target_include_directories(MsgPack INTERFACE ${msgpack_SOURCE_DIR})

# Patch hideakitai/MsgPack to expose `packRawBytes` (private) and the
# Unpacker's byte-position state (also private). LXMessage's fields map
# uses `dict[int, Any]` on the wire — to splice arbitrary msgpack values
# into the stream without LXMessage knowing the value type, we splice
# pre-encoded bytes via packRawBytes; on decode we use Unpacker's
# `indices` to capture each key+value byte span.
#
# Idempotent: detects the already-patched state by grepping for the
# patch marker comment, skips if already applied. Uses a real ASCII
# newline (not the `\n` literal in a quoted argument, which doesn't
# round-trip reliably across CMake versions when re-stringified through
# string(REPLACE)) to make the multi-line match work in CI.
string(ASCII 10 _PATCH_NL)
set(_PATCH_MARKER "// patched-by-microlxmf")

message(STATUS "msgpack_SOURCE_DIR = ${msgpack_SOURCE_DIR}")
if(EXISTS "${msgpack_SOURCE_DIR}/MsgPack/Packer.h")
    message(STATUS "Patching ${msgpack_SOURCE_DIR}/MsgPack/Packer.h")
    file(READ "${msgpack_SOURCE_DIR}/MsgPack/Packer.h" _packer_content)
    if(NOT _packer_content MATCHES "${_PATCH_MARKER}")
        # v0.4.2 has the only `private:` block in this file at line ~963,
        # immediately followed by `void packRawByte`. Match-and-replace it.
        set(_packer_target "    private:${_PATCH_NL}        void packRawByte")
        set(_packer_replacement "    public:  ${_PATCH_MARKER}${_PATCH_NL}        void packRawByte")
        string(REPLACE "${_packer_target}" "${_packer_replacement}"
                       _packer_patched "${_packer_content}")
        if("${_packer_patched}" STREQUAL "${_packer_content}")
            message(FATAL_ERROR
                "Failed to patch ${msgpack_SOURCE_DIR}/MsgPack/Packer.h — "
                "expected `    private:\\n        void packRawByte` block "
                "from hideakitai/MsgPack v0.4.2 not found. Did the upstream "
                "tag change?")
        endif()
        file(WRITE "${msgpack_SOURCE_DIR}/MsgPack/Packer.h" "${_packer_patched}")
        message(STATUS "Patched Packer.h: rewrote private→public")
    else()
        message(STATUS "Packer.h already has marker — skipping patch")
    endif()
else()
    message(WARNING "Packer.h not found at ${msgpack_SOURCE_DIR}/MsgPack/Packer.h")
endif()
if(EXISTS "${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h")
    message(STATUS "Patching ${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h")
    file(READ "${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h" _unpacker_content)
    if(NOT _unpacker_content MATCHES "${_PATCH_MARKER}")
        # `class Unpacker {` defaults to private, so `raw_data` and
        # `indices` (declared right after the opening brace) are private.
        # Inject `public:` between the brace and the first member.
        set(_unpacker_target "    class Unpacker {${_PATCH_NL}        uint8_t* raw_data")
        set(_unpacker_replacement
            "    class Unpacker {${_PATCH_NL}    public:  ${_PATCH_MARKER}${_PATCH_NL}        uint8_t* raw_data")
        string(REPLACE "${_unpacker_target}" "${_unpacker_replacement}"
                       _unpacker_patched "${_unpacker_content}")
        if("${_unpacker_patched}" STREQUAL "${_unpacker_content}")
            message(FATAL_ERROR
                "Failed to patch ${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h — "
                "expected `class Unpacker {\\n        uint8_t* raw_data` "
                "block from hideakitai/MsgPack v0.4.2 not found. Did the "
                "upstream tag change?")
        endif()
        file(WRITE "${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h" "${_unpacker_patched}")
        message(STATUS "Patched Unpacker.h: injected public:")
    else()
        message(STATUS "Unpacker.h already has marker — skipping patch")
    endif()
else()
    message(WARNING "Unpacker.h not found at ${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h")
endif()

# The current microReticulum target recursively compiles tlsf.c, links system
# bzip2, exports native persistence definitions, and propagates microStore.
# Keep the explicit include for compatibility with its public Bytes headers.
target_include_directories(${RETICULUM_TARGET} PUBLIC ${microstore_SOURCE_DIR}/include)

# ============================================================================
# microLXMF static lib
# ============================================================================
if(NOT DEFINED MICROLXMF_SRC)
    set(MICROLXMF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
endif()

file(GLOB_RECURSE MICROLXMF_SOURCES ${MICROLXMF_SRC}/LXMF/*.cpp)
add_library(MicroLXMFLib STATIC ${MICROLXMF_SOURCES})
# Only expose the parent dir, NOT MICROLXMF_SRC/LXMF — putting LXMF on the
# system include path would shadow microReticulum's Type.h (LXMF/Type.h
# uses `#include <Type.h>` to pull in RNS::Type and `#include "Type.h"`
# from sibling files to pull in LXMF::Type).
target_include_directories(MicroLXMFLib PUBLIC ${MICROLXMF_SRC})
# ArduinoJson + MsgPack are header-only INTERFACE libs created by
# microReticulum's FetchContent. Linking PUBLIC propagates their include
# dirs to consumers (microLXMF .cpp files include <ArduinoJson.h> in
# MessageStore.cpp; LXMRouter.cpp + LXMessage.cpp include <MsgPack.h>).
target_link_libraries(MicroLXMFLib PUBLIC ${RETICULUM_TARGET} ArduinoJson MsgPack)
target_compile_options(MicroLXMFLib PRIVATE
    -fexceptions
    -fstack-usage
    -Wno-everything
)

# ============================================================================
# Bridge executable
# ============================================================================
file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/commands/*.cpp
    src/runtime/*.cpp
)

add_executable(microLXMFBridge ${BRIDGE_SOURCES})
target_include_directories(microLXMFBridge PRIVATE src)
target_link_libraries(microLXMFBridge PRIVATE MicroLXMFLib)
target_compile_options(microLXMFBridge PRIVATE
    -fexceptions
    -Wall
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
)
target_compile_definitions(microLXMFBridge PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)

find_package(Threads REQUIRED)
target_link_libraries(microLXMFBridge PRIVATE Threads::Threads)

# ============================================================================
# Host-side test: validates MessageStore two-tier (hot + archive) behavior.
# Runs via `./test_messagestore_tiers` after `cmake --build build/`. No
# hardware needed — uses two temp dirs as POSIX-backed hot + archive
# filesystems and exercises cull, archive fallback, hard-cap eviction.
# ============================================================================
add_executable(test_messagestore_tiers tests/test_messagestore_tiers.cpp)
target_include_directories(test_messagestore_tiers PRIVATE src)
target_link_libraries(test_messagestore_tiers PRIVATE MicroLXMFLib Threads::Threads)
target_compile_options(test_messagestore_tiers PRIVATE
    -fexceptions
    -Wall -Wno-unused-parameter -Wno-missing-field-initializers
)
target_compile_definitions(test_messagestore_tiers PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)

# Pyxis invokes save_message() from LVGL's 8 KiB task. Keep enough headroom
# for UI, packing, filesystem and C++ frames; v153 regressed to >8 KiB here.
add_custom_target(check_messagestore_stack
    COMMAND ${CMAKE_COMMAND}
        -DSTACK_ROOT=${CMAKE_BINARY_DIR}
        -DMAX_SAVE_MESSAGE_STACK=2048
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMessageStoreStack.cmake
    DEPENDS MicroLXMFLib
    COMMENT "Checking MessageStore::save_message stack budget"
)

# Host-side regression coverage for lxmf.delivery announced stamp costs:
# MessagePack parsing, nil clearing, malformed-data rejection, and bounded
# cache eviction. The live cross-implementation suite separately proves that
# the learned cost results in a stamped DIRECT message accepted by Python LXMF.
add_executable(test_announced_stamp_cost tests/test_announced_stamp_cost.cpp)
target_include_directories(test_announced_stamp_cost PRIVATE src)
target_link_libraries(test_announced_stamp_cost PRIVATE MicroLXMFLib Threads::Threads)
target_compile_options(test_announced_stamp_cost PRIVATE
    -fexceptions
    -Wall -Wno-unused-parameter -Wno-missing-field-initializers
)
target_compile_definitions(test_announced_stamp_cost PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)

# Host-side regression coverage for explicit, non-evicting outbound queue
# admission. A full queue must reject the new message without mutating it or
# discarding previously accepted work.
add_executable(test_outbound_admission tests/test_outbound_admission.cpp)
target_include_directories(test_outbound_admission PRIVATE src)
target_link_libraries(test_outbound_admission PRIVATE MicroLXMFLib Threads::Threads)
target_compile_options(test_outbound_admission PRIVATE
    -fexceptions
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
)
target_compile_definitions(test_outbound_admission PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)
