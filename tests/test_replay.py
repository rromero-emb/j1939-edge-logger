"""End to end: generated truck trace -> j1939log replay -> state, journal, stats."""
import json
import os
import subprocess
import sys
import tempfile

exe, root = sys.argv[1], sys.argv[2]
with tempfile.TemporaryDirectory() as tmp:
    trace = os.path.join(tmp, "truck.log")
    exp_path = os.path.join(tmp, "expected.json")
    subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_trace.py"), trace, exp_path], check=True)
    # the committed sample must be what the generator produces
    with open(trace) as a, open(os.path.join(root, "traces", "truck_pull_away.log")) as b:
        assert a.read() == b.read(), "traces/truck_pull_away.log is stale: regenerate it with tools/gen_trace.py"
    expected = json.load(open(exp_path))
    jdir = os.path.join(tmp, "journal")
    out = subprocess.run([exe, "replay", trace, "--journal", jdir], check=True, capture_output=True, text=True).stdout
    state = json.loads(out)
    for k in ("engine_speed_rpm", "wheel_speed_kmh", "total_distance_km", "engine_hours"):
        assert state[k] is not None and abs(state[k]["value"] - expected[k]) < 1e-6, (k, state[k], expected[k])
        assert state[k]["fresh"], k
    assert state["vin"] == expected["vin"], state["vin"]
    st = state["stats"]
    assert st["tp_completed"] == expected["tp_completed"], st
    assert st["tp_timed_out"] == expected["tp_timed_out"], st
    # 60 s at 1 Hz for EEC1/CCVS + HRVD + hours, + 5 VINs; throttling keeps the journal small
    assert st["journaled"] == 60 * 4 + 5, st
    assert st["throttled"] > 3000, st
    dump = subprocess.run([exe, "dump", "--journal", jdir], check=True, capture_output=True, text=True).stdout
    assert len(dump.splitlines()) == st["journaled"]
    # replaying again appends after the existing records (sequence continues across restarts)
    subprocess.run([exe, "replay", trace, "--journal", jdir], check=True, capture_output=True)
    dump = subprocess.run([exe, "dump", "--journal", jdir], check=True, capture_output=True, text=True).stdout
    lines = dump.splitlines()
    assert len(lines) == 2 * st["journaled"] and lines[-1].split()[0] == str(2 * st["journaled"])
print("replay ok:", json.dumps(state["stats"]))
