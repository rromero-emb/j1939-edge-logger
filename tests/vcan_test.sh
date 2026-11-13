#!/bin/sh
# Live path on a virtual CAN interface: canplayer sends the sample trace, j1939log live reads it
# through a SocketCAN raw socket. Needs root (modprobe, ip link) and can-utils.
set -eu
exe=$1
trace=$2
work=$(mktemp -d)
trap 'kill $pid 2>/dev/null || true; rm -rf "$work"' EXIT
modprobe vcan
ip link add dev vcan0 type vcan 2>/dev/null || true
ip link set up vcan0
"$exe" live vcan0 --journal "$work/j" > "$work/state.json" &
pid=$!
sleep 1
canplayer -I "$trace" vcan0=can0 -t   # -t: ignore timestamps, send as fast as possible
sleep 1
kill -INT $pid
timeout 10 sh -c "while kill -0 $pid 2>/dev/null; do sleep 0.2; done"
wait $pid
cat "$work/state.json"
grep -q '"vin": "1XKAD49X0DJ123456"' "$work/state.json"
grep -q '"engine_speed_rpm": {"value": 1500.000' "$work/state.json"
test "$("$exe" dump --journal "$work/j" | wc -l)" -gt 0
echo "vcan live ok"
