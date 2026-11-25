"""Fake simulator logs for testing the pipeline off the UCL network.

    python fake_stream.py      # writes data/fake_{nofaults,three_faults,variable_setpoints}.csv

The column names are GUESSES at the simulator's shape (temperature_C.mean etc.);
the real ones come from log_stream.py's first message. Fault behaviour is also
a guess: therm_voltage_bias shifts the temperature reading, ph_offset_bias
shifts pH, heater_power_loss lets the temperature sag while heater duty maxes out.
"""
import json
from pathlib import Path

import numpy as np

FAULTS = ["therm_voltage_bias", "ph_offset_bias", "heater_power_loss"]


def schedule(n, rng, kinds, mean_gap=300, mean_len=60):
    active = [[] for _ in range(n)]
    t = int(rng.exponential(mean_gap))
    while t < n:
        length = max(10, int(rng.exponential(mean_len)))
        kind = rng.choice(kinds)
        for k in range(t, min(n, t + length)):
            active[k].append(str(kind))
        t += length + int(rng.exponential(mean_gap))
    return active


def generate(stream, minutes, seed):
    rng = np.random.default_rng(seed)
    n = minutes * 60
    kinds = {"nofaults": [], "single_fault": FAULTS[:1]}.get(stream, FAULTS)
    active = schedule(n, rng, kinds) if kinds else [[] for _ in range(n)]
    rows, sag = [], 0.0
    for k in range(n):
        if stream == "variable_setpoints":  # real one changes on the hour; faster here to test it
            sp_t, sp_ph, sp_rpm = [(30, 5.0, 1000), (27, 4.5, 800), (33, 6.0, 1300)][(k // 600) % 3]
        else:
            sp_t, sp_ph, sp_rpm = 30.0, 5.0, 1000.0
        f = active[k]
        sag = min(sag + 0.02, 1.5) if "heater_power_loss" in f else max(sag - 0.05, 0.0)
        temp = sp_t - sag + rng.normal(0, 0.08) + (0.6 if "therm_voltage_bias" in f else 0)
        ph = sp_ph + rng.normal(0, 0.02) + (0.25 if "ph_offset_bias" in f else 0)
        rpm = sp_rpm + rng.normal(0, 5)
        heater = 1.0 if "heater_power_loss" in f else float(np.clip(0.45 + rng.normal(0, 0.05), 0, 1))
        rows.append({
            "timestamp": 1795950000 + k,
            "temperature_C.mean": round(temp, 3), "temperature_C.min": round(temp - 0.1, 3),
            "temperature_C.max": round(temp + 0.1, 3),
            "pH.mean": round(ph, 3), "rpm.mean": round(rpm, 1),
            "actuators.heater_duty": round(heater, 3), "actuators.motor_duty": 0.5,
            "actuators.acid_duty": 0.0, "actuators.base_duty": 0.0,
            "setpoints.temperature_C": sp_t, "setpoints.pH": sp_ph, "setpoints.rpm": sp_rpm,
            "faults": json.dumps(f),
        })
    return rows


def main():
    import csv
    Path("data").mkdir(exist_ok=True)
    for i, (stream, minutes) in enumerate([("nofaults", 30), ("three_faults", 40), ("variable_setpoints", 40)]):
        rows = generate(stream, minutes, seed=i)
        path = f"data/fake_{stream}.csv"
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
        print(f"{path}: {len(rows)} rows, {sum(r['faults'] != '[]' for r in rows)} faulty")


if __name__ == "__main__":
    main()
