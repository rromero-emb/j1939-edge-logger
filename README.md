# j1939-edge-logger

[![ci](https://github.com/rromero-emb/j1939-edge-logger/actions/workflows/ci.yml/badge.svg)](https://github.com/rromero-emb/j1939-edge-logger/actions/workflows/ci.yml)

A **J1939 logger for a vehicle CAN bus**, written as the core of an on-vehicle telematics or ELD agent on embedded Linux. It listens to a truck's bus, never disturbs it, keeps the signals an hours-of-service or fleet app needs (engine speed, wheel-based speed, total distance, engine hours and VIN), stores them in a **journal that survives a power cut at any byte**, and hands them to a phone or gateway with **replay and acknowledgements**.

C++20, no dependencies beyond the C++ standard library and Linux SocketCAN. It cross-compiles for 32- and 64-bit ARM in CI.

> **Status:** the code is verified by 34 unit tests and an end-to-end replay of a synthetic truck trace in CI (under AddressSanitizer and UBSan), plus a live SocketCAN test on `vcan`. It has **not been run on a real truck**. The BLE GATT transport and a signed package are not in this repository yet (see [Not yet](#not-yet)).

## What it does

| Area | Implementation |
|---|---|
| Bus access | SocketCAN raw socket, receive only, extended-id frames, kernel receive timestamps (`SO_TIMESTAMP`). The interface goes up in **listen-only** mode, so the logger cannot even ACK a frame |
| Bus speed | `choose_bitrate()`: listen-only probes at 250 and 500 kbit/s. It picks a rate only when exactly one has clean traffic (≥ 20 frames, < 1 % error frames), so a silent bus (ignition off) or a noisy one gives "undecided", never a guess |
| Identifiers | J1939-21 decoding: PDU1 (destination-specific, PS is not part of the PGN) vs PDU2 (broadcast) |
| Signals | SPN 190 engine speed (EEC1), SPN 84 wheel-based speed (CCVS), SPN 917 total distance (HRVD, 5 m) with fallback to SPN 245 (VD, 125 m), SPN 247 engine hours, SPN 237 VIN. "Not available" and error ranges are rejected. Each signal keeps its age, so the app can tell whether it is fresh (≤ 5 s) |
| Multi-packet | Passive **transport-protocol reassembly**: BAM and RTS/CTS sessions between other nodes, interleaved sessions from different sources, retransmitted and duplicate packets, aborts from either side, T1/T2 timeouts, invalid announcements |
| Volume control | Per-PGN throttle: EEC1 at 50 Hz on the bus becomes 1 Hz in the journal (60 s of traffic: 3809 frames in, 245 records kept). The in-memory state still sees every frame |
| Requests | `RequestGate`: the **only frame it can build** is a Request (PGN 59904), for whitelisted PGNs (engine hours, VIN). It is **off by default**, rate-limited per PGN and per hour, and refuses to send from the null address: the node must have claimed an address first (Address Claimed message and NAME arbitration included). It has no control messages |
| Journal | Append-only segments with a CRC on every record, synced in batches. After a power cut, recovery keeps every complete record and truncates the torn tail. The acknowledged position is a separate file, replaced atomically (tmp + fsync + rename + fsync of the directory). A size cap drops acknowledged segments first, and counts any unacknowledged loss |
| Delivery | `SyncSession`: batches after the last acknowledged record. After a disconnect or a reboot, delivery restarts from the durable cursor: the phone may see a record twice (it de-duplicates by sequence number) but never misses one |

### The power-cut test

`Journal.PowerCutAtEveryByte` writes 12 records, then for **every byte offset** of the segment file builds a copy cut at that offset and reopens it. Recovery must return exactly the records that were complete before the cut, and a new append must follow with the next sequence number. A second test flips a byte inside the last record (a partly written flash page) and checks it is dropped by the CRC.

## Build and run

```sh
cmake -S . -B build -G Ninja -DJ1939_SANITIZE=ON && cmake --build build && ctest --test-dir build

# Recorded trace (candump -L format). The sample is a synthetic truck pulling away, see tools/gen_trace.py
build/j1939log replay traces/truck_pull_away.log --journal /tmp/j
build/j1939log dump --journal /tmp/j --from 1

# Live, on a real interface (listen-only) or vcan
sudo ip link set can0 type can bitrate 250000 listen-only on && sudo ip link set up can0
build/j1939log live can0 --journal /var/lib/j1939
```

`replay` prints the latest state:

```json
{
  "engine_speed_rpm": {"value": 1500.000, "fresh": true},
  "wheel_speed_kmh": {"value": 88.000, "fresh": true},
  "total_distance_km": {"value": 1235.520, "fresh": true},
  "engine_hours": {"value": 10000.000, "fresh": true},
  "vin": "1XKAD49X0DJ123456",
  "stats": {"frames": 3809, "ignored": 60, "throttled": 3480, "journaled": 245, "tp_completed": 6, "tp_aborted": 0, "tp_timed_out": 1, "tp_rejected": 0}
}
```

The trace includes a VIN broadcast with a lost packet (the session times out and the next broadcast is reassembled) and an unrelated RTS/CTS transfer between two other nodes.

## Layout

```
include/j1939/   frame/identifier, signals, transport protocol, request gate, logger pipeline
include/journal/ power-loss-safe journal
src/             implementation
app/j1939log.cpp CLI: replay / live (SocketCAN) / dump
tests/           GoogleTest unit tests, end-to-end replay (Python), vcan live test
tools/           gen_trace.py: deterministic synthetic truck trace + expected values
traces/          sample trace
docs/DESIGN.md   design decisions and what a vendor SDK changes
```

## Not yet

- **BLE GATT peripheral** (BlueZ over D-Bus): the delivery logic (`SyncSession`) is transport-independent; the GATT service is a thin layer on top. The planned layout is in [docs/DESIGN.md](docs/DESIGN.md).
- **J1708/J1587** for older trucks, and a second input path through a serial or LAN adapter.
- **Signed package and staged rollout with rollback**: shown for a whole embedded Linux image in [cra-embedded-linux-demo](https://github.com/rromero-emb/cra-embedded-linux-demo) (signed FIT, RAUC A/B, anti-rollback).
- Validation on real vehicles.

## License

MIT.
