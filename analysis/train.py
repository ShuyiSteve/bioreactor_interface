"""Fit the baselines (median + MAD per feature, both feature sets) on a fault-free log.

    python train.py data/nofaults_....csv --unlabelled data/single_fault_....csv data/three_faults_....csv

--unlabelled logs tell the model features which way faults start. Their labels are
not used, and only the first half of each is read (compare.py tests on the second).
"""
import argparse

import config
import detector
from load import describe, load_csv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--unlabelled", nargs="*", default=[])
    args = ap.parse_args()
    path = args.log
    df, cols = load_csv(path)
    print("columns picked:\n" + describe(cols) + "\n")
    if df["label"].any():
        print(f"warning: {df['label'].sum()} rows have an active fault, this should be the nofaults stream")
    if len(df) < 1200:
        print(f"warning: only {len(df)} rows (~{len(df) // 60} min), the brief asks for 30 min")

    others = [load_csv(p)[0].drop(columns=["label", "faults"]) for p in args.unlabelled]
    others = [d[d["t"] < d["t"].iloc[-1] * 0.5] for d in others]
    models = [detector.fit(df, m, others) for m in ("basic", "model")]
    detector.save(models, cols, trained_on=path, n=len(df))
    for m in models:
        print(f"{m['mode']:<14}{'median':>12}{'MAD':>12}   ({len(df)} samples)")
        for n, (a, b) in m["base"].items():
            print(f"  {n:<12}{a:>12.5f}{b:>12.5f}")
    print(f"\nsaved -> {config.BASELINE_FILE}")


if __name__ == "__main__":
    main()
