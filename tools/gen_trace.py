#!/usr/bin/env python3
"""Synthetic J1939 trace of a truck pulling away, in candump -L format (deterministic).

Engine (SA 0x00): EEC1 at 50 Hz, engine hours at 1 Hz, VIN by BAM every 10 s.
Cluster (SA 0x17): CCVS at 10 Hz. Tachograph (SA 0xEE): HRVD at 1 Hz.
Plus traffic the logger must ignore: engine temperature, and an RTS/CTS session between two ECUs.
At t = 20 s one BAM packet is lost, so that reassembly has to time out and recover.

Usage: gen_trace.py OUT.log [EXPECTED.json]
"""
import json
import sys

T0 = 1762000000_000000  # microseconds
DURATION_S = 60
VIN = "1XKAD49X0DJ123456"


def can_id(prio, pgn, da, sa):
    pf = (pgn >> 8) & 0xFF
    ps = da if pf < 240 else (pgn & 0xFF)
    return (prio << 26) | (((pgn >> 16) & 3) << 24) | (pf << 16) | (ps << 8) | sa


def line(t_us, cid, data):
    return "(%d.%06d) can0 %08X#%s" % (t_us // 1_000_000, t_us % 1_000_000, cid, bytes(data).hex().upper())


def le(v, n):
    return [(v >> (8 * i)) & 0xFF for i in range(n)]


def rpm_at(t):
    return 600 + min(t, 30.0) * 30  # 600 -> 1500 rpm


def speed_at(t):
    return min(t, 40.0) * 2.2  # 0 -> 88 km/h


def tp_frames(t_us, sa, da, pgn, payload, lose=None, rts=False):
    size = len(payload)
    packets = (size + 6) // 7
    ctrl = 0x10 if rts else 0x20
    cm = [ctrl] + le(size, 2) + [packets, 0xFF if not rts else packets] + le(pgn, 3)
    out = [(t_us, can_id(7, 60416, da, sa), cm)]
    t = t_us
    if rts:  # receiver sends CTS for all packets
        t += 5_000
        out.append((t, can_id(7, 60416, sa, da), [0x11, packets, 1, 0xFF, 0xFF] + le(pgn, 3)))
    padded = list(payload) + [0xFF] * (packets * 7 - size)
    for seq in range(1, packets + 1):
        t += 50_000
        if seq == lose:
            continue
        out.append((t, can_id(7, 60160, da, sa), [seq] + padded[(seq - 1) * 7: seq * 7]))
    if rts:
        t += 5_000
        out.append((t, can_id(7, 60416, sa, da), [0x13] + le(size, 2) + [packets, 0xFF] + le(pgn, 3)))
    return out


def main():
    frames = []
    distance_m = 1_234_567.0  # odometer at start: 1234.567 km
    hours_raw = 200_000  # 10,000 h (changes too slowly to move within a minute)
    last_dist = None
    for ms in range(0, DURATION_S * 1000, 20):
        t = ms / 1000.0
        t_us = T0 + ms * 1000
        rpm_raw = int(rpm_at(t) / 0.125)
        frames.append((t_us, can_id(3, 61444, 0xFF, 0x00), [0xF0, 0x7D, 0x7D] + le(rpm_raw, 2) + [0xFF, 0xFF, 0xFF]))
        if ms % 100 == 0:
            sp = int(speed_at(t) * 256)
            frames.append((t_us + 300, can_id(6, 65265, 0xFF, 0x17), [0xFF] + le(sp, 2) + [0xFF] * 5))
            distance_m += speed_at(t) / 3.6 * 0.1
        if ms % 1000 == 0:
            last_dist = int(distance_m / 5)
            frames.append((t_us + 500, can_id(6, 65217, 0xFF, 0xEE), le(last_dist, 4) + [0xFF] * 4))
            frames.append((t_us + 700, can_id(6, 65253, 0xFF, 0x00), le(hours_raw, 4) + [0xFF] * 4))
            frames.append((t_us + 900, can_id(6, 65262, 0xFF, 0x00), [0x7D, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]))
        if ms % 10_000 == 5_000:
            lose = 2 if ms == 25_000 else None
            frames += tp_frames(t_us + 1100, 0x00, 0xFF, 65260, (VIN + "*").encode(), lose=lose)
        if ms == 12_000:  # unrelated RTS/CTS transfer (DM2) between a tool (0xF9) and the engine
            frames += tp_frames(t_us + 1300, 0x00, 0xF9, 65227, list(range(20)), rts=True)
    frames.sort(key=lambda f: f[0])
    with open(sys.argv[1], "w", encoding="ascii") as f:
        for t_us, cid, data in frames:
            f.write(line(t_us, cid, data) + "\n")
    if len(sys.argv) > 2:
        last_t = DURATION_S - 0.02
        expected = {
            "engine_speed_rpm": int(rpm_at(last_t) / 0.125) * 0.125,
            "wheel_speed_kmh": int(speed_at(59.9) * 256) / 256,
            "total_distance_km": last_dist * 0.005,
            "engine_hours": hours_raw * 0.05,
            "vin": VIN,
            "tp_completed": 6,  # 5 of 6 VIN broadcasts + the DM2 RTS/CTS transfer
            "tp_timed_out": 1,
        }
        with open(sys.argv[2], "w", encoding="ascii") as f:
            json.dump(expected, f, indent=2)
            f.write("\n")


if __name__ == "__main__":
    main()
