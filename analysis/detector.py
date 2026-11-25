"""Robust z-score detector (as in the brief), on either feature set in features.py.

Baseline: median and MAD of each feature on fault-free data.
Score:    z = 0.6745 * (x - median) / MAD
Rule:     hysteresis  alert starts when |z| > Z_ON and clears when |z| < Z_OFF
          window      alert starts when P_ON of the last N samples have |z| > Z_ON,
                      clears when P_OFF of them have |z| < Z_OFF
          Applied per feature (the overall alert is on if any feature is), or to
          the pooled score sqrt(sum z^2).

The same Detector.update() runs offline (run.py) and live (live.py), so the
numbers in the report are the numbers you'll see in the demo.
"""
import json
import math
from collections import deque

import config
from features import FAULT_OF, Features, find_polarity

K = 0.6745  # makes MAD comparable to a standard deviation for normal data


def _med_mad(x):
    x = x.dropna()
    med = float(x.median())
    return med, float((x - med).abs().median())


def fit(df, mode="model", unlabelled=()):
    """Baseline from fault-free data, plus fault polarity from (faulty) unlabelled logs."""
    jump_sd = {n: _med_mad(df[n].diff())[1] / K for n in ("temp", "ph")}
    polarity = {}
    if mode == "model":
        for n in jump_sd:
            polarity[n], stays = find_polarity(unlabelled, n, config.JUMP_SD[n] * jump_sd[n])
            print(f"polarity {n}: {polarity[n]}   stays after up {[round(x) for x in stays[1]]}, "
                  f"after down {[round(x) for x in stays[-1]]}")
    fx = Features(mode, jump_sd)
    vals = [fx.update(r) for r in df.to_dict("records")]
    base = {}
    for n in vals[0]:
        s = df[n.split("_")[0] + "_in"] if n.endswith("_offset") else df.assign(v=[v[n] for v in vals])["v"].astype(float)
        med, mad = _med_mad(s)
        # an offset is 0 when nothing's wrong, so measure it against the sensor's own noise
        base[n] = (0.0 if n.endswith("_offset") else med, mad)
        if mad < 1e-9:
            print(f"warning: {n} has MAD = 0 (constant in training data), any change will alert")
            base[n] = (base[n][0], 1e-9)
    return {"mode": mode, "base": base, "jump_sd": jump_sd, "polarity": polarity}


def save(models, cols, path=config.BASELINE_FILE, **meta):
    out = {m["mode"]: {"baseline": {n: {"median": a, "mad": b} for n, (a, b) in m["base"].items()},
                       "jump_sd": m["jump_sd"], "polarity": m["polarity"]} for m in models}
    with open(path, "w") as f:
        json.dump({**out, "columns": cols, **meta}, f, indent=2)


def load(mode=config.MODE, path=config.BASELINE_FILE):
    with open(path) as f:
        d = json.load(f)[mode]
    return {"mode": mode, "jump_sd": d["jump_sd"], "polarity": d["polarity"],
            "base": {n: (v["median"], v["mad"]) for n, v in d["baseline"].items()}}


class Detector:
    def __init__(self, model, z_on=config.Z_ON, z_off=config.Z_OFF, rule=config.RULE,
                 pooled=config.POOLED, window=config.WINDOW_N, p_on=config.P_ON, p_off=config.P_OFF):
        self.base, self.z_on, self.z_off = model["base"], z_on, z_off
        self.rule, self.pooled, self.p_on, self.p_off = rule, pooled, p_on, p_off
        self.fx = Features(model["mode"], model["jump_sd"], model["polarity"])
        keys = ["pooled"] if pooled else list(self.base)
        self.state = {n: False for n in keys}
        self.hist = {n: deque(maxlen=window) for n in keys}

    def _step(self, n, s):
        if self.rule == "hysteresis":
            self.state[n] = s >= self.z_off if self.state[n] else s > self.z_on
            return
        h = self.hist[n]
        h.append(s)
        if self.state[n]:
            self.state[n] = sum(v < self.z_off for v in h) < self.p_off * h.maxlen
        else:
            self.state[n] = sum(v > self.z_on for v in h) >= self.p_on * h.maxlen

    def update(self, rec):
        """rec comes from load.parse_row. Returns (alert, {feature: z}). Missing readings keep the last state."""
        z = {}
        for n, x in self.fx.update(rec).items():
            med, mad = self.base[n]
            z[n] = float("nan") if x is None or math.isnan(x) else K * (x - med) / mad
        if self.pooled:
            vals = [v for v in z.values() if not math.isnan(v)]
            if vals:
                self._step("pooled", math.sqrt(sum(v * v for v in vals)))
        else:
            for n, v in z.items():
                if not math.isnan(v):
                    self._step(n, abs(v))
        return any(self.state.values()), z

    def alert_by(self):
        return ",".join(n for n, s in self.state.items() if s)

    def named(self):
        """Faults the active features point to (model features only, not pooled)."""
        return [FAULT_OF[n] for n, s in self.state.items() if s and n in FAULT_OF]

    def run(self, df):
        """Score a whole DataFrame sample by sample. Adds alert, z_<name>, alert_by, named and score."""
        out = df.copy()
        alerts, zs, by, named = [], [], [], []
        for rec in df.to_dict("records"):
            a, z = self.update(rec)
            alerts.append(a)
            zs.append(z)
            by.append(self.alert_by())
            named.append(",".join(self.named()))
        out["alert"] = alerts
        for n in self.base:
            out[f"z_{n}"] = [z[n] for z in zs]
        out["alert_by"], out["named"] = by, named
        az = out[[f"z_{n}" for n in self.base]].abs()
        out["score"] = (az ** 2).sum(axis=1) ** 0.5 if self.pooled else az.max(axis=1)
        return out
