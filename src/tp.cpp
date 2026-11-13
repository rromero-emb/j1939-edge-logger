#include "j1939/tp.hpp"

namespace j1939 {
namespace {
constexpr uint8_t kRts = 0x10;
constexpr uint8_t kCts = 0x11;
constexpr uint8_t kEoma = 0x13;
constexpr uint8_t kBam = 0x20;
constexpr uint8_t kAbort = 0xFF;
}  // namespace

std::optional<TpMessage> TpReassembler::feed(const Frame &f)
{
    expire(f.t_us);
    const Id id = decode_id(f.id);
    if (id.pgn == pgn::kTpCm && f.dlc == 8) {
        return on_cm(id, f);
    }
    if (id.pgn == pgn::kTpDt && f.dlc == 8) {
        return on_dt(id, f);
    }
    return std::nullopt;
}

void TpReassembler::expire(uint64_t now_us)
{
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const uint64_t limit = it->second.bam ? kBamGapUs : kRtsGapUs;
        if (now_us > it->second.last_us && (now_us - it->second.last_us) > limit) {
            stats_.timed_out++;
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<TpMessage> TpReassembler::on_cm(const Id &id, const Frame &f)
{
    const uint8_t control = f.data[0];
    const Key key{id.sa, id.da};
    if (control == kBam || control == kRts) {
        // BAM must go to the global address, RTS to a specific one.
        if ((control == kBam) != (id.da == kGlobal)) {
            stats_.rejected++;
            return std::nullopt;
        }
        const uint16_t size = static_cast<uint16_t>(f.data[1] | (f.data[2] << 8));
        const uint8_t packets = f.data[3];
        if (size < 9 || size > kMaxSize || packets != (size + 6) / 7) {
            stats_.rejected++;
            return std::nullopt;
        }
        if (sessions_.erase(key) > 0) {
            stats_.aborted++;  // a new announcement replaces an unfinished one
        }
        Session s;
        s.bam = (control == kBam);
        s.pgn = f.data[5] | (static_cast<uint32_t>(f.data[6]) << 8) | (static_cast<uint32_t>(f.data[7]) << 16);
        s.size = size;
        s.packets = packets;
        s.last_us = f.t_us;
        s.data.assign(static_cast<size_t>(packets) * 7, 0xFF);
        s.have.assign(packets, false);
        sessions_[key] = std::move(s);
        return std::nullopt;
    }
    if (control == kCts) {
        // From the receiver (sa) to the sender (da): keeps the sender's session alive.
        auto it = sessions_.find(Key{id.da, id.sa});
        if (it != sessions_.end()) {
            it->second.last_us = f.t_us;
        }
        return std::nullopt;
    }
    if (control == kAbort) {
        // Either side may abort.
        size_t n = sessions_.erase(key) + sessions_.erase(Key{id.da, id.sa});
        stats_.aborted += static_cast<uint32_t>(n);
        return std::nullopt;
    }
    // kEoma: the message was already emitted when its last packet arrived.
    (void)kEoma;
    return std::nullopt;
}

std::optional<TpMessage> TpReassembler::on_dt(const Id &id, const Frame &f)
{
    auto it = sessions_.find(Key{id.sa, id.da});
    if (it == sessions_.end()) {
        stats_.rejected++;
        return std::nullopt;
    }
    Session &s = it->second;
    const uint8_t seq = f.data[0];
    if (seq == 0 || seq > s.packets) {
        stats_.rejected++;
        return std::nullopt;
    }
    s.last_us = f.t_us;
    const size_t at = static_cast<size_t>(seq - 1) * 7;
    for (size_t i = 0; i < 7; i++) {
        s.data[at + i] = f.data[1 + i];
    }
    if (!s.have[seq - 1]) {
        s.have[seq - 1] = true;
        s.received++;
    }
    if (s.received < s.packets) {
        return std::nullopt;
    }
    TpMessage m;
    m.pgn = s.pgn;
    m.sa = id.sa;
    m.da = id.da;
    m.t_us = f.t_us;
    m.data.assign(s.data.begin(), s.data.begin() + s.size);
    sessions_.erase(it);
    stats_.completed++;
    return m;
}

}  // namespace j1939
