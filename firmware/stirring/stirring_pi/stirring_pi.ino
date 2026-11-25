// Stirring bench sketch (Lily, Yiwen), from the 23 Nov version in the shared plan.
// Review fixes (25 Nov): PWM ramp limit with matching anti-windup, set point
// clamped to 500-1500 RPM (0 = stop), default 1000 RPM.
#include <Arduino.h>

// ============================================================
// Stirring motor: Hall-sensor RPM + PI speed control
// Board: Arduino Nano ESP32
//
// Serial commands (115200 baud, end with Enter / newline):
//   S<rpm>   set target speed, e.g. S1000   (turns auto-step off)
//   A        toggle auto-step 1000 <-> 1500 RPM every 10 s
//   P<val>   set Kp, e.g. P0.8
//   I<val>   set Ki, e.g. I0.3
//   R        reset stall alarm
// Lines starting with "#" are info messages, the rest is plotter data.
// ============================================================

// ------------------------------------------------------------
// 1. Configuration
// ------------------------------------------------------------

// Pins (D2 / D6 constants work with either pin-numbering setting)
const byte HALL_PIN  = D2;   // Hall sensor signal, MUST be 3.3 V max
const byte MOTOR_PIN = D6;   // PWM to MOSFET gate

// TODO: set this to the value you MEASURE with the tachometer
// (Moodle says 70, Tips PDF says 2)
const float PULSES_PER_REV = 70;

const int RPM_MIN = 500;    // spec range 500 to 1500 RPM (0 = stop)
const int RPM_MAX = 1500;
const float RPM_DEFAULT = 1000;

// Ramp limit: the PWM may change by at most this many counts per 10 ms sample
// (0 to full scale in about 2 s), so the gears never see a sudden jump.
const int RAMP_MAX_STEP = 5;

// Ignore pulses closer together than this (noise / bounce).
// Computed for 2000 RPM so there is some headroom above RPM_MAX.
const unsigned long DEBOUNCE_US =
  (unsigned long)(6e7 / 2000.0 / PULSES_PER_REV);

// PWM (ESP32: any resolution, we use 10 bit -> 0..1023)
const int PWM_BITS = 10;
const int PWM_MAX  = 1023;
const int PWM_FREQ = 7800;

// Control loop timing
const unsigned long SAMPLE_US  = 10000;    // 10 ms sampling period
const unsigned long STOPPED_US = 100000;   // no pulse for 100 ms -> speed = 0

// Stall alarm: PWM >= STALL_PWM_MIN and no Hall pulse for STALL_TIMEOUT_US
const int STALL_PWM_MIN = 150;
const unsigned long STALL_TIMEOUT_US = 500000;   // 0.5 s

// Auto set-point step test
const float STEP_LOW  = 1000;
const float STEP_HIGH = 1500;
const unsigned long STEP_PERIOD_MS = 10000;

// Settling time definition: stays within +-SETTLE_BAND for SETTLE_HOLD_MS
const float SETTLE_BAND = 20;
const unsigned long SETTLE_HOLD_MS = 500;

// ------------------------------------------------------------
// 2. Variables
// ------------------------------------------------------------

// Shared with the interrupt (integers only, no float in the ISR)
volatile unsigned long pulseT     = 0;   // time of last accepted pulse
volatile unsigned long prevPulseT = 0;
volatile unsigned long periodAvg  = 0;   // filtered pulse period (us)
volatile unsigned long pulseCount = 0;

// Timing
unsigned long prevTimeUs   = 0;
unsigned long nextSampleUs = 0;

// Speed
float measSpeed = 0;     // raw RPM
float meanSpeed = 0;     // smoothed RPM (for display / step metrics)

// PI controller
float setpoint = 0;      // set in setup() via setSetpoint()
float Kp = 0.5;          // initial gains, need tuning
float Ki = 0.2;
float integral = 0;
int   Vmotor = 0;

// Stall alarm
bool stallAlarm = false;
unsigned long stallRefUs = 0;
unsigned long lastPulseSeen = 0;

// LED (blinks once per revolution)
unsigned long ledCountRef = 0;

