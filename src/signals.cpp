#include "j1939/signals.hpp"

namespace j1939 {
namespace {

std::optional<uint32_t> le16(std::span<const uint8_t> d, size_t at)
{
    if (d.size() < at + 2) {
        return std::nullopt;
    }
    const uint32_t v = d[at] | (static_cast<uint32_t>(d[at + 1]) << 8);
    return (v <= 0xFAFFu) ? std::optional<uint32_t>(v) : std::nullopt;
}

std::optional<uint32_t> le32(std::span<const uint8_t> d, size_t at)
{
    if (d.size() < at + 4) {
        return std::nullopt;
    }
    const uint32_t v = d[at] | (static_cast<uint32_t>(d[at + 1]) << 8) | (static_cast<uint32_t>(d[at + 2]) << 16) |
                       (static_cast<uint32_t>(d[at + 3]) << 24);
    return (v <= 0xFAFFFFFFu) ? std::optional<uint32_t>(v) : std::nullopt;
}

}  // namespace

// Byte positions below are 0-based; J1939-71 counts from 1 (SPN 190 = bytes 4-5).
std::optional<double> engine_speed_rpm(std::span<const uint8_t> eec1)
{
    auto v = le16(eec1, 3);
    return v ? std::optional<double>(*v * 0.125) : std::nullopt;
}

std::optional<double> wheel_speed_kmh(std::span<const uint8_t> ccvs)
{
    auto v = le16(ccvs, 1);
    return v ? std::optional<double>(*v / 256.0) : std::nullopt;
}

std::optional<double> hr_total_distance_km(std::span<const uint8_t> hrvd)
{
    auto v = le32(hrvd, 0);
    return v ? std::optional<double>(*v * 0.005) : std::nullopt;
}

std::optional<double> total_distance_km(std::span<const uint8_t> vd)
{
    auto v = le32(vd, 4);
    return v ? std::optional<double>(*v * 0.125) : std::nullopt;
}

std::optional<double> engine_hours(std::span<const uint8_t> hours)
{
    auto v = le32(hours, 0);
    return v ? std::optional<double>(*v * 0.05) : std::nullopt;
}

std::optional<std::string> vin(std::span<const uint8_t> vi)
{
    std::string s;
    for (uint8_t c : vi) {
        if (c == '*') {
            break;
        }
        if (c < 0x20 || c > 0x7E) {
            return std::nullopt;  // VIN is printable ASCII
        }
        s.push_back(static_cast<char>(c));
    }
    if (s.empty()) {
        return std::nullopt;
    }
    return s;
}

bool VehicleState::update(uint32_t p, std::span<const uint8_t> d, uint64_t t)
{
    auto set = [t](Signal &s, std::optional<double> v) {
        if (!v) {
            return false;
        }
        s.value = *v;
        s.t_us = t;
        s.valid = true;
        return true;
    };
    switch (p) {
    case pgn::kEec1:
        return set(engine_speed_rpm, j1939::engine_speed_rpm(d));
    case pgn::kCcvs:
        return set(wheel_speed_kmh, j1939::wheel_speed_kmh(d));
    case pgn::kHrvd:
        if (set(total_distance_km, hr_total_distance_km(d))) {
            distance_from_hrvd = true;
            return true;
        }
        return false;
    case pgn::kVd: {
        // Lower resolution: only used while the vehicle sends no HRVD.
        if (distance_from_hrvd) {
            return false;
        }
        return set(total_distance_km, j1939::total_distance_km(d));
    }
    case pgn::kHours:
        return set(engine_hours, j1939::engine_hours(d));
    case pgn::kVi: {
        auto v = j1939::vin(d);
        if (!v) {
            return false;
        }
        vin = *v;
        vin_t_us = t;
        return true;
    }
    default:
        return false;
    }
}

bool VehicleState::fresh(const Signal &s, uint64_t now_us, uint64_t max_age_us)
{
    return s.valid && now_us >= s.t_us && (now_us - s.t_us) <= max_age_us;
}

}  // namespace j1939
