#include "j1939/logger.hpp"

#include <charconv>
#include <cstdio>

namespace j1939 {
namespace {

int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace

std::optional<Frame> parse_candump(std::string_view line)
{
    if (line.size() < 4 || line[0] != '(') {
        return std::nullopt;
    }
    const size_t close = line.find(')');
    const size_t dot = line.find('.');
    if (close == std::string_view::npos || dot == std::string_view::npos || dot > close || close - dot != 7) {
        return std::nullopt;
    }
    uint64_t sec = 0;
    uint64_t usec = 0;
    if (std::from_chars(line.data() + 1, line.data() + dot, sec).ec != std::errc() ||
        std::from_chars(line.data() + dot + 1, line.data() + close, usec).ec != std::errc()) {
        return std::nullopt;
    }
    const size_t sp = line.find(' ', close + 2);
    if (sp == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = line.substr(sp + 1);
    while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n' || rest.back() == ' ')) {
        rest.remove_suffix(1);
    }
    const size_t hash = rest.find('#');
    if (hash != 8) {
        return std::nullopt;  // standard 11-bit id, or malformed
    }
    Frame f;
    f.t_us = sec * 1'000'000 + usec;
    for (size_t i = 0; i < 8; i++) {
        int v = hexval(rest[i]);
        if (v < 0) {
            return std::nullopt;
        }
        f.id = (f.id << 4) | static_cast<uint32_t>(v);
    }
    std::string_view hex = rest.substr(hash + 1);
    if (!hex.empty() && hex[0] == 'R') {
        return std::nullopt;  // remote frame
    }
    if (hex.size() % 2 != 0 || hex.size() > 16 || f.id > 0x1FFFFFFFu) {
        return std::nullopt;
    }
    f.dlc = static_cast<uint8_t>(hex.size() / 2);
    for (size_t i = 0; i < f.dlc; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        f.data[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return f;
}

std::string format_candump(const Frame &f, std::string_view ifname)
{
    char buf[96];
    int n = std::snprintf(buf, sizeof buf, "(%llu.%06llu) %.*s %08X#", static_cast<unsigned long long>(f.t_us / 1'000'000),
                          static_cast<unsigned long long>(f.t_us % 1'000'000), static_cast<int>(ifname.size()),
                          ifname.data(), f.id);
    std::string s(buf, static_cast<size_t>(n));
    for (size_t i = 0; i < f.dlc; i++) {
        std::snprintf(buf, sizeof buf, "%02X", f.data[i]);
        s += buf;
    }
    return s;
}

bool Throttle::admit(uint32_t pgn, uint64_t t_us)
{
    auto m = min_.find(pgn);
    if (m == min_.end()) {
        return true;
    }
    auto l = last_.find(pgn);
    if (l != last_.end() && t_us >= l->second && t_us - l->second < m->second) {
        return false;
    }
    last_[pgn] = t_us;
    return true;
}

uint32_t choose_bitrate(const std::vector<ProbeResult> &probes, uint32_t min_frames)
{
    uint32_t chosen = 0;
    for (const auto &p : probes) {
        const uint64_t total = uint64_t{p.frames_ok} + p.error_frames;
        const bool clean = p.frames_ok >= min_frames && p.error_frames * 100ULL < total;
        if (clean) {
            if (chosen != 0) {
                return 0;  // two rates look valid: something is wrong, do not guess
            }
            chosen = p.bitrate;
        }
    }
    return chosen;
}

std::vector<uint8_t> encode_event(uint8_t kind, uint32_t id, const uint8_t *data, size_t len)
{
    std::vector<uint8_t> v;
    v.reserve(7 + len);
    v.push_back(kind);
    for (int i = 0; i < 4; i++) {
        v.push_back(static_cast<uint8_t>(id >> (8 * i)));
    }
    v.push_back(static_cast<uint8_t>(len & 0xFF));
    v.push_back(static_cast<uint8_t>(len >> 8));
    v.insert(v.end(), data, data + len);
    return v;
}

Logger::Logger(journal::Journal *j, std::set<uint32_t> pgns) : journal_(j), pgns_(std::move(pgns)) {}

void Logger::feed(const Frame &f)
{
    stats_.frames++;
    const Id id = decode_id(f.id);
    if (id.pgn == pgn::kTpCm || id.pgn == pgn::kTpDt) {
        auto m = tp_.feed(f);
        if (m && pgns_.count(m->pgn) != 0) {
            state_.update(m->pgn, m->data, m->t_us);
            if (journal_ != nullptr) {
                const uint32_t pseudo_id = encode_id(id.priority, m->pgn, m->da, m->sa);
                journal_->append(encode_event(2, pseudo_id, m->data.data(), m->data.size()), m->t_us);
                stats_.journaled++;
            }
        }
        return;
    }
    tp_.expire(f.t_us);
    if (pgns_.count(id.pgn) == 0) {
        stats_.ignored++;
        return;
    }
    state_.update(id.pgn, std::span<const uint8_t>(f.data.data(), f.dlc), f.t_us);
    if (!throttle_.admit(id.pgn, f.t_us)) {
        stats_.throttled++;
        return;
    }
    if (journal_ != nullptr) {
        journal_->append(encode_event(1, f.id, f.data.data(), f.dlc), f.t_us);
        stats_.journaled++;
    }
}

std::vector<journal::Record> SyncSession::next_batch(size_t max_records)
{
    std::vector<journal::Record> out;
    if (max_records == 0) {
        return out;
    }
    journal_->read_from(next_, [&](const journal::Record &r) {
        out.push_back(r);
        return out.size() < max_records;
    });
    if (!out.empty()) {
        next_ = out.back().seq + 1;
    }
    return out;
}

}  // namespace j1939
