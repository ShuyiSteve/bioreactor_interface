"""Live scoreboard for the final session: scores every sample as it arrives and
keeps running TP / TN / FP / FN counts (when the stream has labels).

    python live.py three_faults                     # live from the simulator (UCL network/VPN)
    python live.py three_faults --publish           # also send alerts to config.ALERTS_TOPIC
    python live.py --replay data/fake_three_faults.csv --speed 20   # practise offline

Prints one updating status line, plus a permanent line whenever an alert
starts or clears. Ctrl-C prints the final score.
"""
import argparse
import json
import queue
import sys
import time

import pandas as pd

import config
import detector
from evaluate import Score
from load import describe, flatten, parse_row, resolve

RED, GREEN, DIM, RESET = "\033[31m", "\033[32m", "\033[2m", "\033[0m"


def mqtt_rows(stream, q, publish_to=None):
    import paho.mqtt.client as mqtt

    topic = config.TOPIC.format(stream=stream)

    def on_connect(c, u, f, rc, p):
        print(f"{DIM}connected ({rc}), subscribed to {topic}{RESET}")
        c.subscribe(topic)

    def on_message(c, u, msg):
        try:
            q.put(flatten(json.loads(msg.payload)))
        except (json.JSONDecodeError, AttributeError):
            pass

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect, client.on_message = on_connect, on_message
    client.connect(config.BROKER, config.PORT, 60)
    client.loop_start()
    while True:
        yield q.get()


def replay_rows(path, speed):
    for row in pd.read_csv(path).to_dict("records"):
        yield row
        if speed > 0:
            time.sleep(1 / speed)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream", nargs="?", default="three_faults", choices=config.STREAMS)
    ap.add_argument("--replay", metavar="CSV", help="replay a logged CSV instead of connecting")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed-up (0 = as fast as possible)")
    ap.add_argument("--publish", nargs="?", const=config.ALERTS_TOPIC, metavar="TOPIC",
                    help=f"publish a message to TOPIC when an alert starts, changes or clears (default {config.ALERTS_TOPIC})")
    ap.add_argument("--mode", choices=["basic", "model"], default=config.MODE)
    ap.add_argument("--rule", choices=["hysteresis", "window"], default=config.RULE)
    ap.add_argument("--z-on", type=float, default=config.Z_ON)
    ap.add_argument("--z-off", type=float, default=config.Z_OFF)
    args = ap.parse_args()

    det = detector.Detector(detector.load(args.mode), args.z_on, args.z_off, rule=args.rule)
    score, cols, labelled, was_named, t = Score(), None, False, "", 0

    pub = None
    if args.publish:
        import paho.mqtt.client as mqtt
        pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        pub.connect(config.ALERTS_BROKER, config.ALERTS_PORT, 60)
        pub.loop_start()

    rows = replay_rows(args.replay, args.speed) if args.replay else mqtt_rows(args.stream, queue.Queue())
    try:
        for row in rows:
            if cols is None:
                cols = resolve(row.keys())
                print(f"{DIM}columns:\n{describe(cols)}{RESET}")
            rec = parse_row(row, cols)
            if rec["time"] is None:
                rec["time"] = t
            alert, z = det.update(rec)
            labelled = labelled or bool(cols["faults"])
            if labelled:
                score.add(rec["label"], alert)

            named = ",".join(det.named()) or det.alert_by()
            if named != was_named:
                who = named
                msg = f"{RED}ALERT ON  ({who}){RESET}" if alert else f"{GREEN}alert cleared{RESET}"
                truth = f"  truth: {rec['faults'] or 'no fault'}" if labelled else ""
                print(f"\r\033[K{t:>6}s  {msg}{truth}")
                if pub:
                    level, text = ("warn", f"fault detected: {named}") if alert else ("info", "fault cleared")
                    pub.publish(args.publish, json.dumps({"level": level, "message": f"{args.stream}: {text}"}))
                was_named = named
            zs = " ".join(f"{n} {v:+6.1f}" for n, v in z.items())
            state = f"{RED}ALERT{RESET}" if alert else f"{GREEN} ok  {RESET}"
            board = (f"TP {score.tp:>4} TN {score.tn:>5} FP {score.fp:>4} FN {score.fn:>4} | "
                     f"P {score.precision:.2f} R {score.recall:.2f} F1 {score.f1:.2f}") if labelled else "no labels"
            sys.stdout.write(f"\r\033[K{t:>6}s {state} z: {zs} | {board}")
            sys.stdout.flush()
            t += 1
    except KeyboardInterrupt:
        pass
    print(f"\n\n{t} samples scored")
    if labelled:
        print(json.dumps(score.as_dict()))


if __name__ == "__main__":
    main()
