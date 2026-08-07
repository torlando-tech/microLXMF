#include "../../src/LXMF/LXMRouter.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

using LXMF::LXMRouter;
using LXMF::LXMessage;
using LXMF::OutboundAdmissionResult;
using RNS::Bytes;
using RNS::Destination;
using RNS::Identity;

namespace {

bool rejectGuard(void*) { return false; }

Destination remoteDestination(uint8_t seed) {
    Identity identity;
    uint8_t material[64]{};
    for (std::size_t i = 0; i < sizeof(material); ++i) {
        material[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
    }
    identity.load_private_key(Bytes(material, sizeof(material)));
    return Destination(identity,
                       RNS::Type::Destination::OUT,
                       RNS::Type::Destination::SINGLE,
                       "lxmf",
                       "delivery");
}

void fullQueueRejectsWithoutEvictionOrMessageMutation() {
    LXMRouter router(Identity(), "", false);
    Destination remote = remoteDestination(7);
    assert(router.outbound_queue_has_capacity());

    LXMessage guard_rejected(remote, router.delivery_destination(),
                             Bytes("guard-rejected"));
    assert(router.try_handle_outbound(guard_rejected, rejectGuard, nullptr) ==
           OutboundAdmissionResult::GUARD_REJECTED);
    assert(router.pending_outbound_count() == 0U);

    std::size_t accepted = 0;
    for (;;) {
        const uint8_t payload[] = {
            static_cast<uint8_t>(accepted & 0xffU), 0x5aU};
        LXMessage message(remote, router.delivery_destination(),
                          Bytes(payload, sizeof(payload)));
        const OutboundAdmissionResult result = router.try_handle_outbound(message);
        if (result == OutboundAdmissionResult::QUEUE_FULL) {
            assert(message.state() != LXMF::Type::Message::OUTBOUND);
            break;
        }
        assert(result == OutboundAdmissionResult::ACCEPTED);
        ++accepted;
        assert(router.pending_outbound_count() == accepted);
        assert(accepted < 1024U);
    }

    assert(accepted > 0U);
    const std::size_t full_count = router.pending_outbound_count();
    assert(!router.outbound_queue_has_capacity());
    LXMessage rejected(remote, router.delivery_destination(), Bytes("rejected"));
    assert(router.try_handle_outbound(rejected) ==
           OutboundAdmissionResult::QUEUE_FULL);
    assert(router.pending_outbound_count() == full_count);
    assert(rejected.state() != LXMF::Type::Message::OUTBOUND);

    LXMessage legacy_rejected(remote, router.delivery_destination(),
                              Bytes("legacy-rejected"));
    router.handle_outbound(legacy_rejected);
    assert(router.pending_outbound_count() == full_count);
    assert(legacy_rejected.state() != LXMF::Type::Message::OUTBOUND);
}

}  // namespace

int main() {
    fullQueueRejectsWithoutEvictionOrMessageMutation();
    std::cout << "outbound admission: passed\n";
    return 0;
}
