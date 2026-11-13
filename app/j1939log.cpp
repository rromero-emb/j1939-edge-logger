// j1939log: J1939 logger for a vehicle CAN bus.
//   j1939log replay <candump.log> --journal DIR   process a recorded trace (candump -L format)
//   j1939log live <ifname> --journal DIR          read a SocketCAN interface (set listen-only first:
//                                                 ip link set can0 type can bitrate 250000 listen-only on)
//   j1939log dump --journal DIR [--from SEQ]       print journal records
// Prints the latest vehicle state as JSON at the end (replay) or on SIGINT/SIGTERM (live).
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "j1939/logger.hpp"

using namespace j1939;

namespace {

volatile std::sig_atomic_t stop_flag = 0;
void on_signal(int)
{
    stop_flag = 1;
}

const std::set<uint32_t> kPgns{pgn::kEec1, pgn::kCcvs, pgn::kHrvd, pgn::kVd, pgn::kHours, pgn::kVi};

void configure(Logger &lg)
{
    lg.throttle().set(pgn::kEec1, 1'000'000);  // 50 Hz on the bus, 1 Hz in the journal
    lg.throttle().set(pgn::kCcvs, 1'000'000);
}

void print_state(const Logger &lg, uint64_t now_us)
{
    const VehicleState &s = lg.state();
    auto sig = [now_us](const char *name, const Signal &x, bool last) {
        std::printf("  \"%s\": ", name);
        if (x.valid) {
            std::printf("{\"value\": %.3f, \"fresh\": %s}", x.value, VehicleState::fresh(x, now_us, 5'000'000) ? "true" : "false");
        } else {
            std::printf("null");
        }
        std::printf("%s\n", last ? "" : ",");
    };
    std::printf("{\n");
    sig("engine_speed_rpm", s.engine_speed_rpm, false);
    sig("wheel_speed_kmh", s.wheel_speed_kmh, false);
    sig("total_distance_km", s.total_distance_km, false);
    sig("engine_hours", s.engine_hours, false);
    std::printf("  \"vin\": %s%s%s,\n", s.vin.empty() ? "" : "\"", s.vin.empty() ? "null" : s.vin.c_str(),
                s.vin.empty() ? "" : "\"");
    const auto &st = lg.stats();
    const auto &tp = lg.tp_stats();
    std::printf("  \"stats\": {\"frames\": %llu, \"ignored\": %llu, \"throttled\": %llu, \"journaled\": %llu, "
                "\"tp_completed\": %u, \"tp_aborted\": %u, \"tp_timed_out\": %u, \"tp_rejected\": %u}\n",
                (unsigned long long)st.frames, (unsigned long long)st.ignored, (unsigned long long)st.throttled,
                (unsigned long long)st.journaled, tp.completed, tp.aborted, tp.timed_out, tp.rejected);
    std::printf("}\n");
}

int replay(const std::string &file, journal::Journal *j)
{
    std::ifstream in(file);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", file.c_str());
        return 1;
    }
    Logger lg(j, kPgns);
    configure(lg);
    std::string line;
    uint64_t last_t = 0;
    while (std::getline(in, line)) {
        if (auto f = parse_candump(line)) {
            lg.feed(*f);
            last_t = f->t_us;
        }
    }
    if (j != nullptr) {
        j->sync();
    }
    print_state(lg, last_t);
    return 0;
}

int live(const std::string &ifname, journal::Journal *j)
{
    int s = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        std::perror("socket");
        return 1;
    }
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (::ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        std::perror("SIOCGIFINDEX");
        return 1;
    }
    // Receive only: this socket never writes. Extended-id frames only (J1939).
    struct can_filter flt{CAN_EFF_FLAG, CAN_EFF_FLAG | CAN_RTR_FLAG};
    ::setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof flt);
    int ts = 1;
    ::setsockopt(s, SOL_SOCKET, SO_TIMESTAMP, &ts, sizeof ts);
    struct sockaddr_can addr {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
        std::perror("bind");
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    Logger lg(j, kPgns);
    configure(lg);
    uint64_t last_t = 0;
    while (!stop_flag) {
        struct can_frame cf {};
        char ctrl[CMSG_SPACE(sizeof(struct timeval))];
        struct iovec iov{&cf, sizeof cf};
        struct msghdr msg {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof ctrl;
        ssize_t n = ::recvmsg(s, &msg, 0);
        if (n < static_cast<ssize_t>(sizeof cf)) {
            continue;  // EINTR on stop, or a short read
        }
        Frame f;
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMP) {
                struct timeval tv;
                std::memcpy(&tv, CMSG_DATA(c), sizeof tv);
                f.t_us = uint64_t(tv.tv_sec) * 1'000'000 + uint64_t(tv.tv_usec);  // kernel receive time
            }
        }
        f.id = cf.can_id & CAN_EFF_MASK;
        f.dlc = cf.can_dlc > 8 ? 8 : cf.can_dlc;
        std::memcpy(f.data.data(), cf.data, f.dlc);
        lg.feed(f);
        last_t = f.t_us;
    }
    if (j != nullptr) {
        j->sync();
    }
    ::close(s);
    print_state(lg, last_t);
    return 0;
}

int dump(journal::Journal &j, uint64_t from)
{
    j.read_from(from, [](const journal::Record &r) {
        std::printf("%llu %llu.%06llu", (unsigned long long)r.seq, (unsigned long long)(r.t_us / 1'000'000),
                    (unsigned long long)(r.t_us % 1'000'000));
        for (uint8_t b : r.payload) {
            std::printf(" %02X", b);
        }
        std::printf("\n");
        return true;
    });
    return 0;
}

int usage()
{
    std::fprintf(stderr, "usage: j1939log replay <candump.log> [--journal DIR]\n"
                         "       j1939log live <ifname> [--journal DIR]\n"
                         "       j1939log dump --journal DIR [--from SEQ]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    const std::string cmd = argv[1];
    std::string arg;
    std::string dir;
    uint64_t from = 1;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--journal" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--from" && i + 1 < argc) {
            from = std::stoull(argv[++i]);
        } else if (arg.empty()) {
            arg = a;
        } else {
            return usage();
        }
    }
    try {
        std::unique_ptr<journal::Journal> j;
        if (!dir.empty()) {
            j = std::make_unique<journal::Journal>(dir);
            auto rep = j->open();
            std::fprintf(stderr, "journal %s: %zu segments, %llu records, %llu torn bytes removed\n", dir.c_str(),
                         rep.segments, (unsigned long long)rep.records, (unsigned long long)rep.truncated_bytes);
        }
        if (cmd == "replay" && !arg.empty()) {
            return replay(arg, j.get());
        }
        if (cmd == "live" && !arg.empty()) {
            return live(arg, j.get());
        }
        if (cmd == "dump" && j) {
            return dump(*j, from);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
