// Direct parser regression for upstream microLXMF. No radio is involved.
#include "LXMF/LXMessage.h"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const size_t depth = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 20000;
    // Destination hash, source hash, and an intentionally invalid signature.
    std::vector<uint8_t> wire(96, 0);
    // [float64 timestamp, empty title, empty content, {1: nested value}]
    const uint8_t prefix[] = {
        0x94, 0xcb, 0, 0, 0, 0, 0, 0, 0, 0,
        0xc4, 0, 0xc4, 0, 0x81, 1,
    };
    wire.insert(wire.end(), prefix, prefix + sizeof(prefix));
    wire.insert(wire.end(), depth, 0x91);  // one-element arrays
    wire.push_back(0xc0);                   // innermost nil

    std::fprintf(stderr, "lxmf_bytes=%zu depth=%zu\n", wire.size(), depth);

    auto message = LXMF::LXMessage::unpack_from_bytes(
        RNS::Bytes(wire.data(), wire.size()), LXMF::Type::Message::DIRECT, true);
    return message.destination_hash().size() == 0 ? 0 : 1;
}
