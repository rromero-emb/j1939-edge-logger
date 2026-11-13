#include "journal/journal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace journal {
namespace fs = std::filesystem;

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc)
{
    // CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), bitwise: small and fast enough here.
    crc = ~crc;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

namespace {

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++) {
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    }
}
void put64(std::vector<uint8_t> &v, uint64_t x)
{
    for (int i = 0; i < 8; i++) {
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    }
}
uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t{get32(p + 4)} << 32);
}

[[noreturn]] void fail(const std::string &what)
{
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

void fsync_dir(const fs::path &dir)
{
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail("open dir " + dir.string());
    }
    ::fsync(fd);
    ::close(fd);
}

std::vector<uint8_t> read_file(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

Journal::Journal(fs::path dir, Options opt) : dir_(std::move(dir)), opt_(opt) {}

Journal::~Journal()
{
    if (fd_ >= 0) {
        ::fdatasync(fd_);
        ::close(fd_);
    }
}

// Returns the byte length of the valid prefix of the segment. Calls fn for each valid record.
uint64_t Journal::scan(const fs::path &p, uint64_t *last_seq, uint64_t *count,
                       const std::function<bool(const Record &)> *fn)
{
    const std::vector<uint8_t> buf = read_file(p);
    size_t off = 0;
    while (off + kHeader + 4 <= buf.size()) {
        const uint8_t *h = &buf[off];
        if (get32(h) != kMagic) {
            break;
        }
        const uint32_t len = get32(h + 4);
        if (len > kMaxPayload || off + kHeader + len + 4 > buf.size()) {
            break;
        }
        const uint32_t want = get32(h + kHeader + len);
        if (crc32(std::span<const uint8_t>(h + 4, kHeader - 4 + len)) != want) {
            break;
        }
        Record r;
        r.seq = get64(h + 8);
        r.t_us = get64(h + 16);
        if (last_seq != nullptr) {
            if (*count > 0 && r.seq != *last_seq + 1) {
                break;  // sequence must be contiguous: anything else is stale data
            }
            *last_seq = r.seq;
        }
        if (count != nullptr) {
            (*count)++;
        }
        if (fn != nullptr) {
            r.payload.assign(h + kHeader, h + kHeader + len);
            if (!(*fn)(r)) {
                return off;
            }
        }
        off += kHeader + len + 4;
    }
    return off;
}

RecoveryReport Journal::open()
{
    fs::create_directories(dir_);
    segments_.clear();
    for (const auto &e : fs::directory_iterator(dir_)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("seg-", 0) == 0 && name.size() == 4 + 16 + 4) {
            segments_.push_back({std::stoull(name.substr(4, 16), nullptr, 16), e.path(), fs::file_size(e.path())});
        }
    }
    std::sort(segments_.begin(), segments_.end(), [](auto &a, auto &b) { return a.first_seq < b.first_seq; });

    RecoveryReport rep;
    rep.segments = segments_.size();
    next_seq_ = 1;
    if (!segments_.empty()) {
        Segment &last = segments_.back();
        uint64_t last_seq = 0;
        uint64_t count = 0;
        const uint64_t good = scan(last.path, &last_seq, &count, nullptr);
        if (good < last.bytes) {
            rep.truncated_bytes = last.bytes - good;
            if (::truncate(last.path.c_str(), static_cast<off_t>(good)) != 0) {
                fail("truncate " + last.path.string());
            }
            last.bytes = good;
        }
        next_seq_ = (count > 0) ? last_seq + 1 : last.first_seq;
        rep.records = next_seq_ - segments_.front().first_seq;
    }
    load_ack();
    if (segments_.empty()) {
        start_segment();
    } else {
        fd_ = ::open(segments_.back().path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd_ < 0) {
            fail("open " + segments_.back().path.string());
        }
        ::fsync(fd_);  // make the truncation durable
    }
    return rep;
}

void Journal::start_segment()
{
    if (fd_ >= 0) {
        ::fdatasync(fd_);
        ::close(fd_);
    }
    char name[32];
    std::snprintf(name, sizeof name, "seg-%016llx.log", static_cast<unsigned long long>(next_seq_));
    const fs::path p = dir_ / name;
    fd_ = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        fail("create " + p.string());
    }
    fsync_dir(dir_);  // the new file name must survive a power cut too
    segments_.push_back({next_seq_, p, 0});
}

