#include "j1939/request.hpp"

namespace j1939 {

std::optional<Frame> RequestGate::make(uint32_t p, uint8_t da, uint8_t sa, uint64_t now_us)
{
    if (!policy_.enabled || policy_.whitelist.count(p) == 0 || sa >= kNullAddress) {
        return std::nullopt;
    }
    auto it = last_us_.find(p);
    if (it != last_us_.end() && now_us - it->second < policy_.min_interval_us) {
        return std::nullopt;
    }
    if (!window_open_ || now_us - window_start_us_ >= 3'600'000'000ULL) {
        window_open_ = true;
        window_start_us_ = now_us;
        in_window_ = 0;
    }
    if (in_window_ >= policy_.max_per_hour) {
        return std::nullopt;
    }
    in_window_++;
    last_us_[p] = now_us;

    Frame f;
    f.t_us = now_us;
    f.id = encode_id(6, pgn::kRequest, da, sa);
    f.dlc = 3;
    f.data = {static_cast<uint8_t>(p & 0xFF), static_cast<uint8_t>((p >> 8) & 0xFF),
              static_cast<uint8_t>((p >> 16) & 0xFF), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return f;
}

Frame make_address_claim(uint64_t name, uint8_t sa, uint64_t now_us)
{
    Frame f;
    f.t_us = now_us;
    f.id = encode_id(6, pgn::kAddressClaim, kGlobal, sa);
    f.dlc = 8;
    for (size_t i = 0; i < 8; i++) {
        f.data[i] = static_cast<uint8_t>(name >> (8 * i));
    }
    return f;
}

}  // namespace j1939
