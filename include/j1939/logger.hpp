#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "j1939/frame.hpp"
#include "j1939/signals.hpp"
#include "j1939/tp.hpp"
#include "journal/journal.hpp"

namespace j1939 {

// candump -L line: "(1699999999.123456) can0 18FEF100#11223344AABBCCDD". 8 hex digits = 29-bit id.
// Returns nullopt for standard-id frames, remote frames and malformed lines.
std::optional<Frame> parse_candump(std::string_view line);
std::string format_candump(const Frame &f, std::string_view ifname);

// Minimum interval per PGN: keeps a 20 ms EEC1 from filling the journal (the app needs ~1 Hz).
class Throttle {
public:
    void set(uint32_t pgn, uint64_t min_interval_us) { min_[pgn] = min_interval_us; }
    bool admit(uint32_t pgn, uint64_t t_us);

private:
    std::map<uint32_t, uint64_t> min_;
    std::map<uint32_t, uint64_t> last_;
};

// Bus speed detection without transmitting: the interface is brought up in listen-only mode at
// each candidate bitrate for a short window; at the wrong rate a listen-only node sees only
// error frames (it cannot destroy traffic because it never drives the bus, not even an ACK).
struct ProbeResult {
    uint32_t bitrate = 0;
    uint32_t frames_ok = 0;
    uint32_t error_frames = 0;
};
// Picks the candidate with valid traffic and an error ratio under 1 %. 0 = undecided (silent
// bus, or both rates noisy: try again later, never guess).
uint32_t choose_bitrate(const std::vector<ProbeResult> &probes, uint32_t min_frames = 20);

// Journal payload: kind (1 = single frame, 2 = reassembled multi-packet) | id u32 | len u16 | data.
std::vector<uint8_t> encode_event(uint8_t kind, uint32_t id, const uint8_t *data, size_t len);

struct LoggerStats {
    uint64_t frames = 0;
    uint64_t ignored = 0;    // PGNs we do not keep
    uint64_t throttled = 0;
    uint64_t journaled = 0;
};

// Frame in -> TP reassembly -> PGN filter -> throttle -> journal + latest vehicle state.
class Logger {
public:
    Logger(journal::Journal *j, std::set<uint32_t> pgns);
    Throttle &throttle() { return throttle_; }
    void feed(const Frame &f);

    const VehicleState &state() const { return state_; }
    const LoggerStats &stats() const { return stats_; }
    const TpStats &tp_stats() const { return tp_.stats(); }

private:
    journal::Journal *journal_;
    std::set<uint32_t> pgns_;
    Throttle throttle_;
    TpReassembler tp_;
    VehicleState state_;
    LoggerStats stats_;
};

// Delivery to the phone with replay: batches start after the last acknowledged record; an
// acknowledgement moves the durable cursor. After a disconnect, or a reboot, delivery resumes from
// the cursor, so the phone may see a record twice (it de-duplicates by seq) but never misses one.
// The transport (BLE GATT notifications, or TCP over Wi-Fi) only moves the bytes.
class SyncSession {
public:
    explicit SyncSession(journal::Journal *j) : journal_(j) { restart(); }
    void restart() { next_ = journal_->acked() + 1; }  // on (re)connection
    // Up to max_records records after the last one handed out.
    std::vector<journal::Record> next_batch(size_t max_records);
    void ack(uint64_t seq) { journal_->ack(seq); }

private:
    journal::Journal *journal_;
    uint64_t next_ = 1;
};

}  // namespace j1939
