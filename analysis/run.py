"""Run the detector on a labelled log, score it, save a plot and log the result.

    python run.py data/three_faults_20261123_150000.csv
    python run.py data/three_faults_....csv --z-on 4 --z-off 3     # try other thresholds
    python run.py data/three_faults_....csv --sweep                # grid of thresholds

Each run is appended to results.csv (date, stream, thresholds, scores) for the
Results table and the report.
"""
import argparse
import csv
import datetime
import json
import os
from pathlib import Path

import config
import detector
from evaluate import naming, recall_by_fault, score, timing
from load import load_csv


def stream_name(path):
    name = Path(path).stem
    return next((s for s in sorted(config.STREAMS, key=len, reverse=True) if s in name), name)


def log_result(path, z_on, z_off, s, variant, note=""):
    new = not os.path.exists(config.RESULTS_FILE)
    with open(config.RESULTS_FILE, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["date", "stream", "z_on", "z_off", "precision", "recall", "f1", "tp", "fp", "fn", "tn", "file", "variant", "notes"])
        w.writerow([datetime.date.today(), stream_name(path), z_on, z_off, round(s.precision, 3),
                    round(s.recall, 3), round(s.f1, 3), s.tp, s.fp, s.fn, s.tn, path, variant, note])


def plot(df, base, z_on, z_off, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    raw, zs = ["temp", "ph", "rpm"], list(base)
    fig, axes = plt.subplots(len(raw) + len(zs), 1, figsize=(14, 2 * (len(raw) + len(zs))), sharex=True)
    for ax in axes:
        tr = ax.get_xaxis_transform()
        ax.fill_between(df["t"], 0.5, 1, where=df["label"].astype(bool), transform=tr, color="#f4a582", alpha=0.5,
                        step="mid", lw=0, label="fault active (truth)")
        ax.fill_between(df["t"], 0, 0.5, where=df["alert"], transform=tr, color="#92c5de",
                        alpha=0.6, step="mid", lw=0, label="alert")
    for ax, n in zip(axes, raw):
        ax.plot(df["t"], df[n], lw=0.7, color="#333")
        if df[f"{n}_sp"].notna().any():
            ax.plot(df["t"], df[f"{n}_sp"], lw=0.8, ls="--", color="#999", label="set point")
        ax.set_ylabel(n)
    for ax, n in zip(axes[len(raw):], zs):
        ax.plot(df["t"], df[f"z_{n}"].abs().clip(lower=0.05), lw=0.7, color="#333")
        ax.axhline(z_on, color="r", ls="--", lw=0.8, label=f"on {z_on}")
        ax.axhline(z_off, color="r", ls=":", lw=0.8, label=f"off {z_off}")
        ax.set_yscale("log")
        ax.set_ylabel(f"|z| {n}")
    axes[0].legend(loc="upper right", fontsize=7, ncol=3)
    axes[-1].legend(loc="upper right", fontsize=7, ncol=3)
    axes[-1].set_xlabel("time (s)")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--z-on", type=float, default=config.Z_ON)
    ap.add_argument("--z-off", type=float, default=config.Z_OFF)
    ap.add_argument("--sweep", action="store_true", help="try a grid of thresholds (not logged)")
    ap.add_argument("--mode", choices=["basic", "model"], default=config.MODE, help="feature set (see features.py)")
    ap.add_argument("--rule", choices=["hysteresis", "window"], default=config.RULE)
    ap.add_argument("--pooled", action="store_true", default=config.POOLED)
    ap.add_argument("--window", type=int, default=config.WINDOW_N, help="window length for --rule window")
    ap.add_argument("--from-t", type=float, default=0, help="only score samples from this many seconds into the log")
    ap.add_argument("--note", default="")
    args = ap.parse_args()

    model = detector.load(args.mode)
    base = model["base"]
    kw = {"rule": args.rule, "pooled": args.pooled, "window": args.window}
    df, cols = load_csv(args.log)
    if not df["label"].any():
        print("note: no active faults in this log, so recall can't be measured")

    if args.sweep:
        print(f"{'z_on':>5}{'z_off':>6}{'P':>7}{'R':>7}{'F1':>7}")
        for z_on in (2.5, 3.0, 3.5, 4.0, 5.0, 6.0):
            for gap in (0.5, 1.0, 1.5):
                s = score(detector.Detector(model, z_on, z_on - gap, **kw).run(df))
                print(f"{z_on:>5}{z_on - gap:>6}{s.precision:>7.3f}{s.recall:>7.3f}{s.f1:>7.3f}")
        return

    out = detector.Detector(model, args.z_on, args.z_off, **kw).run(df)
    out = out[out["t"] >= args.from_t]
    s = score(out)
    print(f"{stream_name(args.log)}: {len(out)} samples, {int(out['alert'].sum())} alerted, "
          f"{int(out['label'].sum())} with a fault active   (z_on {args.z_on}, z_off {args.z_off})")
    print(json.dumps(s.as_dict()))
    print("recall by fault:", json.dumps(recall_by_fault(out), indent=1))
    print("timing:", json.dumps(timing(out)))
    if args.mode == "model" and not args.pooled:
        print("right faults named:", json.dumps(naming(out)))

    stem = Path(args.log).with_suffix("")
    out.to_csv(f"{stem}_scored.csv", index=False)
    plot(out, base, args.z_on, args.z_off, f"{stem}_plot.png")
    log_result(args.log, args.z_on, args.z_off, s,
               f"{args.mode}/{args.rule}{'/pooled' if args.pooled else ''}", args.note)
    print(f"\n-> {stem}_scored.csv, {stem}_plot.png, row added to {config.RESULTS_FILE}")


if __name__ == "__main__":
    main()
