#pragma once
#include <array>
#include <cstdint>

namespace j1939 {

// One classic CAN frame with a 29-bit identifier, as received from the bus.
struct Frame {
    uint64_t t_us = 0;  // receive time, microseconds (monotonic or wall clock, caller's choice)
    uint32_t id = 0;    // 29-bit identifier (without the CAN_EFF_FLAG)
    uint8_t dlc = 0;
    std::array<uint8_t, 8> data{};
};

// J1939-21 identifier: priority (3) | EDP (1) | DP (1) | PF (8) | PS (8) | SA (8).
// PF < 240 (PDU1): PS is a destination address and is not part of the PGN.
// PF >= 240 (PDU2): PS is the group extension, the message is broadcast.
struct Id {
    uint8_t priority = 0;
    uint32_t pgn = 0;
    uint8_t sa = 0;
    uint8_t da = 0xFF;  // 0xFF = global (always for PDU2)
};

constexpr uint8_t kGlobal = 0xFF;
constexpr uint8_t kNullAddress = 0xFE;

constexpr Id decode_id(uint32_t can_id)
{
    Id id;
    id.sa = static_cast<uint8_t>(can_id & 0xFFu);
    const uint8_t ps = static_cast<uint8_t>((can_id >> 8) & 0xFFu);
    const uint8_t pf = static_cast<uint8_t>((can_id >> 16) & 0xFFu);
    const uint32_t dp = (can_id >> 24) & 0x3u;  // EDP and DP
    id.priority = static_cast<uint8_t>((can_id >> 26) & 0x7u);
    if (pf < 240) {
        id.pgn = (dp << 16) | (static_cast<uint32_t>(pf) << 8);
        id.da = ps;
    } else {
        id.pgn = (dp << 16) | (static_cast<uint32_t>(pf) << 8) | ps;
        id.da = kGlobal;
    }
    return id;
}

constexpr uint32_t encode_id(uint8_t priority, uint32_t pgn, uint8_t da, uint8_t sa)
{
    const uint32_t pf = (pgn >> 8) & 0xFFu;
    const uint32_t ps = (pf < 240) ? da : (pgn & 0xFFu);
    return (static_cast<uint32_t>(priority & 0x7u) << 26) | (((pgn >> 16) & 0x3u) << 24) | (pf << 16) | (ps << 8) | sa;
}

// PGNs used here (J1939-71 / J1939-21).
namespace pgn {
constexpr uint32_t kRequest = 59904;       // 0xEA00
constexpr uint32_t kAddressClaim = 60928;  // 0xEE00
constexpr uint32_t kTpCm = 60416;          // 0xEC00 transport protocol, connection management
constexpr uint32_t kTpDt = 60160;          // 0xEB00 transport protocol, data transfer
constexpr uint32_t kEec1 = 61444;          // electronic engine controller 1 (engine speed)
constexpr uint32_t kCcvs = 65265;          // cruise control / vehicle speed (wheel-based speed)
constexpr uint32_t kHrvd = 65217;          // high-resolution vehicle distance
constexpr uint32_t kVd = 65248;            // vehicle distance
constexpr uint32_t kHours = 65253;         // engine hours, revolutions
constexpr uint32_t kVi = 65260;            // vehicle identification (VIN), multi-packet
}  // namespace pgn

}  // namespace j1939
