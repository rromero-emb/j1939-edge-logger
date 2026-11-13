#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "journal/journal.hpp"

namespace fs = std::filesystem;
using journal::Journal;
using journal::Record;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        char tmpl[] = "/tmp/j1939-journal-XXXXXX";
        path = ::mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::vector<uint8_t> payload_for(uint64_t i)
{
    std::vector<uint8_t> p(1 + i % 23);  // variable lengths
    for (size_t k = 0; k < p.size(); k++) {
        p[k] = static_cast<uint8_t>(i * 31 + k);
    }
    return p;
}

std::vector<Record> read_all(const Journal &j, uint64_t from = 1)
{
    std::vector<Record> out;
    j.read_from(from, [&](const Record &r) {
        out.push_back(r);
        return true;
    });
    return out;
}

void copy_truncated(const fs::path &src, const fs::path &dst, uintmax_t len)
{
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    fs::resize_file(dst, len);
}

}  // namespace

TEST(Crc32, KnownVector)
{
    const std::string s = "123456789";
    EXPECT_EQ(journal::crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(s.data()), s.size())),
              0xCBF43926u);
}

TEST(Journal, AppendReadReopen)
{
    TempDir d;
    {
        Journal j(d.path);
        j.open();
        for (uint64_t i = 1; i <= 100; i++) {
            EXPECT_EQ(j.append(payload_for(i), i * 1000), i);
        }
    }
    Journal j(d.path);
    auto rep = j.open();
    EXPECT_EQ(rep.records, 100u);
    EXPECT_EQ(rep.truncated_bytes, 0u);
    auto all = read_all(j);
    ASSERT_EQ(all.size(), 100u);
    EXPECT_EQ(all[41].seq, 42u);
    EXPECT_EQ(all[41].t_us, 42000u);
    EXPECT_EQ(all[41].payload, payload_for(42));
    EXPECT_EQ(j.append(payload_for(101), 0), 101u);  // continues the sequence
}

// The core property: cut the power after ANY byte of the last segment. Recovery must keep exactly
// the records that were completely written before the cut, drop the rest, and accept new appends.
TEST(Journal, PowerCutAtEveryByte)
{
    TempDir d;
    const fs::path src = d.path / "src";
    std::vector<uint64_t> end_offsets;  // file size after each record
    {
        Journal j(src, journal::Options{1 << 20, 16 << 20, 1});
        j.open();
        for (uint64_t i = 1; i <= 12; i++) {
            j.append(payload_for(i), i);
            end_offsets.push_back(fs::file_size(src / "seg-0000000000000001.log"));
        }
    }
    const fs::path seg = src / "seg-0000000000000001.log";
    const uintmax_t size = fs::file_size(seg);
    for (uintmax_t cut = 0; cut <= size; cut++) {
        const fs::path dst = d.path / ("cut" + std::to_string(cut));
        copy_truncated(seg, dst / "seg-0000000000000001.log", cut);
        uint64_t complete = 0;
        while (complete < end_offsets.size() && end_offsets[complete] <= cut) {
            complete++;
        }
        Journal j(dst);
        auto rep = j.open();
        ASSERT_EQ(rep.records, complete) << "cut at byte " << cut;
        auto all = read_all(j);
        ASSERT_EQ(all.size(), complete);
        for (uint64_t i = 0; i < complete; i++) {
            ASSERT_EQ(all[i].payload, payload_for(i + 1));
        }
        // the journal is usable again: the next record follows the last complete one
        EXPECT_EQ(j.append(payload_for(99), 0), complete + 1);
        EXPECT_EQ(read_all(j).size(), complete + 1);
        fs::remove_all(dst);
    }
}

// A cut can also leave a record with the right length but wrong bytes (sector written partly).
TEST(Journal, CorruptedTailRecordIsDropped)
{
    TempDir d;
    {
        Journal j(d.path);
        j.open();
        for (uint64_t i = 1; i <= 5; i++) {
            j.append(payload_for(i), i);
        }
    }
    const fs::path seg = d.path / "seg-0000000000000001.log";
    {
        std::fstream f(seg, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-6, std::ios::end);  // inside the last record's payload
        f.put('\x5A');
    }
    Journal j(d.path);
    auto rep = j.open();
    EXPECT_EQ(rep.records, 4u);
    EXPECT_GT(rep.truncated_bytes, 0u);
}

TEST(Journal, SegmentsRollAndReadAcrossThem)
{
    TempDir d;
    Journal j(d.path, journal::Options{256, 1 << 20, 8});
    j.open();
    for (uint64_t i = 1; i <= 200; i++) {
        j.append(payload_for(i), i);
    }
    size_t segs = 0;
    for (auto &e : fs::directory_iterator(d.path)) {
        segs += e.path().filename().string().rfind("seg-", 0) == 0 ? 1 : 0;
    }
    EXPECT_GT(segs, 5u);
    auto tail = read_all(j, 150);
    ASSERT_EQ(tail.size(), 51u);
    EXPECT_EQ(tail.front().seq, 150u);
    EXPECT_EQ(tail.back().seq, 200u);
}

TEST(Journal, AckIsDurableAndMonotonic)
{
    TempDir d;
    {
        Journal j(d.path);
        j.open();
        for (uint64_t i = 1; i <= 10; i++) {
            j.append(payload_for(i), i);
        }
        j.ack(7);
        j.ack(3);   // going backwards is ignored
        j.ack(50);  // beyond what exists is ignored
        EXPECT_EQ(j.acked(), 7u);
    }
    Journal j(d.path);
    j.open();
    EXPECT_EQ(j.acked(), 7u);
}

TEST(Journal, TornAckFileMeansNothingAcked)
{
    TempDir d;
    {
        Journal j(d.path);
        j.open();
        j.append(payload_for(1), 1);
        j.ack(1);
    }
    fs::resize_file(d.path / "ack", 5);
    Journal j(d.path);
    j.open();
    EXPECT_EQ(j.acked(), 0u);  // duplicates for the phone, never a gap
}

TEST(Journal, SizeCapDropsAckedFirstThenCountsLoss)
{
    TempDir d;
    Journal j(d.path, journal::Options{200, 1000, 1});
    j.open();
    for (uint64_t i = 1; i <= 300; i++) {
        j.append(payload_for(i), i);
    }
    uintmax_t total = 0;
    for (auto &e : fs::directory_iterator(d.path)) {
        total += fs::file_size(e.path());
    }
    EXPECT_LE(total, 1000u + 200u + 64u);  // cap + one segment being filled
    EXPECT_GT(j.dropped(), 0u);            // nothing was acked: the oldest records were lost, and counted
    auto all = read_all(j);
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all.back().seq, 300u);
    EXPECT_EQ(j.dropped() + all.size(), 300u);
}
