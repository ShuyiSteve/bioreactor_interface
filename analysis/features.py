"""Turns each record into the numbers the detector scores. Labels are never read here.

basic:  reading - set point for temp, pH and rpm (the method in the brief).
model:  one physically motivated feature per kind of fault.
  heater_power  energy used / energy the heater setting should use. Drops when
                heater power is lost, whatever the temperature is doing.
  temp_offset   sum of the jumps in the temperature reading that are too fast to
                be real. Stays away from 0 for as long as the sensor is biased,
                and ignores the slow overshoot after a fault.
  ph_offset     the same for pH, minus what the dosing could explain (it really can
                move pH by several units in a second).
"""
import math

import config

FAULT_OF = {"heater_power": "heater_power_loss", "temp_offset": "therm_voltage_bias",
            "ph_offset": "ph_offset_bias"}


def is_jump(d, dt, explained, jump):
    """explained: how far (down, up) the reading could really have moved."""
    return abs(d) - explained[d > 0] > jump and dt <= config.MAX_GAP_S


def dosing_reach(rec, prev):
    """How far (down, up) dosing in this sample and the last could have moved the pH."""
    total = lambda k: sum((r or {}).get(k) or 0.0 for r in (rec, prev))
    return config.DOSE_GAIN * total("acid_pwm"), config.DOSE_GAIN * total("base_pwm")


def find_polarity(dfs, name, jump):
    """Which way faults start, from unlabelled data: the reading is biased far less
    often than it's right, so the direction followed by the shorter stays is the start."""
    stays = {1: [], -1: []}
    for df in dfs:
        last, prev = None, None
        for r in df.to_dict("records"):
            if prev is not None:
                d = r[name] - prev[name]
                if is_jump(d, r["time"] - prev["time"], dosing_reach(r, prev) if name == "ph" else (0, 0), jump):
                    sign = 1 if d > 0 else -1
                    if last and last[0] != sign:
                        stays[last[0]].append(r["time"] - last[1])
                    if not last or last[0] != sign:
                        last = (sign, r["time"])
            prev = r
    mean = {s: sum(v) / len(v) for s, v in stays.items() if v}
    if len(mean) < 2:
        return None, stays
    return min(mean, key=mean.get), stays


class Offset:
    """Running sum of unexplained jumps, snapped back to 0 once a jump undoes it.

    polarity: which way a fault starts (+1/-1, None if unknown). A first jump the
    other way is a fault ending that was already on when we started watching.
    """

    def __init__(self, jump, polarity=None):
        self.jump, self.polarity = jump, polarity
        self.prev = self.prev_t = None
        self.b = self.peak = self.age = 0.0
        self.ending = 0  # direction of the jump that just ended a fault

    def update(self, x, t, explained=(0, 0)):
        if x is None or math.isnan(x):
            return self.b
        if self.prev is not None:
            dt, d = t - self.prev_t, x - self.prev
            if is_jump(d, dt, explained, self.jump):
                if self.b and (self.b + d) * self.b < 0:
                    # undone (part of the first jump can be hidden by dosing, so the
                    # end can be the bigger one)
                    self.b = self.peak = 0.0
                    self.ending = math.copysign(1, d)
                elif d * self.ending <= 0 and (self.b or self.polarity is None or d * self.polarity > 0):
                    self.b += d
                    self.peak = max(self.peak, abs(self.b))
            else:
                self.ending = 0
                # jumps can span two samples, so only settle once it's quiet again
                if abs(self.b) < 0.5 * self.peak:
                    self.b = self.peak = 0.0
            self.age = self.age + dt if self.b else 0.0
            if self.age > config.MAX_FAULT_S:
                # probably started logging mid-fault, so took its end for a start
                self.b = self.peak = self.age = 0.0
        self.prev, self.prev_t = x, t
        return self.b


class Features:
    def __init__(self, mode, jump_sd=None, polarity=None):
        self.mode, self.off, self.prev = mode, {}, None
        if mode == "model":
            for n in ("temp", "ph"):
                self.off[n] = Offset(config.JUMP_SD[n] * jump_sd[n], (polarity or {}).get(n))

    def update(self, rec):
        """{feature: value}; None if it can't be computed for this sample."""
        if self.mode == "basic":
            return {n: rec.get(f"{n}_in") for n in ("temp", "ph", "rpm")}
        reach = dosing_reach(rec, self.prev)
        self.prev = rec
        pwm, wh, n = rec.get("heater_pwm"), rec.get("heater_wh"), rec.get("samples")
        ok = pwm is not None and wh is not None and n and pwm > 0.05
        return {"temp_offset": self.off["temp"].update(rec["temp"], rec["time"]),
                "ph_offset": self.off["ph"].update(rec["ph"], rec["time"], reach),
                "heater_power": wh / (pwm * n) if ok else None}
