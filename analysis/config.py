"""Settings. Column names are auto-detected from the first CSV; only set them
here if the auto-detect picks the wrong one (train.py prints what it picked)."""

# --- Course simulator (Moodle week 5). UCL network or VPN only. ------------
BROKER = "engf0001.cs.ucl.ac.uk"
PORT = 1883
TOPIC = "bioreactor_sim/{stream}/telemetry/summary"
STREAMS = ["nofaults", "single_fault", "three_faults", "variable_setpoints"]

# Where --publish sends alerts, as {"level", "message"}. The dashboard server listens on
# bioreactor/<device>/alerts on its own broker (its .env). To confirm with Kendrick.
ALERTS_BROKER, ALERTS_PORT = "broker.hivemq.com", 1883
ALERTS_TOPIC = "bioreactor/sim/alerts"

# --- Columns (flattened JSON keys, as in the logger's CSV) -----------------
# None = auto-detect. Extra features can be added by name, e.g.
#   "heater": "actuators.heater_duty"   # may help catch heater_power_loss
FEATURES = {"temp": None, "ph": None, "rpm": None}
# Set point column per feature. None = auto-detect, False = don't use.
SETPOINTS = {"temp": None, "ph": None, "rpm": None}
# Score (reading - set point) instead of the raw reading when a set point is
# logged. Needed for variable_setpoints, harmless on the others.
USE_SETPOINT_RESIDUAL = True
# Column with the active-fault list. The stream also has faults.counts.* (samples per
# fault in each 1 s window), which auto-detect would pick up as extra labels.
FAULTS = "faults.last_active"

# Other columns the model features use.
EXTRA = {"time": "window.start", "samples": "window.samples",
         "heater_pwm": "actuators_avg.heater_pwm", "heater_wh": "heater_energy_Wh",
         "acid_pwm": "actuators_avg.acid_pwm", "base_pwm": "actuators_avg.base_pwm"}

# --- Detector (starting values from the brief, tune these) -----------------
Z_ON = 12.0   # start an alert when |z| goes above this (the brief starts at 3.5)
Z_OFF = 9.0   # clear it when |z| drops below this (the brief starts at 2.5)
MODE = "model"   # "basic" (reading - set point, as in the brief) or "model" (see features.py)
RULE = "hysteresis"   # or "window": alert on the share of the last WINDOW_N samples
WINDOW_N, P_ON, P_OFF = 10, 0.6, 0.5   # slide 19
POOLED = False   # score sqrt(sum z^2) over the features instead of each on its own (slide 15)

# --- Model features --------------------------------------------------------
JUMP_SD = {"temp": 15, "ph": 15}   # a one-second change over this many normal SDs is a jump
DOSE_GAIN = 60      # pH units/s a full-duty acid or base dose could explain
MAX_GAP_S = 3       # don't look for jumps across gaps in the data
MAX_FAULT_S = 300   # give up on an offset after this long (faults last ~60 s)
BASELINE_FILE = "baseline.json"
RESULTS_FILE = "results.csv"
