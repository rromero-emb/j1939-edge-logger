#include <gtest/gtest.h>

#include "j1939/frame.hpp"
#include "j1939/request.hpp"
#include "j1939/signals.hpp"

using namespace j1939;

TEST(Id, Pdu2BroadcastKeepsGroupExtensionInPgn)
{
    // CCVS from the engine (SA 0x00), priority 6: 0x18FEF100
    Id id = decode_id(0x18FEF100);
    EXPECT_EQ(id.priority, 6);
    EXPECT_EQ(id.pgn, pgn::kCcvs);
    EXPECT_EQ(id.sa, 0x00);
    EXPECT_EQ(id.da, kGlobal);
}

TEST(Id, Pdu1DestinationIsNotPartOfPgn)
{
    // Request from 0xF9 to 0x00: 0x18EA00F9 -> PGN 59904, DA 0
    Id id = decode_id(0x18EA00F9);
    EXPECT_EQ(id.pgn, pgn::kRequest);
    EXPECT_EQ(id.da, 0x00);
    EXPECT_EQ(id.sa, 0xF9);
}

TEST(Id, EncodeRoundTrip)
{
    EXPECT_EQ(encode_id(6, pgn::kCcvs, kGlobal, 0x00), 0x18FEF100u);
    EXPECT_EQ(encode_id(6, pgn::kRequest, 0x00, 0xF9), 0x18EA00F9u);
    EXPECT_EQ(encode_id(3, pgn::kEec1, kGlobal, 0x00), 0x0CF00400u);
    for (uint32_t raw : {0x0CF00400u, 0x18FEE500u, 0x1CEBFF00u, 0x1CECFF00u, 0x18EEFF80u}) {
        Id i = decode_id(raw);
        EXPECT_EQ(encode_id(i.priority, i.pgn, i.da, i.sa), raw);
    }
}

TEST(Signals, EngineSpeedEec1)
{
    // bytes 4-5 = 0x3E80 = 16000 * 0.125 = 2000 rpm
    std::array<uint8_t, 8> d{0xFF, 0xFF, 0xFF, 0x80, 0x3E, 0xFF, 0xFF, 0xFF};
    EXPECT_DOUBLE_EQ(*engine_speed_rpm(d), 2000.0);
    d[3] = 0xFF;
    d[4] = 0xFF;  // not available
    EXPECT_FALSE(engine_speed_rpm(d).has_value());
    d[3] = 0x00;
    d[4] = 0xFE;  // 0xFE00: error indicator
    EXPECT_FALSE(engine_speed_rpm(d).has_value());
}

