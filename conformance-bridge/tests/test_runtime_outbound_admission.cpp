#include "runtime/Runtime.h"

#include <microReticulum/Utilities/OS.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

struct Fixture {
    char path_template[64] = "/tmp/microlxmf-runtime-admission-XXXXXX";
    std::string root;
    bridge::PrefixedFileSystem filesystem;

    Fixture()
        : root(::mkdtemp(path_template)), filesystem(root) {
        assert(!root.empty());
        RNS::Utilities::OS::register_filesystem(filesystem);
    }

    ~Fixture() {
        RNS::Utilities::OS::deregister_filesystem();
        std::filesystem::remove_all(root);
    }
};

RNS::Bytes rememberedDestination(uint8_t seed) {
    RNS::Identity remote;
    RNS::Destination destination(
        remote, RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    uint8_t bytes[16] = {};
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<uint8_t>(seed + i);
    }
    RNS::Identity::remember(
        RNS::Bytes(bytes, sizeof(bytes)), destination.hash(),
        remote.get_public_key());
    return destination.hash();
}

void preparationFailureLeavesNoDurableOutbound() {
    Fixture fixture;
    RNS::Identity local;
    LXMF::LXMRouter router(local, "", false);
    LXMF::MessageStore store("/lxmf");
    std::mutex store_mutex;
    std::mutex router_mutex;
    std::mutex outbound_mutex;
    std::map<RNS::Bytes, LXMF::Type::Message::State> outbound_states;
    const RNS::Bytes destination = rememberedDestination(0x31U);
    router.update_stamp_cost(destination, 254U);

    bool threw = false;
    try {
        (void)bridge::detail::send_message_internal(
            router, local, destination, "must-not-persist", "", {},
            LXMF::Type::Message::DIRECT, store, store_mutex, router_mutex,
            outbound_mutex, outbound_states, 1700000000.0);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
    assert(router.pending_outbound_count() == 0U);
    assert(outbound_states.empty());
    assert(store.get_messages_for_conversation(destination).empty());
}

void acceptedOwnershipIsDurableAndUsesFinalHash() {
    Fixture fixture;
    RNS::Identity local;
    LXMF::LXMRouter router(local, "", false);
    LXMF::MessageStore store("/lxmf");
    std::mutex store_mutex;
    std::mutex router_mutex;
    std::mutex outbound_mutex;
    std::map<RNS::Bytes, LXMF::Type::Message::State> outbound_states;
    const RNS::Bytes destination = rememberedDestination(0x61U);

    const RNS::Bytes hash = bridge::detail::send_message_internal(
        router, local, destination, "persist-and-queue", "", {},
        LXMF::Type::Message::DIRECT, store, store_mutex, router_mutex,
        outbound_mutex, outbound_states, 1700000001.0);

    const auto hashes = store.get_messages_for_conversation(destination);
    assert(router.pending_outbound_count() == 1U);
    assert(hashes.size() == 1U);
    assert(hashes.front() == hash);
    assert(store.load_message(hash).hash() == hash);
    assert(outbound_states.at(hash) == LXMF::Type::Message::OUTBOUND);
}

}  // namespace

int main() {
    preparationFailureLeavesNoDurableOutbound();
    acceptedOwnershipIsDurableAndUsesFinalHash();
    std::cout << "runtime outbound admission: passed\n";
    return 0;
}
