# Data Analysis: fault detector

Robust z-score detector (median + MAD baseline, hysteresis alerts) for the ENGF0001
simulator streams, scored per sample (TP/TN/FP/FN, precision, recall, F1).
Labels are only used for scoring, never by the detector.

## Setup

```bash
uv venv --python 3.14 && source .venv/bin/activate
uv pip install -r requirements.txt
```

## Workflow (UCL network or VPN for steps 1 and 5)

```bash
python log_stream.py nofaults          # 30 min, Ctrl-C to stop -> data/nofaults_<time>.csv
python log_stream.py three_faults      # 20+ min
python train.py data/nofaults_<time>.csv --unlabelled data/single_fault_<time>.csv data/three_faults_<time>.csv
python run.py data/three_faults_<time>.csv                # P/R/F1, timing, plot, row in results.csv
python run.py data/three_faults_<time>.csv --mode basic --z-on 3.5 --z-off 2.5   # same, with the brief's plain z-score
python compare.py data/three_faults_<time>.csv            # all variants, tuned on the first half, tested on the second
python live.py three_faults                               # live scoreboard for the demo
python live.py three_faults --publish                     # ...and send alerts (agree the topic first)
```

Rehearse the demo offline with `python live.py --replay data/three_faults_<time>.csv --speed 20`.

## Method

The score is the brief's: z = 0.6745 (x - median) / MAD, with hysteresis. `--mode`
picks what x is (details in `features.py`):

- `basic`: reading - set point for temperature, pH and RPM. After a fault ends the
  controller takes minutes to recover, so this keeps alerting on healthy samples.
- `model`: one feature per fault. Heater energy per unit of heater duty (power loss),
  and the running sum of jumps too fast to be real in temperature and pH (sensor bias).
  Also names the fault. Which way a bias fault jumps comes from unlabelled logs:
  the reading spends less time biased than not.

`--rule window` (share of the last N samples) and `--pooled` are the slides' alternatives.

## Files

| file | what it does |
|---|---|
| `config.py` | broker, column overrides, thresholds, alerts topic |
| `log_stream.py` | the team's logger (saves into `data/`) |
| `load.py` | CSV row or live message → features, set-point residuals, fault label |
| `features.py` | basic and model features, no labels |
| `detector.py` | median/MAD baseline, z-score, hysteresis, the same code offline and live |
| `evaluate.py` | TP/TN/FP/FN, precision/recall/F1, recall per fault, PR curve, delays, naming |
| `compare.py` | tunes and tests every variant, PR plot |
| `train.py` / `run.py` / `live.py` | fit baseline / score a log / live scoreboard |
| `fake_stream.py` | old synthetic logs, guessed fields, only usable with `--mode basic` |

`results.csv` collects every `run.py` result (date, stream, thresholds, scores) for the
Results table and the report.

## Results (25 Nov, 10:15-10:34, settings frozen beforehand)

| stream | plain z-score F1 (3.5 / 2.5) | model features F1 | precision | recall |
|---|---|---|---|---|
| single_fault | 0.26 | 1.00 | 1.00 | 1.00 |
| three_faults | 0.55 | 0.996 | 1.00 | 0.993 |
| variable_setpoints | 0.55 | 0.994 | 0.993 | 0.995 |

Caveats: few fault periods (4 on single_fault, 8 on three_faults, and variable_setpoints
has the same faults as three_faults). Only one set point change seen, and the detector was fixed
after it. The features and thresholds were chosen while looking at labelled data from
earlier in the same recording. A bias fault already on at start-up can be missed. Not tested
on our rig. `data/*_pr.png` uses the second half of the log, part of which was used
while developing. The final runs are in `results.csv`; the raw logs aren't committed.
