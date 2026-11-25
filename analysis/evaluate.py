"""Per-sample scoring: every sample is a TP, TN, FP or FN (as the brief asks)."""


class Score:
    def __init__(self):
        self.tp = self.tn = self.fp = self.fn = 0

    def add(self, label, alert):
        if label and alert:
            self.tp += 1
        elif label:
            self.fn += 1
        elif alert:
            self.fp += 1
        else:
            self.tn += 1

    @property
    def precision(self):
        return self.tp / (self.tp + self.fp) if self.tp + self.fp else 0.0

    @property
    def recall(self):
        return self.tp / (self.tp + self.fn) if self.tp + self.fn else 0.0

    @property
    def f1(self):
        p, r = self.precision, self.recall
        return 2 * p * r / (p + r) if p + r else 0.0

    def as_dict(self):
        return {"tp": self.tp, "tn": self.tn, "fp": self.fp, "fn": self.fn,
                "precision": round(self.precision, 3), "recall": round(self.recall, 3), "f1": round(self.f1, 3)}


def score(df):
    s = Score()
    for label, alert in zip(df["label"], df["alert"]):
        s.add(label, alert)
    return s


def recall_by_fault(df):
    """Fraction of samples flagged while each fault is active, to see which faults get missed."""
    out = {}
    for faults, alert in zip(df["faults"], df["alert"]):
        for f in filter(None, str(faults).split(",")):
            n, hit = out.get(f, (0, 0))
            out[f] = (n + 1, hit + bool(alert))
    return {f: {"samples": n, "recall": round(h / n, 3)} for f, (n, h) in sorted(out.items())}


def pr_curve(df):
    """(recall, precision, average precision) of the raw score over every threshold (slides 26-27)."""
    d = df[["score", "label"]].fillna({"score": 0}).sort_values("score", ascending=False)
    tp = d["label"].cumsum()
    count = d.assign(i=range(1, len(d) + 1)).groupby("score")["i"].max().sort_index(ascending=False)
    tps = d.assign(tp=tp).groupby("score")["tp"].max().sort_index(ascending=False)
    p, r = tps / count, tps / max(d["label"].sum(), 1)
    ap = float((r.diff().fillna(r.iloc[0]) * p).sum())
    return r.tolist(), p.tolist(), ap


def timing(df, tail=30):
    """How late alerts start and how many false positives fall just after a fault (slide 8)."""
    t, y, a = df["t"].tolist(), df["label"].tolist(), df["alert"].tolist()
    delays, missed, after, last_end = [], 0, 0, None
    start = hit = None
    for i in range(len(y) + 1):
        on = i < len(y) and y[i]
        if on and start is None:
            start, hit = t[i], None
        if on and hit is None and a[i]:
            hit = t[i] - start
        if not on and start is not None:
            if hit is None:
                missed += 1
            else:
                delays.append(hit)
            start, last_end = None, t[i - 1]
        if i < len(y) and not y[i] and a[i] and last_end is not None and t[i] - last_end <= tail:
            after += 1
    return {"faults": len(delays) + missed, "missed": missed,
            "median_delay_s": sorted(delays)[len(delays) // 2] if delays else None,
            "max_delay_s": max(delays, default=None), f"fp_within_{tail}s_after": after}


def naming(df):
    """Share of samples where exactly the right set of faults was named."""
    ok = [set(filter(None, str(f).split(","))) == set(filter(None, str(g).split(",")))
          for f, g in zip(df["faults"], df["named"])]
    faulty = [o for o, l in zip(ok, df["label"]) if l]
    return {"all": round(sum(ok) / len(ok), 3), "while_faulty": round(sum(faulty) / len(faulty), 3) if faulty else None}
