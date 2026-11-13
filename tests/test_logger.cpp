#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>

#include "j1939/logger.hpp"

using namespace j1939;
namespace fs = std::filesystem;

TEST(Candump, ParseAndFormat)
{
    auto f = parse_candump("(1700000000.123456) can0 18FEF100#FF005AFFFFFFFFFF");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->t_us, 1700000000123456ULL);
    EXPECT_EQ(f->id, 0x18FEF100u);
    EXPECT_EQ(f->dlc, 8);
    EXPECT_EQ(f->data[2], 0x5A);
    EXPECT_EQ(format_candump(*f, "can0"), "(1700000000.123456) can0 18FEF100#FF005AFFFFFFFFFF");
    auto r = parse_candump("(1700000000.000001) can0 18EA00F9#E5FE00");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->dlc, 3);
}

TEST(Candump, RejectsWhatIsNotJ1939)
{
    EXPECT_FALSE(parse_candump("(1700000000.123456) can0 123#11").has_value());      // 11-bit id
    EXPECT_FALSE(parse_candump("(1700000000.123456) can0 18FEF100#R").has_value());  // remote
    EXPECT_FALSE(parse_candump("(1700000000.12) can0 18FEF100#11").has_value());     // bad time
    EXPECT_FALSE(parse_candump("(1700000000.123456) can0 18FEF100#1").has_value());  // odd hex
    EXPECT_FALSE(parse_candump("(1700000000.123456) can0 3FFEF100#11").has_value()); // > 29 bits
    EXPECT_FALSE(parse_candump("garbage").has_value());
}

TEST(Throttle, PerPgnInterval)
{
    Throttle t;
    t.set(pgn::kEec1, 1'000'000);
    EXPECT_TRUE(t.admit(pgn::kEec1, 0));
    EXPECT_FALSE(t.admit(pgn::kEec1, 20'000));
    EXPECT_FALSE(t.admit(pgn::kEec1, 999'999));
    EXPECT_TRUE(t.admit(pgn::kEec1, 1'000'000));
    EXPECT_TRUE(t.admit(pgn::kCcvs, 5));  // no limit configured
    EXPECT_TRUE(t.admit(pgn::kCcvs, 6));
}

TEST(Bitrate, ChoosesOnlyAClearWinner)
{
    EXPECT_EQ(choose_bitrate({{250000, 900, 0}, {500000, 0, 340}}), 250000u);
    EXPECT_EQ(choose_bitrate({{250000, 0, 500}, {500000, 1200, 3}}), 500000u);
    EXPECT_EQ(choose_bitrate({{250000, 0, 0}, {500000, 0, 0}}), 0u);        // silent bus (ignition off)
    EXPECT_EQ(choose_bitrate({{250000, 10, 0}, {500000, 0, 0}}), 0u);       // too little traffic
    EXPECT_EQ(choose_bitrate({{250000, 900, 100}, {500000, 0, 300}}), 0u);  // 10 % errors: noisy
}

TEST(Logger, ThrottlesJournalsAndKeepsLatestState)
{
    char tmpl[] = "/tmp/j1939-logger-XXXXXX";
    fs::path dir = ::mkdtemp(tmpl);
    {
        journal::Journal j(dir);
        j.open();
        Logger lg(&j, {pgn::kEec1});
        lg.throttle().set(pgn::kEec1, 1'000'000);
        for (uint64_t t = 0; t < 3'000'000; t += 20'000) {  // 3 s of EEC1 at 50 Hz
            Frame f;
            f.id = 0x0CF00400;
            f.dlc = 8;
            const uint16_t raw = static_cast<uint16_t>(8 * (600 + t / 10'000));
            f.data = {0xFF, 0xFF, 0xFF, uint8_t(raw & 0xFF), uint8_t(raw >> 8), 0xFF, 0xFF, 0xFF};
            f.t_us = t;
            lg.feed(f);
        }
        Frame other;
        other.id = 0x18FEEE00;  // engine temperature: not kept
        other.dlc = 8;
        lg.feed(other);
        EXPECT_EQ(lg.stats().frames, 151u);
        EXPECT_EQ(lg.stats().journaled, 3u);
        EXPECT_EQ(lg.stats().throttled, 147u);
        EXPECT_EQ(lg.stats().ignored, 1u);
        EXPECT_DOUBLE_EQ(lg.state().engine_speed_rpm.value, 600 + 2'980'000 / 10'000);  // latest, not throttled
    }
    fs::remove_all(dir);
}

TEST(Sync, ReplayFromLastAckAfterDisconnect)
{
    char tmpl[] = "/tmp/j1939-sync-XXXXXX";
    fs::path dir = ::mkdtemp(tmpl);
    {
        journal::Journal j(dir);
        j.open();
        for (uint8_t i = 1; i <= 10; i++) {
            std::array<uint8_t, 1> p{i};
            j.append(p, i);
        }
        SyncSession s(&j);
        auto b1 = s.next_batch(4);
        ASSERT_EQ(b1.size(), 4u);
        s.ack(b1.back().seq);  // phone confirmed 1..4
        auto b2 = s.next_batch(4);
        ASSERT_EQ(b2.front().seq, 5u);
        // link drops before the ack of 5..8; on reconnect delivery restarts at 5
        s.restart();
        auto b3 = s.next_batch(100);
        ASSERT_EQ(b3.size(), 6u);
        EXPECT_EQ(b3.front().seq, 5u);
        EXPECT_EQ(b3.back().seq, 10u);
        s.ack(10);
    }
    {
        journal::Journal j(dir);  // reboot
        j.open();
        SyncSession s(&j);
        EXPECT_TRUE(s.next_batch(10).empty());  // everything was acknowledged
    }
    fs::remove_all(dir);
}
