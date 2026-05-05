// HDLC framing for TCP transport — matches Python RNS TCPInterface
// (RNS/Interfaces/TCPInterface.py).
//
// Wire format: [FLAG][escaped_payload][FLAG]
//   FLAG = 0x7E, ESC = 0x7D, ESC_MASK = 0x20
//
// Vendored from pyxis/src/HDLC.h verbatim — pure-C++ no platform deps so
// builds clean on host.

#pragma once

#include <Bytes.h>
#include <stdint.h>

namespace bridge {

class HDLC {
public:
    static constexpr uint8_t FLAG = 0x7E;
    static constexpr uint8_t ESC = 0x7D;
    static constexpr uint8_t ESC_MASK = 0x20;

    static RNS::Bytes escape(const RNS::Bytes& data) {
        RNS::Bytes result;
        result.reserve(data.size() * 2);
        for (size_t i = 0; i < data.size(); ++i) {
            uint8_t byte = data.data()[i];
            if (byte == ESC) {
                result.append(ESC);
                result.append(static_cast<uint8_t>(ESC ^ ESC_MASK));
            } else if (byte == FLAG) {
                result.append(ESC);
                result.append(static_cast<uint8_t>(FLAG ^ ESC_MASK));
            } else {
                result.append(byte);
            }
        }
        return result;
    }

    static RNS::Bytes unescape(const RNS::Bytes& data) {
        RNS::Bytes result;
        result.reserve(data.size());
        bool in_escape = false;
        for (size_t i = 0; i < data.size(); ++i) {
            uint8_t byte = data.data()[i];
            if (in_escape) {
                result.append(static_cast<uint8_t>(byte ^ ESC_MASK));
                in_escape = false;
            } else if (byte == ESC) {
                in_escape = true;
            } else {
                result.append(byte);
            }
        }
        if (in_escape) return RNS::Bytes();  // invalid trailing escape
        return result;
    }

    static RNS::Bytes frame(const RNS::Bytes& data) {
        RNS::Bytes escaped = escape(data);
        RNS::Bytes framed;
        framed.reserve(escaped.size() + 2);
        framed.append(FLAG);
        framed.append(escaped);
        framed.append(FLAG);
        return framed;
    }
};

}  // namespace bridge