// Auto step
bool autoStep = false;
unsigned long lastStepMs = 0;

// Step response metrics
bool  stepActive = false;
float stepFrom = 0, stepTo = 0, stepPeak = 0;
unsigned long stepStartMs = 0, lastOutsideMs = 0;

// Serial command buffer
char cmdBuf[24];
byte cmdLen = 0;

void IRAM_ATTR freqcount();

// ------------------------------------------------------------
// 3. Helpers
// ------------------------------------------------------------

void setSetpoint(float rpm) {
  // 0 (or less) means stop; anything else is clamped to the spec range.
  if (rpm <= 0) rpm = 0;
  else if (rpm < RPM_MIN) rpm = RPM_MIN;
  else if (rpm > RPM_MAX) rpm = RPM_MAX;
  if (rpm == setpoint) return;

  stepFrom = setpoint;
  stepTo = rpm;
  stepPeak = 0;
  stepActive = true;
  stepStartMs = millis();
  lastOutsideMs = stepStartMs;
  setpoint = rpm;
}

void processCommand(char *buf) {
  char cmd = toupper(buf[0]);
  float val = atof(buf + 1);

  switch (cmd) {
    case 'S':
      autoStep = false;
      setSetpoint(val);
      Serial.printf("# setpoint = %.0f RPM\n", setpoint);
      break;

    case 'A':
      autoStep = !autoStep;
      if (autoStep) {
        lastStepMs = millis();
        setSetpoint(STEP_LOW);
      }
      Serial.printf("# auto-step %s\n", autoStep ? "ON" : "OFF");
      break;

    case 'P':
      Kp = val;
      Serial.printf("# Kp = %.3f\n", Kp);
      break;

    case 'I':
      // keep the integral term's contribution constant when Ki changes
      if (val > 0) integral = integral * Ki / val;
      else integral = 0;
      Ki = val;
      Serial.printf("# Ki = %.3f\n", Ki);
      break;

    case 'R':
      stallAlarm = false;
      integral = 0;
      stallRefUs = micros();
      Serial.println("# stall alarm reset");
      break;

    default:
      Serial.println("# unknown command (S<rpm> A P<val> I<val> R)");
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = 0;
        processCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ------------------------------------------------------------
// 4. Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(HALL_PIN, INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  analogWriteResolution(MOTOR_PIN, PWM_BITS);
  analogWriteFrequency(MOTOR_PIN, PWM_FREQ);
  analogWrite(MOTOR_PIN, 0);

  attachInterrupt(digitalPinToInterrupt(HALL_PIN), freqcount, RISING);

  setSetpoint(RPM_DEFAULT);   // initial target (1000 RPM, mid-range)

  unsigned long now = micros();
  prevTimeUs = now;              // avoids a huge deltaT on the first sample
  nextSampleUs = now + SAMPLE_US;
  stallRefUs = now;
}

// ------------------------------------------------------------
// 5. Main loop
// ------------------------------------------------------------

void loop() {
  handleSerial();

  unsigned long now = micros();
  if ((long)(now - nextSampleUs) < 0) return;

  float deltaT = (now - prevTimeUs) * 1e-6f;
  prevTimeUs = now;
  nextSampleUs = now + SAMPLE_US;

  // ---- Auto set-point step ----
  if (autoStep && (millis() - lastStepMs >= STEP_PERIOD_MS)) {
    lastStepMs = millis();
    setSetpoint(setpoint == STEP_LOW ? STEP_HIGH : STEP_LOW);
  }

  // ---- Read shared data safely ----
  noInterrupts();
  unsigned long lastPulse = pulseT;
  unsigned long period    = periodAvg;
  unsigned long count     = pulseCount;
  interrupts();

  // ---- Measured speed ----
  now = micros();
  if ((long)(now - lastPulse) > (long)STOPPED_US || period == 0) {
    measSpeed = 0;                       // motor stopped
  } else {
    measSpeed = 6e7f / ((float)period * PULSES_PER_REV);
  }
  meanSpeed = 0.1f * measSpeed + 0.9f * meanSpeed;

  // ---- PI control ----
  if (stallAlarm) {
    Vmotor = 0;
    integral = 0;
  } else {
    float error = setpoint - measSpeed;
    float P = Kp * error;

    float candidateIntegral = integral + error * deltaT;
    float candidateOutput = P + Ki * candidateIntegral;
    int candidateTarget = constrain((int)candidateOutput, 0, PWM_MAX);
    int candidateLimited = constrain(candidateTarget, Vmotor - RAMP_MAX_STEP, Vmotor + RAMP_MAX_STEP);

    // Anti-windup: don't integrate while the output is held back, either by
    // saturation (0 or PWM_MAX) or by the ramp limit, unless the error is
    // pulling the output back in.
    bool heldBySaturation = (candidateOutput > PWM_MAX && error > 0) ||
                            (candidateOutput < 0 && error < 0);
    bool heldByRamp = (candidateLimited != candidateTarget) &&
                      ((candidateTarget > candidateLimited) == (error > 0));
    if (!heldBySaturation && !heldByRamp) {
      integral = candidateIntegral;
    }

    float controlOutput = P + Ki * integral;
    int target = constrain((int)controlOutput, 0, PWM_MAX);
    Vmotor = constrain(target, Vmotor - RAMP_MAX_STEP, Vmotor + RAMP_MAX_STEP);

    // ---- Stall detection ----
    // Reference time restarts whenever a pulse arrives or PWM is low.
    if (Vmotor < STALL_PWM_MIN || lastPulse != lastPulseSeen) {
      lastPulseSeen = lastPulse;
      stallRefUs = now;
    } else if ((long)(now - stallRefUs) > (long)STALL_TIMEOUT_US) {
      stallAlarm = true;
      autoStep = false;
      stepActive = false;
      Vmotor = 0;
      integral = 0;
      Serial.println("# STALL ALARM: no Hall pulses while driving. Motor stopped. Send R to reset.");
    }
  }

  analogWrite(MOTOR_PIN, Vmotor);

  // ---- LED: blink once per revolution, solid ON when stall alarm ----
  if (stallAlarm) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    unsigned long half = (unsigned long)(PULSES_PER_REV / 2);
    if (half < 1) half = 1;
    if (count - ledCountRef >= half) {
      ledCountRef = count;
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
  }

  // ---- Step response metrics (settling time + overshoot) ----
  if (stepActive) {
    float dir = (stepTo > stepFrom) ? 1.0f : -1.0f;
    float excursion = dir * (meanSpeed - stepTo);
    if (excursion > stepPeak) stepPeak = excursion;

    unsigned long ms = millis();
    if (fabsf(meanSpeed - stepTo) > SETTLE_BAND) {
      lastOutsideMs = ms;
    } else if (ms - lastOutsideMs >= SETTLE_HOLD_MS) {
      stepActive = false;
      float stepSize = fabsf(stepTo - stepFrom);
      Serial.printf(
        "# STEP %.0f->%.0f RPM: settling(+-%.0f RPM) = %lu ms, overshoot = %.0f RPM (%.1f %%)\n",
        stepFrom, stepTo, SETTLE_BAND,
        lastOutsideMs - stepStartMs,
        stepPeak, 100.0f * stepPeak / stepSize);
    }
  }

  // ---- Serial Plotter ----
  Serial.print("Target:");
  Serial.print(setpoint);
  Serial.print(",Actual:");
  Serial.print(meanSpeed);
  Serial.print(",PWM:");
  Serial.println(Vmotor);
}

// ------------------------------------------------------------
// 6. Hall sensor interrupt (integer maths only)
// ------------------------------------------------------------

void IRAM_ATTR freqcount() {
  unsigned long now = micros();
  unsigned long interval = now - prevPulseT;

  if (interval > DEBOUNCE_US) {
    pulseT = now;
    prevPulseT = now;

    // Filter the pulse period: 3/4 old + 1/4 new.
    // Re-initialise after a long gap (motor was stopped / first pulse).
    if (periodAvg == 0 || interval > STOPPED_US) {
      periodAvg = interval;
    } else {
      periodAvg = periodAvg - (periodAvg >> 2) + (interval >> 2);
    }

    pulseCount++;
  }
}
