#include "../../src/LXMF/LXMRouter.h"

#include <MsgPack.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using LXMF::LXMRouter;
using LXMF::LXMessage;
using RNS::Bytes;
using RNS::Destination;
using RNS::Identity;

namespace {

Bytes destination_hash(uint8_t seed) {
    uint8_t raw[16];
    for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = static_cast<uint8_t>(seed + i);
    return Bytes(raw, sizeof(raw));
}

Bytes announce_with_cost(int64_t cost) {
    const uint8_t name[] = {'p', 'e', 'e', 'r'};
    MsgPack::Packer packer;
    packer.packArraySize(2);
    packer.packBinary(name, sizeof(name));
    packer.pack(cost);
    return Bytes(packer.data(), packer.size());
}

Bytes announce_without_cost() {
    const uint8_t name[] = {'p', 'e', 'e', 'r'};
    MsgPack::Packer packer;
    packer.packArraySize(2);
    packer.packBinary(name, sizeof(name));
    packer.packNil();
    return Bytes(packer.data(), packer.size());
}

void test_announce_updates_and_clears_cost() {
    LXMRouter router(Identity(), "", false);
    const Bytes hash = destination_hash(1);

    router.on_delivery_announce(hash, announce_with_cost(4));
    assert(router.get_outbound_stamp_cost(hash) == 4);

    router.on_delivery_announce(hash, announce_without_cost());
    assert(router.get_outbound_stamp_cost(hash) == 0);

    const Bytes nil_name_hash = destination_hash(17);
    const uint8_t nil_name[] = {0x92, 0xc0, 0x05};
    router.on_delivery_announce(nil_name_hash, Bytes(nil_name, sizeof(nil_name)));
    assert(router.get_outbound_stamp_cost(nil_name_hash) == 5);

    const Bytes string_name_hash = destination_hash(18);
    const uint8_t string_name[] = {0x92, 0xa4, 'p', 'e', 'e', 'r', 0xcc, 0x06};
    router.on_delivery_announce(string_name_hash, Bytes(string_name, sizeof(string_name)));
    assert(router.get_outbound_stamp_cost(string_name_hash) == 6);
}

void test_noncanonical_integer_encodings_are_accepted() {
    LXMRouter router(Identity(), "", false);
    const uint8_t uint64_cost[] = {0x92, 0xc0, 0xcf, 0, 0, 0, 0, 0, 0, 0, 4};
    const uint8_t int32_cost[] = {0x92, 0xc0, 0xd2, 0, 0, 0, 5};
    const uint8_t int64_cost[] = {0x92, 0xc0, 0xd3, 0, 0, 0, 0, 0, 0, 0, 6};

    router.on_delivery_announce(destination_hash(19), Bytes(uint64_cost, sizeof(uint64_cost)));
    router.on_delivery_announce(destination_hash(20), Bytes(int32_cost, sizeof(int32_cost)));
    router.on_delivery_announce(destination_hash(21), Bytes(int64_cost, sizeof(int64_cost)));

    assert(router.get_outbound_stamp_cost(destination_hash(19)) == 4);
    assert(router.get_outbound_stamp_cost(destination_hash(20)) == 5);
    assert(router.get_outbound_stamp_cost(destination_hash(21)) == 6);
}

void test_malformed_announce_does_not_replace_cost() {
    LXMRouter router(Identity(), "", false);
    const Bytes hash = destination_hash(33);
    router.update_stamp_cost(hash, 7);

    const uint8_t malformed[] = {0x91, 0xc4, 0x01, 'x'}; // one-element array
    router.on_delivery_announce(hash, Bytes(malformed, sizeof(malformed)));
    assert(router.get_outbound_stamp_cost(hash) == 7);

    router.on_delivery_announce(hash, announce_with_cost(255));
    assert(router.get_outbound_stamp_cost(hash) == 7);
}

void test_cache_is_bounded_and_evicts_oldest() {
    LXMRouter router(Identity(), "", false);

    for (uint8_t i = 0; i < 33; ++i) {
        router.update_stamp_cost(destination_hash(i), static_cast<uint8_t>(i + 1));
    }

    assert(router.get_outbound_stamp_cost(destination_hash(0)) == 0);
    assert(router.get_outbound_stamp_cost(destination_hash(1)) == 2);
    assert(router.get_outbound_stamp_cost(destination_hash(32)) == 33);
}

void test_cache_reuses_cleared_slot_before_eviction() {
    LXMRouter router(Identity(), "", false);
    for (uint8_t i = 0; i < 32; ++i) {
        router.update_stamp_cost(destination_hash(i), static_cast<uint8_t>(i + 1));
    }

    router.update_stamp_cost(destination_hash(9), 0);
    router.update_stamp_cost(destination_hash(90), 7);

    assert(router.get_outbound_stamp_cost(destination_hash(0)) == 1);
    assert(router.get_outbound_stamp_cost(destination_hash(9)) == 0);
    assert(router.get_outbound_stamp_cost(destination_hash(90)) == 7);

    router.update_stamp_cost(destination_hash(91), 8);
    assert(router.get_outbound_stamp_cost(destination_hash(0)) == 0);
    assert(router.get_outbound_stamp_cost(destination_hash(1)) == 2);
    assert(router.get_outbound_stamp_cost(destination_hash(90)) == 7);
}

void test_outbound_message_uses_cached_cost_and_contains_valid_stamp() {
    Identity local_identity;
    Identity remote_identity;
    LXMRouter router(local_identity, "", false);
    Destination remote(
        remote_identity,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );
    router.update_stamp_cost(remote.hash(), 4);

    const uint8_t content[] = {'s', 't', 'a', 'm', 'p'};
    LXMessage message(remote, router.delivery_destination(), Bytes(content, sizeof(content)));
    router.handle_outbound(message);

    assert(message.stamp_cost() == 4);
    assert(message.has_valid_stamp());
    assert(message.stamp().size() == 32);
    assert(message.validate_stamp(4));
    assert(router.pending_outbound_count() == 1);
}

void test_excessive_announced_cost_fails_without_mining_or_queueing() {
    Identity local_identity;
    Identity remote_identity;
    LXMRouter router(local_identity, "", false);
    Destination remote(
        remote_identity,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );
    router.update_stamp_cost(remote.hash(), 254);

    LXMessage message(remote, router.delivery_destination(), Bytes("bounded"));
    bool rejected = false;
    try {
        router.handle_outbound(message);
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    assert(rejected);
    assert(message.stamp_cost() == 0);
    assert(!message.has_valid_stamp());
    assert(router.pending_outbound_count() == 0);
}

} // namespace

int main() {
    test_announce_updates_and_clears_cost();
    test_noncanonical_integer_encodings_are_accepted();
    test_malformed_announce_does_not_replace_cost();
    test_cache_is_bounded_and_evicts_oldest();
    test_cache_reuses_cleared_slot_before_eviction();
    test_outbound_message_uses_cached_cost_and_contains_valid_stamp();
    test_excessive_announced_cost_fails_without_mining_or_queueing();
    std::cout << "announced stamp cost tests passed\n";
    return 0;
}
