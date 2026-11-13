#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace journal {

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);

// Append-only journal on flash that survives a power cut at any instant.
//
// Record: magic u32 | length u32 | seq u64 | t_us u64 | payload | crc32 u32 (over length..payload),
// little-endian. Records go into segment files "seg-<first seq>.log"; a new segment starts when
// the current one reaches segment_bytes.
//
// After a power cut the tail of the last segment may hold a torn record (partly written, or
// written but with stale bytes in it). open() scans the last segment, keeps every record whose
// magic, length and CRC check out, and truncates the file after the last good one. Records that
// were synced are never lost; records appended after the last sync may be.
//
// The reader's position (what the phone has acknowledged) is stored separately in "ack",
// replaced atomically: write "ack.tmp", fsync, rename, fsync the directory.
struct Options {
    size_t segment_bytes = 256 * 1024;
    size_t max_total_bytes = 16 * 1024 * 1024;  // oldest segments are dropped beyond this, acked or not
    uint32_t sync_every = 32;                   // fdatasync after this many appends (1 = every record)
};

struct Record {
    uint64_t seq = 0;
    uint64_t t_us = 0;
    std::vector<uint8_t> payload;
};

struct RecoveryReport {
    size_t segments = 0;
    uint64_t records = 0;
    uint64_t truncated_bytes = 0;  // torn tail removed from the last segment
};

class Journal {
public:
    Journal(std::filesystem::path dir, Options opt = {});
    ~Journal();
    Journal(const Journal &) = delete;
    Journal &operator=(const Journal &) = delete;

    // Opens (and recovers) the journal in dir, creating it if needed. Throws std::runtime_error.
    RecoveryReport open();

    uint64_t append(std::span<const uint8_t> payload, uint64_t t_us);  // returns the record's seq
    void sync();

    // Records with seq >= from, in order, until fn returns false.
    void read_from(uint64_t from, const std::function<bool(const Record &)> &fn) const;

    void ack(uint64_t seq);  // everything up to and including seq has been delivered
    uint64_t acked() const { return acked_; }
    uint64_t next_seq() const { return next_seq_; }
    uint64_t dropped() const { return dropped_; }  // unacked records lost to the size cap

    static constexpr uint32_t kMagic = 0x4C4A3931;  // "19JL"
    static constexpr size_t kHeader = 4 + 4 + 8 + 8;
    static constexpr size_t kMaxPayload = 4096;

private:
    struct Segment {
        uint64_t first_seq;
        std::filesystem::path path;
        uint64_t bytes;
    };
    void start_segment();
    void enforce_size_cap();
    void load_ack();
    static uint64_t scan(const std::filesystem::path &p, uint64_t *last_seq, uint64_t *count,
                         const std::function<bool(const Record &)> *fn);

    std::filesystem::path dir_;
    Options opt_;
    std::vector<Segment> segments_;
    int fd_ = -1;
    uint64_t next_seq_ = 1;
    uint64_t acked_ = 0;
    uint64_t dropped_ = 0;
    uint32_t unsynced_ = 0;
};

}  // namespace journal
