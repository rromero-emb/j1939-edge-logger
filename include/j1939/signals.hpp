#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "j1939/frame.hpp"

namespace j1939 {

// SPN decoders (J1939-71). Each returns nullopt when the field says "not available" or "error"
// (raw values above the valid range: > 0xFAFF for 2 bytes, > 0xFAFFFFFF for 4 bytes).
std::optional<double> engine_speed_rpm(std::span<const uint8_t> eec1);     // SPN 190, 0.125 rpm/bit
std::optional<double> wheel_speed_kmh(std::span<const uint8_t> ccvs);      // SPN 84, 1/256 km/h per bit
std::optional<double> hr_total_distance_km(std::span<const uint8_t> hrvd); // SPN 917, 5 m/bit
std::optional<double> total_distance_km(std::span<const uint8_t> vd);      // SPN 245, 0.125 km/bit
std::optional<double> engine_hours(std::span<const uint8_t> hours);        // SPN 247, 0.05 h/bit
// SPN 237: ASCII, up to 200 characters, terminated by '*' (the delimiter is optional at the end).
std::optional<std::string> vin(std::span<const uint8_t> vi);

// Latest value of each signal with its age. "Fresh" = updated within max_age_us.
struct Signal {
    double value = 0;
    uint64_t t_us = 0;
    bool valid = false;
};

struct VehicleState {
    Signal engine_speed_rpm;
    Signal wheel_speed_kmh;
    Signal total_distance_km;  // from HRVD when available, else VD
    Signal engine_hours;
    std::string vin;
    uint64_t vin_t_us = 0;
    bool distance_from_hrvd = false;

    // Feeds one complete message (single frame or reassembled multi-packet). Returns true if
    // it updated a signal.
    bool update(uint32_t pgn, std::span<const uint8_t> data, uint64_t t_us);
    static bool fresh(const Signal &s, uint64_t now_us, uint64_t max_age_us);
};

}  // namespace j1939
