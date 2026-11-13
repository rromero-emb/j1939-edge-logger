#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>

#include "j1939/frame.hpp"

namespace j1939 {

// The only thing this logger may ever transmit: a Request (PGN 59904) for data that is not
// broadcast on its own, typically engine hours and VIN. Off by default, whitelisted, rate-limited,
// and only after the logger owns a source address (address claim, PGN 60928). No control messages.
struct RequestPolicy {
    bool enabled = false;
    std::set<uint32_t> whitelist{pgn::kHours, pgn::kVi};
    uint64_t min_interval_us = 60'000'000;  // per PGN
    uint32_t max_per_hour = 30;             // all PGNs together
};

class RequestGate {
public:
    explicit RequestGate(RequestPolicy p) : policy_(p) {}

    // A Request frame (DLC 3, PGN little-endian, priority 6) or nullopt if the policy forbids it.
    // `sa` must be an address this node has claimed; the null address may not send requests.
    std::optional<Frame> make(uint32_t pgn, uint8_t da, uint8_t sa, uint64_t now_us);

private:
    RequestPolicy policy_;
    std::map<uint32_t, uint64_t> last_us_;
    uint64_t window_start_us_ = 0;
    uint32_t in_window_ = 0;
    bool window_open_ = false;
};

// 64-bit NAME of J1939-81. During address arbitration the numerically lower NAME wins.
constexpr bool name_wins(uint64_t ours, uint64_t theirs)
{
    return ours < theirs;
}

// Address Claimed message: PGN 60928 to global, data = NAME little-endian.
Frame make_address_claim(uint64_t name, uint8_t sa, uint64_t now_us);

}  // namespace j1939