uint64_t Journal::append(std::span<const uint8_t> payload, uint64_t t_us)
{
    if (payload.size() > kMaxPayload) {
        throw std::invalid_argument("journal payload too large");
    }
    if (segments_.back().bytes >= opt_.segment_bytes) {
        start_segment();
        enforce_size_cap();
    }
    std::vector<uint8_t> rec;
    rec.reserve(kHeader + payload.size() + 4);
    put32(rec, kMagic);
    put32(rec, static_cast<uint32_t>(payload.size()));
    put64(rec, next_seq_);
    put64(rec, t_us);
    rec.insert(rec.end(), payload.begin(), payload.end());
    put32(rec, crc32(std::span<const uint8_t>(rec.data() + 4, rec.size() - 4)));

    // One write() per record: on a power cut the kernel may still persist only part of it, which
    // recovery handles; it never interleaves two records.
    size_t done = 0;
    while (done < rec.size()) {
        ssize_t n = ::write(fd_, rec.data() + done, rec.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write journal");
        }
        done += static_cast<size_t>(n);
    }
    segments_.back().bytes += rec.size();
    if (++unsynced_ >= opt_.sync_every) {
        sync();
    }
    return next_seq_++;
}

void Journal::sync()
{
    if (fd_ >= 0 && unsynced_ > 0) {
        ::fdatasync(fd_);
    }
    unsynced_ = 0;
}

void Journal::enforce_size_cap()
{
    uint64_t total = 0;
    for (const auto &s : segments_) {
        total += s.bytes;
    }
    // Delete whole segments from the oldest: first those fully acknowledged, then, if the phone
    // has not synced for a long time, unacked ones too (counted in dropped()).
    while (segments_.size() > 1) {
        const Segment &oldest = segments_.front();
        const uint64_t next_first = segments_[1].first_seq;
        const bool fully_acked = (next_first - 1) <= acked_;
        if (!fully_acked && total <= opt_.max_total_bytes) {
            break;
        }
        if (!fully_acked) {
            dropped_ += next_first - std::max(oldest.first_seq, acked_ + 1);
        }
        total -= oldest.bytes;
        fs::remove(oldest.path);
        segments_.erase(segments_.begin());
    }
}

void Journal::read_from(uint64_t from, const std::function<bool(const Record &)> &fn) const
{
    for (size_t i = 0; i < segments_.size(); i++) {
        const bool last = (i + 1 == segments_.size());
        if (!last && segments_[i + 1].first_seq <= from) {
            continue;  // whole segment before `from`
        }
        bool keep_going = true;
        const std::function<bool(const Record &)> filter = [&](const Record &r) {
            if (r.seq < from) {
                return true;
            }
            keep_going = fn(r);
            return keep_going;
        };
        scan(segments_[i].path, nullptr, nullptr, &filter);
        if (!keep_going) {
            return;
        }
    }
}

void Journal::load_ack()
{
    acked_ = 0;
    const auto buf = read_file(dir_ / "ack");
    if (buf.size() == 12 && crc32(std::span<const uint8_t>(buf.data(), 8)) == get32(&buf[8])) {
        acked_ = get64(buf.data());
    }
    // A torn or missing ack file means "nothing acknowledged": the phone gets duplicates, not gaps.
}

void Journal::ack(uint64_t seq)
{
    if (seq <= acked_ || seq >= next_seq_) {
        return;
    }
    std::vector<uint8_t> buf;
    put64(buf, seq);
    put32(buf, crc32(std::span<const uint8_t>(buf.data(), 8)));
    const fs::path tmp = dir_ / "ack.tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fail("open ack.tmp");
    }
    if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) {
        ::close(fd);
        fail("write ack.tmp");
    }
    ::fsync(fd);
    ::close(fd);
    fs::rename(tmp, dir_ / "ack");
    fsync_dir(dir_);
    acked_ = seq;
}

}  // namespace journal
