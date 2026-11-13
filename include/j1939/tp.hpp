#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "j1939/frame.hpp"

namespace j1939 {

// Passive reassembly of the J1939-21 transport protocol, as a silent listener: the logger never
// sends CTS or acknowledgements, it only reads what other nodes exchange.
//  - BAM (broadcast): TP.CM 0x20 announces size/packets/PGN, then TP.DT packets 1..n.
//  - RTS/CTS (to one destination): TP.CM 0x10, the receiver's CTS (0x11) asks for packets, the
//    sender may resend any packet the receiver asks for again; EoMA (0x13) or Abort (0xFF) ends it.
// Sessions are keyed by (source, destination). Data packets are stored by sequence number, so
// a resent packet overwrites, duplicates do not count twice, and the message is emitted as soon
// as every packet is in.
struct TpMessage {
    uint32_t pgn = 0;
    uint8_t sa = 0;
    uint8_t da = kGlobal;
    uint64_t t_us = 0;  // time of the last packet
    std::vector<uint8_t> data;
};

struct TpStats {
    uint32_t completed = 0;
    uint32_t aborted = 0;    // abort message, or a new announcement replacing a session
    uint32_t timed_out = 0;
    uint32_t rejected = 0;   // invalid announcement (size/packet count), or data without a session
};

class TpReassembler {
public:
    // J1939-21 T1 (750 ms) is the receiver timeout between packets; a passive listener also has to
    // allow for the CTS round trip (T2, 1250 ms) before the next packet of an RTS/CTS session.
    static constexpr uint64_t kBamGapUs = 750'000;
    static constexpr uint64_t kRtsGapUs = 1'250'000;
    static constexpr uint32_t kMaxSize = 1785;  // 255 packets x 7 bytes

    // Feeds any frame; returns a message when one completes. Non-TP frames are ignored.
    std::optional<TpMessage> feed(const Frame &f);
    // Drops sessions that have been silent too long. Called by feed() as well.
    void expire(uint64_t now_us);

    const TpStats &stats() const { return stats_; }
    size_t open_sessions() const { return sessions_.size(); }

private:
    struct Session {
        bool bam = false;
        uint32_t pgn = 0;
        uint16_t size = 0;
        uint8_t packets = 0;
        uint64_t last_us = 0;
        std::vector<uint8_t> data;
        std::vector<bool> have;
        uint8_t received = 0;
    };
    using Key = std::pair<uint8_t, uint8_t>;  // (sa, da)

    std::optional<TpMessage> on_cm(const Id &id, const Frame &f);
    std::optional<TpMessage> on_dt(const Id &id, const Frame &f);

    std::map<Key, Session> sessions_;
    TpStats stats_;
};

}  // namespace j1939
