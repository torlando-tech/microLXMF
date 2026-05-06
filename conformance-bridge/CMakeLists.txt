cmake_minimum_required(VERSION 3.14)
project(microLXMFBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ============================================================================
# microReticulum: pinned to torlando-tech/microReticulum:pyxis-fixes-on-0.3.0
# (vanilla 0.3.0 + 3 PR-ready Cryptography fixes — PKCS7 padding, HMAC double-
# feed, X25519 RFC 7748 clamping). Without these the bridge's AES/Token
# interop with the python LXMF reference fails.
#
# microReticulum's own CMakeLists already FetchContents ArduinoJson, MsgPack,
# attermann/Crypto and produces these targets we'll link against:
#   - ReticulumStatic  (the entire microReticulum library)
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

FetchContent_Declare(microreticulum
    GIT_REPOSITORY https://github.com/torlando-tech/microReticulum.git
    GIT_TAG pyxis-fixes-on-0.3.0
)
FetchContent_MakeAvailable(microreticulum)

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
target_include_directories(ReticulumStatic PUBLIC
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
# `indices` to capture each key+value byte span. The patch is
# idempotent: only rewrites the modifiers when they're still default-
# private from the upstream tag.
if(EXISTS "${msgpack_SOURCE_DIR}/MsgPack/Packer.h")
    file(READ "${msgpack_SOURCE_DIR}/MsgPack/Packer.h" _packer_content)
    string(REPLACE
        "    private:\n        void packRawByte"
        "    public:  // patched by microLXMF conformance-bridge CMakeLists\n        void packRawByte"
        _packer_patched "${_packer_content}")
    if(NOT "${_packer_patched}" STREQUAL "${_packer_content}")
        file(WRITE "${msgpack_SOURCE_DIR}/MsgPack/Packer.h" "${_packer_patched}")
    endif()
endif()
if(EXISTS "${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h")
    file(READ "${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h" _unpacker_content)
    string(REPLACE
        "    class Unpacker {\n        uint8_t* raw_data"
        "    class Unpacker {\n    public:  // patched by microLXMF conformance-bridge CMakeLists\n        uint8_t* raw_data"
        _unpacker_patched "${_unpacker_content}")
    if(NOT "${_unpacker_patched}" STREQUAL "${_unpacker_content}")
        file(WRITE "${msgpack_SOURCE_DIR}/MsgPack/Unpacker.h" "${_unpacker_patched}")
    endif()
endif()

# microReticulum's CMakeLists glob is `src/*.cpp` — it misses tlsf.c (the
# pool allocator backing RNS::Utilities::Memory). Compile it separately
# and link into ReticulumStatic so the unresolved tlsf_* symbols disappear.
enable_language(C)
add_library(TlsfLib STATIC ${microreticulum_SOURCE_DIR}/src/Utilities/tlsf.c)
target_include_directories(TlsfLib PUBLIC ${microreticulum_SOURCE_DIR}/src/Utilities)
target_link_libraries(ReticulumStatic PUBLIC TlsfLib)

# bzip2 (libbz2) — vendored into microReticulum/lib/libbz2 alongside the
# new Cryptography/BZ2 wrapper. Resource transfer uses bz2 to compress
# payloads matching python LXMF's auto_compress path; without it the
# receiver can't decode resources sent from python.
file(GLOB BZ2_SOURCES ${microreticulum_SOURCE_DIR}/lib/libbz2/*.c)
add_library(Bz2Lib STATIC ${BZ2_SOURCES})
target_include_directories(Bz2Lib PUBLIC ${microreticulum_SOURCE_DIR}/lib/libbz2)
target_compile_options(Bz2Lib PRIVATE -Wno-everything -DBZ_NO_STDIO)
target_link_libraries(ReticulumStatic PUBLIC Bz2Lib)

# Make microStore headers visible to microReticulum's translation units.
target_include_directories(ReticulumStatic PUBLIC ${microstore_SOURCE_DIR}/include)
# Same compile defines microReticulum sets PRIVATE — but we need them for
# downstream targets too (microLXMF, the bridge) so they see the same
# headers Bytes.h sees. Without re-declaring these the bridge will compile
# Bytes.h with different gating and we'll get ODR / linker mismatches.
target_compile_definitions(ReticulumStatic PUBLIC
    NATIVE
    RNS_USE_FS
    RNS_PERSIST_PATHS
    USTORE_USE_POSIXFS
)

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
target_link_libraries(MicroLXMFLib PUBLIC ReticulumStatic ArduinoJson MsgPack)
target_compile_options(MicroLXMFLib PRIVATE
    -fexceptions
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
