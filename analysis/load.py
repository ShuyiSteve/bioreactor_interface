"""Shared parsing for logged CSVs and live MQTT messages.

A row (CSV line or flattened JSON message) becomes:
  {temp, temp_sp, temp_in, ph, ..., label, faults}
where `<name>_in` is what the detector scores (reading - set point, or the raw
reading if there's no set point), `label` is 1 if any fault is active and
`faults` names them. Labels are only ever used for scoring.

    python load.py data/nofaults_....csv    # shows which columns were picked
"""
import json
import math
import re
import sys

import pandas as pd

import config

TOKENS = {"temp": r"temp", "ph": r"(^|[._\s])ph([._\s]|$)", "rpm": r"rpm"}
SP_WORDS = r"set_?point|target|(^|[._])sp([._]|$)"
FALSY = {"", "none", "false", "0", "[]", "{}", "nan", "null", "ok", "normal"}


def flatten(d, prefix=""):
    """Same flattening as log_stream.py, so live rows match CSV columns."""
    out = {}
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.update(flatten(v, key + "."))
        elif isinstance(v, list):
            out[key] = json.dumps(v)
        else:
            out[key] = v
    return out


def resolve(keys, cfg=config):
    """Pick which columns hold each feature, its set point and the fault labels."""
    keys = list(keys)
    low = {k: k.lower() for k in keys}
    is_sp = lambda k: re.search(SP_WORDS, low[k]) is not None
    cols = {"features": {}, "setpoints": {}, "faults": []}

    for name, col in cfg.FEATURES.items():
        if col is None:
            pat = TOKENS.get(name)
            cands = [k for k in keys if pat and re.search(pat, low[k]) and not is_sp(k)]
            col = ([k for k in cands if "mean" in low[k]] or cands or [None])[0]
        if col not in keys:
            raise KeyError(f"no column for {name!r}; set config.FEATURES[{name!r}]. Columns: {keys}")
        cols["features"][name] = col

    if cfg.USE_SETPOINT_RESIDUAL:
        for name in cols["features"]:
            sp = cfg.SETPOINTS.get(name)
            if sp is False:
                continue
            if sp is None:
                pat = TOKENS.get(name)
                c = [k for k in keys if pat and re.search(pat, low[k]) and is_sp(k)]
                sp = c[0] if c else None
            if sp in keys:
                cols["setpoints"][name] = sp

    # unlabelled streams have no fault field at all
    cols["faults"] = [k for k in keys if (k == cfg.FAULTS if cfg.FAULTS else "fault" in low[k])]
    return cols


def _num(v):
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


def active_faults(row, fault_cols):
    """Names of active faults, whether they're logged as a JSON list, a dict or flags."""
    active = []
    for c in fault_cols:
        v = row.get(c)
        if isinstance(v, str) and v.strip()[:1] in "[{":
            try:
                v = json.loads(v)
            except json.JSONDecodeError:
                pass
        if isinstance(v, list):
            for x in v:
                if isinstance(x, dict):
                    x = x.get("name") or x.get("type") or x.get("fault") or json.dumps(x)
                active.append(str(x))
        elif isinstance(v, dict):
            active += [k for k, x in v.items() if x]
        elif isinstance(v, bool) or _num(v) is not None or str(v).lower() in ("true", "false"):
            if str(v).lower() in ("true", "1", "1.0"):
                active.append(c.split(".")[-1])
        elif v is not None and str(v).strip().lower() not in FALSY:
            active.append(str(v).strip())
    return active


def parse_row(row, cols):
    rec = {}
    for name, col in cols["features"].items():
        x = _num(row.get(col))
        sp = _num(row.get(cols["setpoints"][name])) if name in cols["setpoints"] else None
        rec[name], rec[f"{name}_sp"] = x, sp
        rec[f"{name}_in"] = (x - sp) if (x is not None and sp is not None) else x
    faults = active_faults(row, cols["faults"])
    for name, col in config.EXTRA.items():
        rec[name] = _num(row.get(col))
    rec["label"] = int(bool(faults))
    rec["faults"] = ",".join(faults)
    return rec


def load_csv(path, cfg=config):
    raw = pd.read_csv(path)
    cols = resolve(raw.columns, cfg)
    df = pd.DataFrame([parse_row(r, cols) for r in raw.to_dict("records")])
    df["time"] = df["time"].fillna(pd.Series(range(len(df)), dtype=float))
    df.insert(0, "t", df["time"] - df["time"].iloc[0])  # seconds since start
    return df, cols


def describe(cols):
    lines = [f"  {n:<6} <- {c}" + (f"   (minus {cols['setpoints'][n]})" if n in cols["setpoints"] else "")
             for n, c in cols["features"].items()]
    lines.append(f"  labels <- {cols['faults'] or 'none found (no scoring possible)'}")
    return "\n".join(lines)


if __name__ == "__main__":
    df, cols = load_csv(sys.argv[1])
    print("columns picked:\n" + describe(cols))
    print(df.head(10).to_string())
    print(f"\n{len(df)} rows, {df['label'].sum()} with an active fault")
