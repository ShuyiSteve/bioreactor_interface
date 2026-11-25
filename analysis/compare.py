"""Compare detector variants fairly: tune thresholds on the first part of a log,
then score once on the rest (which the tuning never saw).

    python compare.py data/three_faults_....csv             # tune on the first 50%
    python compare.py data/three_faults_....csv --split 0.4

Writes <log>_compare.csv and <log>_pr.png (precision-recall curves on the test part).
"""
import argparse
from pathlib import Path

import pandas as pd

import detector
from evaluate import pr_curve, score, timing
from load import load_csv

VARIANTS = [("basic", "hysteresis", False), ("basic", "window", False), ("basic", "hysteresis", True),
            ("model", "hysteresis", False), ("model", "window", False), ("model", "hysteresis", True)]
Z_GRID = (2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 8.0, 12.0, 20.0, 50.0, 100.0)
WINDOWS = (3, 5, 10)


def settings(rule):
    for z in Z_GRID:
        for frac in (0.5, 0.7, 0.9):
            for n in (WINDOWS if rule == "window" else (10,)):
                yield {"z_on": z, "z_off": round(z * frac, 2), "window": n}


def run(df, split_t, model, **kw):
    """Runs over the whole log so the detector is warmed up, returns (tune, test) parts."""
    out = detector.Detector(model, **kw).run(df)
    return out[out["t"] < split_t], out[out["t"] >= split_t]


def plot_pr(curves, base_rate, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7, 5), facecolor="#fcfcfb")
    ax.set_facecolor("#fcfcfb")
    for (name, r, p, ap), c in zip(curves, ("#2a78d6", "#eb6834", "#1baf7a")):
        ax.plot([0] + r, [p[0]] + p, color=c, lw=2, label=f"{name} (AP = {ap:.3f})")
    ax.axhline(base_rate, color="#52514e", ls="--", lw=1, label=f"always alert ({base_rate:.2f})")
    ax.set(xlim=(0, 1.01), ylim=(0, 1.03), xlabel="Recall", ylabel="Precision")
    ax.set_title("Precision-recall on the second half of the log", loc="left", color="#0b0b0b")
    ax.grid(color="#e6e5e1", lw=0.6)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.legend(loc="lower left", frameon=False, fontsize=9)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--split", type=float, default=0.5, help="share of the log used for tuning")
    args = ap.parse_args()

    df, _ = load_csv(args.log)
    split_t = df["t"].iloc[-1] * args.split
    models = {m: detector.load(m) for m in ("basic", "model")}
    rows, curves = [], []
    for mode, rule, pooled in VARIANTS:
        kw = {"rule": rule, "pooled": pooled}
        default = run(df, split_t, models[mode], **kw, z_on=3.5, z_off=2.5)[1]
        # ties go to the setting nearest the brief's starting threshold
        best = max(settings(rule), key=lambda s: (round(score(run(df, split_t, models[mode], **kw, **s)[0]).f1, 4),
                                                  -abs(s["z_on"] - 3.5)))
        test = run(df, split_t, models[mode], **kw, **best)[1]
        s, tm = score(test), timing(test)
        if rule == "hysteresis" and (mode, pooled) != ("model", True):
            r, p, a = pr_curve(test)
            curves.append((f"{mode}{', pooled' if pooled else ''}", r, p, a))
        rows.append({"features": mode, "rule": rule, "pooled": pooled, "f1_at_3.5/2.5": round(score(default).f1, 3),
                     **best, **s.as_dict(), **tm})
    table = pd.DataFrame(rows)
    print(f"{args.log}: tuned on t < {split_t:.0f}s, scored on the rest "
          f"({len(test)} samples, {int(test['label'].sum())} faulty)\n")
    print(table.to_string(index=False))
    stem = Path(args.log).with_suffix("")
    table.to_csv(f"{stem}_compare.csv", index=False)
    plot_pr(curves, test["label"].mean(), f"{stem}_pr.png")
    print(f"\n-> {stem}_compare.csv, {stem}_pr.png")


if __name__ == "__main__":
    main()
