# log_stream.py: log an ENGF0001 simulator stream to CSV (UCL network or VPN)
# From the team plan's Data Analysis tab; changes: saves into data/, and also keeps
# the raw messages as .jsonl (the CSV drops fault counts missing from the first message).
#   python log_stream.py nofaults        (30 min)
#   python log_stream.py three_faults    (20+ min)
import csv, json, os, sys, time
import paho.mqtt.client as mqtt

BROKER, PORT = "engf0001.cs.ucl.ac.uk", 1883
STREAM = sys.argv[1] if len(sys.argv) > 1 else "nofaults"
TOPIC = f"bioreactor_sim/{STREAM}/telemetry/summary"
os.makedirs("data", exist_ok=True)
OUT = f"data/{STREAM}_{time.strftime('%Y%m%d_%H%M%S')}.csv"

def flatten(d, prefix=""):
    out = {}
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.update(flatten(v, key + "."))
        elif isinstance(v, list):
            out[key] = json.dumps(v)  # e.g. the list of active faults
        else:
            out[key] = v
    return out

f = open(OUT, "w", newline="")
raw = open(OUT.replace(".csv", ".jsonl"), "w")
writer, count = None, 0

def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected ({reason_code}). Subscribing to {TOPIC}")
    client.subscribe(TOPIC)

def on_message(client, userdata, msg):
    global writer, count
    data = json.loads(msg.payload)
    raw.write(json.dumps({"rx": time.time(), **data}) + "\n")
    raw.flush()
    row = flatten(data)
    if writer is None:
        print("First message (check the field names):")
        print(json.dumps(data, indent=2))
        writer = csv.DictWriter(f, fieldnames=list(row.keys()), extrasaction="ignore")
        writer.writeheader()
    writer.writerow(row)
    f.flush()
    count += 1
    if count % 60 == 0:
        print(f"{count} messages saved to {OUT}")

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT, 60)
try:
    client.loop_forever()
except KeyboardInterrupt:
    f.close()
    raw.close()
    print(f"\nStopped. {count} messages in {OUT}")
