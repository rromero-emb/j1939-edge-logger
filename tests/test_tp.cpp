#include <gtest/gtest.h>

#include <string>

#include "j1939/tp.hpp"

using namespace j1939;

namespace {

Frame frame(uint32_t id, std::array<uint8_t, 8> d, uint64_t t)
{
    Frame f;
    f.id = id;
    f.dlc = 8;
    f.data = d;
    f.t_us = t;
    return f;
}

// TP.CM / TP.DT frames for a message from sa to da (da = 0xFF -> BAM)
Frame cm(uint8_t control, uint8_t sa, uint8_t da, uint16_t size, uint32_t pgn, uint64_t t)
{
    const uint8_t packets = static_cast<uint8_t>((size + 6) / 7);
    return frame(encode_id(7, pgn::kTpCm, da, sa),
                 {control, uint8_t(size & 0xFF), uint8_t(size >> 8), packets, 0xFF, uint8_t(pgn & 0xFF),
                  uint8_t((pgn >> 8) & 0xFF), uint8_t(pgn >> 16)},
                 t);
}

Frame dt(uint8_t sa, uint8_t da, uint8_t seq, const std::string &msg, uint64_t t)
{
    std::array<uint8_t, 8> d{seq, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (size_t i = 0; i < 7; i++) {
        size_t at = (seq - 1u) * 7u + i;
        if (at < msg.size()) {
            d[1 + i] = static_cast<uint8_t>(msg[at]);
        }
    }
    return frame(encode_id(7, pgn::kTpDt, da, sa), d, t);
}

const std::string kVin = "1XKAD49X0DJ123456*";  // 18 bytes -> 3 packets

}  // namespace

TEST(Tp, BamReassembly)
{
    TpReassembler r;
    EXPECT_FALSE(r.feed(cm(0x20, 0x00, 0xFF, 18, pgn::kVi, 0)).has_value());
    EXPECT_FALSE(r.feed(dt(0x00, 0xFF, 1, kVin, 50'000)).has_value());
    EXPECT_FALSE(r.feed(dt(0x00, 0xFF, 2, kVin, 100'000)).has_value());
    auto m = r.feed(dt(0x00, 0xFF, 3, kVin, 150'000));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->pgn, pgn::kVi);
    EXPECT_EQ(m->sa, 0x00);
    EXPECT_EQ(std::string(m->data.begin(), m->data.end()), kVin);  // trimmed to size, padding gone
    EXPECT_EQ(r.stats().completed, 1u);
    EXPECT_EQ(r.open_sessions(), 0u);
}

TEST(Tp, InterleavedSessionsFromTwoSources)
{
    TpReassembler r;
    const std::string other = "ABCDEFGHIJKLMNOPQRSTUVW";  // 23 bytes -> 4 packets
    r.feed(cm(0x20, 0x00, 0xFF, 18, pgn::kVi, 0));
    r.feed(cm(0x20, 0x17, 0xFF, 23, 65242, 1000));
    r.feed(dt(0x17, 0xFF, 1, other, 2000));
    r.feed(dt(0x00, 0xFF, 1, kVin, 3000));
    r.feed(dt(0x17, 0xFF, 2, other, 4000));
    r.feed(dt(0x00, 0xFF, 2, kVin, 5000));
    r.feed(dt(0x17, 0xFF, 3, other, 6000));
    auto a = r.feed(dt(0x00, 0xFF, 3, kVin, 7000));
    auto b = r.feed(dt(0x17, 0xFF, 4, other, 8000));
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->pgn, pgn::kVi);
    EXPECT_EQ(b->pgn, 65242u);
    EXPECT_EQ(std::string(b->data.begin(), b->data.end()), other);
}

TEST(Tp, MissingPacketTimesOut)
{
    TpReassembler r;
    r.feed(cm(0x20, 0x00, 0xFF, 18, pgn::kVi, 0));
    r.feed(dt(0x00, 0xFF, 1, kVin, 50'000));
    // packet 2 lost; packet 3 arrives, then silence
    EXPECT_FALSE(r.feed(dt(0x00, 0xFF, 3, kVin, 150'000)).has_value());
    r.expire(150'000 + TpReassembler::kBamGapUs + 1);
    EXPECT_EQ(r.stats().timed_out, 1u);
    EXPECT_EQ(r.open_sessions(), 0u);
    // a late packet 2 has no session any more
    EXPECT_FALSE(r.feed(dt(0x00, 0xFF, 2, kVin, 2'000'000)).has_value());
    EXPECT_EQ(r.stats().rejected, 1u);
}

TEST(Tp, RtsCtsWithRetransmissionAndDuplicates)
{
    TpReassembler r;
    // ECU 0x00 sends VI to 0xF9 (e.g. a reply to a request); we only listen.
    r.feed(cm(0x10, 0x00, 0xF9, 18, pgn::kVi, 0));
    r.feed(frame(encode_id(7, pgn::kTpCm, 0x00, 0xF9), {0x11, 2, 1, 0xFF, 0xFF, 0xE8, 0xFE, 0x00}, 10'000));  // CTS 2 from 1
    r.feed(dt(0x00, 0xF9, 1, kVin, 20'000));
    r.feed(dt(0x00, 0xF9, 1, kVin, 25'000));  // duplicate: does not count twice
    // receiver asks again for packet 2 onwards after a long pause (within T2)
    r.feed(frame(encode_id(7, pgn::kTpCm, 0x00, 0xF9), {0x11, 2, 2, 0xFF, 0xFF, 0xE8, 0xFE, 0x00}, 1'100'000));
    r.feed(dt(0x00, 0xF9, 2, kVin, 1'110'000));
    auto m = r.feed(dt(0x00, 0xF9, 3, kVin, 1'120'000));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->da, 0xF9);
    EXPECT_EQ(std::string(m->data.begin(), m->data.end()), kVin);
}

TEST(Tp, AbortFromReceiverClosesSendersSession)
{
    TpReassembler r;
    r.feed(cm(0x10, 0x00, 0xF9, 18, pgn::kVi, 0));
    r.feed(frame(encode_id(7, pgn::kTpCm, 0x00, 0xF9), {0xFF, 3, 0xFF, 0xFF, 0xFF, 0xE8, 0xFE, 0x00}, 10'000));
    EXPECT_EQ(r.open_sessions(), 0u);
    EXPECT_EQ(r.stats().aborted, 1u);
}

TEST(Tp, InvalidAnnouncementsRejected)
{
    TpReassembler r;
    r.feed(cm(0x20, 0x00, 0xFF, 8, pgn::kVi, 0));     // 8 bytes fit one frame: not TP
    r.feed(cm(0x20, 0x00, 0xFF, 1786, pgn::kVi, 0));  // above 255 x 7
    r.feed(cm(0x20, 0x00, 0x05, 18, pgn::kVi, 0));    // BAM to a specific address
    r.feed(cm(0x10, 0x00, 0xFF, 18, pgn::kVi, 0));    // RTS to global
    Frame bad = cm(0x20, 0x00, 0xFF, 18, pgn::kVi, 0);
    bad.data[3] = 4;  // packet count does not match size
    r.feed(bad);
    EXPECT_EQ(r.stats().rejected, 5u);
    EXPECT_EQ(r.open_sessions(), 0u);
}

TEST(Tp, NewAnnouncementReplacesUnfinishedSession)
{
    TpReassembler r;
    r.feed(cm(0x20, 0x00, 0xFF, 18, pgn::kVi, 0));
    r.feed(dt(0x00, 0xFF, 1, kVin, 50'000));
    r.feed(cm(0x20, 0x00, 0xFF, 18, pgn::kVi, 100'000));
    EXPECT_EQ(r.stats().aborted, 1u);
    r.feed(dt(0x00, 0xFF, 1, kVin, 150'000));
    r.feed(dt(0x00, 0xFF, 2, kVin, 200'000));
    EXPECT_TRUE(r.feed(dt(0x00, 0xFF, 3, kVin, 250'000)).has_value());
}