TEST(Signals, WheelSpeedCcvs)
{
    std::array<uint8_t, 8> d{0xFF, 0x00, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 0x5A00 / 256 = 90 km/h
    EXPECT_DOUBLE_EQ(*wheel_speed_kmh(d), 90.0);
}

TEST(Signals, DistanceAndHours)
{
    std::array<uint8_t, 8> hrvd{0x40, 0x42, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};  // 1,000,000 * 5 m = 5000 km
    EXPECT_DOUBLE_EQ(*hr_total_distance_km(hrvd), 5000.0);
    std::array<uint8_t, 8> vd{0xFF, 0xFF, 0xFF, 0xFF, 0x40, 0x9C, 0x00, 0x00};  // 40000 * 0.125 = 5000 km
    EXPECT_DOUBLE_EQ(*total_distance_km(vd), 5000.0);
    std::array<uint8_t, 8> hours{0x10, 0x27, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};  // 10000 * 0.05 = 500 h
    EXPECT_DOUBLE_EQ(*engine_hours(hours), 500.0);
}

TEST(Signals, Vin)
{
    const std::string s = "1XKAD49X0DJ123456*";
    std::vector<uint8_t> d(s.begin(), s.end());
    EXPECT_EQ(*vin(d), "1XKAD49X0DJ123456");
    d.push_back(0xFF);  // padding after the delimiter is ignored
    EXPECT_EQ(*vin(d), "1XKAD49X0DJ123456");
    std::vector<uint8_t> bad{'1', 0x01, '*'};
    EXPECT_FALSE(vin(bad).has_value());
}

TEST(Signals, StatePrefersHighResolutionDistance)
{
    VehicleState st;
    std::array<uint8_t, 8> vd{0xFF, 0xFF, 0xFF, 0xFF, 0x08, 0x00, 0x00, 0x00};  // 1 km
    EXPECT_TRUE(st.update(pgn::kVd, vd, 1));
    EXPECT_DOUBLE_EQ(st.total_distance_km.value, 1.0);
    std::array<uint8_t, 8> hrvd{0xC8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};  // 200 * 5 m = 1.0 km
    EXPECT_TRUE(st.update(pgn::kHrvd, hrvd, 2));
    EXPECT_FALSE(st.update(pgn::kVd, vd, 3));  // VD ignored once HRVD is seen
    EXPECT_EQ(st.total_distance_km.t_us, 2u);
}

TEST(Signals, Freshness)
{
    Signal s{1.0, 1'000'000, true};
    EXPECT_TRUE(VehicleState::fresh(s, 6'000'000, 5'000'000));
    EXPECT_FALSE(VehicleState::fresh(s, 6'000'001, 5'000'000));
    EXPECT_FALSE(VehicleState::fresh(Signal{}, 0, 5'000'000));
}

TEST(Request, OffByDefault)
{
    RequestGate g(RequestPolicy{});
    EXPECT_FALSE(g.make(pgn::kHours, 0x00, 0xF9, 0).has_value());
}

TEST(Request, WhitelistRateLimitAndNullAddress)
{
    RequestPolicy p;
    p.enabled = true;
    RequestGate g(p);
    EXPECT_FALSE(g.make(pgn::kEec1, 0x00, 0xF9, 0).has_value());           // not whitelisted
    EXPECT_FALSE(g.make(pgn::kHours, 0x00, kNullAddress, 0).has_value());  // no claimed address
    auto f = g.make(pgn::kHours, 0x00, 0xF9, 1'000'000);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->id, 0x18EA00F9u);
    EXPECT_EQ(f->dlc, 3);
    EXPECT_EQ(f->data[0], 0xE5);  // 65253 = 0x00FEE5, little-endian
    EXPECT_EQ(f->data[1], 0xFE);
    EXPECT_EQ(f->data[2], 0x00);
    EXPECT_FALSE(g.make(pgn::kHours, 0x00, 0xF9, 30'000'000).has_value());  // within 60 s
    EXPECT_TRUE(g.make(pgn::kVi, 0x00, 0xF9, 30'000'000).has_value());      // other PGN is fine
    EXPECT_TRUE(g.make(pgn::kHours, 0x00, 0xF9, 61'000'000).has_value());
}

TEST(Request, HourlyBudget)
{
    RequestPolicy p;
    p.enabled = true;
    p.min_interval_us = 0;
    p.max_per_hour = 3;
    RequestGate g(p);
    int sent = 0;
    for (uint64_t t = 0; t < 10; t++) {
        sent += g.make(pgn::kHours, 0x00, 0xF9, t * 1'000'000).has_value() ? 1 : 0;
    }
    EXPECT_EQ(sent, 3);
    EXPECT_TRUE(g.make(pgn::kHours, 0x00, 0xF9, 3'601'000'000ULL).has_value());
}

TEST(AddressClaim, FrameAndArbitration)
{
    Frame f = make_address_claim(0x8000000000000123ULL, 0xF9, 0);
    EXPECT_EQ(f.id, 0x18EEFFF9u);
    EXPECT_EQ(f.data[0], 0x23);
    EXPECT_EQ(f.data[7], 0x80);
    EXPECT_TRUE(name_wins(0x10, 0x20));
    EXPECT_FALSE(name_wins(0x20, 0x10));
}
