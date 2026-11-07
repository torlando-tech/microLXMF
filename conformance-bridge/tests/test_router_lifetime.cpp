// Compile against clean microLXMF with AddressSanitizer. The registry and
// receipt are arranged locally before router destruction; the proof enters
// through normal RNS packet handling with a real peer signature.
#include <sstream>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <vector>
#include <list>
#include <iostream>
#define private public
#define protected public
#include <microReticulum/Reticulum.h>
#include <microReticulum/Transport.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include "LXMF/LXMRouter.cpp"
#undef protected
#undef private

struct InboundInterface final : RNS::InterfaceImpl {
    InboundInterface() : InterfaceImpl("signed-proof-regression") {
        _IN = true;
        _OUT = true;
        _online = true;
        _bitrate = 1000000;
    }
    bool send_outgoing(const RNS::Bytes&) override { return true; }
};

int main() {
    microStore::FileSystem fs{microStore::Adapters::UniversalFileSystem()};
    fs.init();
    RNS::Utilities::OS::register_filesystem(fs);
    RNS::Reticulum rns;
    rns.start();
    RNS::Interface interface(new InboundInterface);
    RNS::Transport::register_interface(interface);

    auto old = std::make_unique<LXMF::LXMRouter>(RNS::Identity());
    RNS::Identity first_peer;
    RNS::Destination first(first_peer, RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    auto* registry = find_empty_router_registry_slot();
    if (!registry) return 2;
    registry->in_use = true;
    registry->set_destination_hash(first.hash());
    registry->router = old.get();

    uint8_t packet_hash[32] = {9};
    uint8_t message_hash[32] = {7};
    RNS::PacketReceipt receipt;
    receipt._object->_hash = RNS::Bytes(packet_hash, sizeof(packet_hash));
    receipt._object->_destination = first;
    receipt.set_delivery_callback(LXMF::LXMRouter::static_proof_callback);
    auto* pending = LXMF::LXMRouter::find_empty_pending_proof_slot();
    if (!pending) return 3;
    pending->in_use = true;
    pending->set_packet_hash(receipt.hash());
    pending->set_message_hash(RNS::Bytes(message_hash, sizeof(message_hash)));

    _active_sync_router = old.get();
    old.reset();
    if (_active_sync_router || find_router_registry_slot(first.hash())) return 4;

    RNS::Bytes proof = receipt.hash() + first_peer.sign(receipt.hash());
    RNS::Destination proof_destination(RNS::Type::NONE,
        RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
        receipt.hash().left(RNS::Type::Reticulum::TRUNCATED_HASHLENGTH / 8));
    RNS::Packet proof_packet(proof_destination, proof);
    proof_packet.packet_type(RNS::Type::Packet::PROOF);
    proof_packet.pack();
    RNS::Transport::_receipts.push_back(receipt);
    interface.handle_incoming(proof_packet.raw());
    return receipt.status() == RNS::Type::PacketReceipt::DELIVERED ? 0 : 5;
}
