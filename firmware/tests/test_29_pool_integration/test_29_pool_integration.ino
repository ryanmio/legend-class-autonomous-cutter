/*
 * test_29_pool_integration.ino
 *
 * FIRST WATER TEST. Production-direction sketch the operator flashes for
 * the pool. No bench gates, no auto-detected phases — just operate.
 *
 * Merges:
 *   test_27  Mode FSM (MANUAL/AUTO/FAILSAFE), CH6 SwD failsafe guard with
 *            sticky ACK, /cruise, /telemetry, INA219 voltage telemetry.
 *   test_25  Single /waypoint + haversine bearing + heading-hold.
 *   NEW      Capture detection (sticky), live /pid tuning, /sim_gps for
 *            bench dry-runs of the app.
 *
 * What's NEW vs test_27:
 *   - POST /waypoint {lat,lon} (and {lat:null,lon:null} to clear) — single
 *     waypoint controls AUTO. App's MapScreen already wires this.
 *   - Capture detection: when wp_dist < 3 m, sticky `captured` flag fires;
 *     ESCs + rudder forced neutral until operator clears or moves the
 *     waypoint. Equivalent to "mission complete" for a single-leg mission.
 *   - POST /pid {kp, kd} for live heading-hold tuning during the run. No
 *     Ki yet — we want to see how P+D behaves in water before adding I.
 *   - POST /sim_gps {lat,lon} as a bench-debug injection. Sticky for the
 *     session; lets the operator dry-run app↔firmware without GPS fix.
 *   - AUTO no longer requires cruise above any floor. Cruise=1500 (neutral)
 *     is now valid, so the static-heading-hold scenario in AUTOPILOT_PLAN
 *     test_32 step 2 ("AUTO at cruise=neutral, rudder tracks waypoint")
 *     just works. Cap stays at 1750 µs to prevent runaway.
 *   - AUTO without a waypoint = neutral. Removes test_27's "hold heading
 *     at entry" placeholder; from here, AUTO means "drive to a waypoint."
 *   - Telemetry adds: gps_fix, gps_simulated, lat, lon, sats, speed_kts,
 *     course, wp_set, wp_lat, wp_lon, wp_dist_m, wp_bearing, captured,
 *     pid_kp, pid_kd.
 *
 * What this sketch deliberately does NOT do:
 *   - Multi-waypoint missions (pool too small; deferred to lake/bay).
 *   - Cross-track guidance (plan-test_30).
 *   - Integral term (plan-test_31; pool tuning is P+D only).
 *   - GPS-loss failsafe (sea-trial scope).
 *   - /estop, /rth, /set-home, /mode (RC failsafe + SwD physical guard
 *     cover the safety case for pool; production firmware adds these).
 *
 * Channel map (locked 2026-05-10):
 *   CH1 (idx 0)  rudder           CH5 (idx 4)  knob
 *   CH2 (idx 1)  reverse          CH6 (idx 5)  SwD failsafe guard
 *   CH3 (idx 2)  throttle         CH7 (idx 6)  SwA mode (up=MANUAL,
 *   CH4 (idx 3)  unused                          down=AUTO)
 *
 * Pool pre-flight (verify these BEFORE getting the boat wet):
 *   - Bilge MOSFET fix verified (signal wire 15 → 13; radar 13 → 15;
 *     2026-05-12). Boat-side check: LED off + pump silent at idle, ESP32
 *     boots normally. See PLANNING_NOTES.md.
 *   - Hatch waterproofing verified.
 *   - Manual TX control: rudder + throttle work.
 *   - SwA at MANUAL boot position. SwD at up.
 *   - Phone connected, /telemetry visible, voltage reading sane.
 *   - PID conservative: Kp=3.0, Kd=8.0 (test_27 / test_28 baseline).
 *   - Cruise via app — start low (e.g. 1660) and let drag work.
 *   - Props on, rudder linkage free, hatch sealed.
 *
 * SAFETY:
 *   AUTO cruise cap 1750 µs. Hard ESC clamp at MAX_FWD_US=1800 in
 *   setEscs(). Captured boat freezes at neutral until operator intervenes.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_INA219.h>
#include "ICM_20948.h"
#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <DFRobot_DF1201S.h>
#include <math.h>
#include "secrets.h"

// ── PCA9685 channels ────────────────────────────────────────────────────────
static const uint8_t  CH_ESC_PORT = 0;
static const uint8_t  CH_ESC_STBD = 1;
static const uint8_t  CH_RUDDER   = 2;

// ── Output bounds ───────────────────────────────────────────────────────────
static const uint16_t RUDDER_MIN_US = 1330;
static const uint16_t RUDDER_MAX_US = 1670;
static const uint16_t NEUTRAL_US    = 1500;
static const uint16_t MAX_FWD_US    = 1800;

// ── AUTO cruise selection (no floor; cap only) ─────────────────────────────
// cruise=NEUTRAL_US is valid for static heading-hold (AUTOPILOT_PLAN
// test_32 step 2). The cap prevents an in-water hands-off runaway.
static const uint16_t AUTO_CRUISE_CAP_US = 1750;
static const uint16_t DEFAULT_CRUISE_US  = 1660;

// ── iBUS channel indices (locked 2026-05-10) ────────────────────────────────
static const uint8_t  IBUS_IDX_RUDDER         = 0;
static const uint8_t  IBUS_IDX_THROTTLE       = 2;
static const uint8_t  IBUS_IDX_FAILSAFE_GUARD = 5;
static const uint8_t  IBUS_IDX_MODE           = 6;
static const uint8_t  IBUS_RX_PIN             = 16;

// ── SwA hysteresis ──────────────────────────────────────────────────────────
static const uint16_t MODE_MAN_BELOW_US  = 1450;
static const uint16_t MODE_AUTO_ABOVE_US = 1550;

// ── Stick safe-arm ──────────────────────────────────────────────────────────
static const uint16_t THROTTLE_IDLE_MAX = 1100;

// ── Failsafe ────────────────────────────────────────────────────────────────
static const uint16_t FAILSAFE_GUARD_THRESHOLD = 1500;
static const uint32_t FAILSAFE_DETECT_MS       = 500;
static const uint32_t FAILSAFE_NO_FRAME_MS     = 3000;

// ── IMU + heading hold ──────────────────────────────────────────────────────
static const uint32_t IMU_UPDATE_INTERVAL_MS = 20;
// Default gains; live-tunable via POST /pid {kp,kd}. Conservative starting
// values from test_27/test_28; pool is the first place we'll see how they
// behave under real water resistance.
static const float    DEFAULT_KP = 3.0f;
static const float    DEFAULT_KD = 8.0f;
static const float    MAG_OFFSET_X = -20.70f;
static const float    MAG_OFFSET_Y =  -0.45f;
static const float    MAG_OFFSET_Z = -17.70f;
static const float    ALPHA = 0.98f;

// ── INA219 ──────────────────────────────────────────────────────────────────
static const uint8_t INA219_ADDR = 0x41;

// ── GPS ─────────────────────────────────────────────────────────────────────
static const uint8_t  GPS_RX_PIN = 17;
static const uint8_t  GPS_TX_PIN = 4;
static const uint32_t GPS_BAUD   = 9600;

// ── LED pins ────────────────────────────────────────────────────────────────
static const uint8_t PIN_NAV    = 18;
static const uint8_t PIN_BRIDGE = 19;
static const uint8_t PIN_DECK   = 23;

// ── Depth sonar (RCWL-1655, bottom-facing) ──────────────────────────────────
// RCWL-1655 — NOT a JSN-SR04T. Pin labels on the board are non-standard
// (TRIG_RX_SCL_I/O = trigger, ECHO_TX_SDA = echo). Confirmed bench-good
// 2026-03-15 in test_04.
//   Range: 2 cm to ~450 cm. Echo timeout (no return) → reading reported
//   as null in telemetry (better to show "no echo" than a phantom zero).
//   pulseIn() blocks up to ~40 ms per ping — at ≤1 ping per 20 s this
//   is negligible for iBUS / IMU / WiFi.
//   Sound speed: 13.4 µs/cm freshwater round-trip (matches production
//   config.h SONAR_SOUND_SPEED_US_CM). In AIR sound is ~4.3× slower,
//   so bench tests will show distances ~4.3× LARGER than reality —
//   e.g., 30 cm wall reads as ~130 cm. Sanity-check accordingly.
static const uint8_t  PIN_SONAR_TRIG        = 27;
static const uint8_t  PIN_SONAR_ECHO        = 14;
static const float    SONAR_US_PER_CM       = 13.4f;     // freshwater round-trip
static const uint32_t DEPTH_PING_TIMEOUT_US = 40000;     // covers max range in AIR too (~26 ms for 4.5 m)
static const uint32_t DEPTH_RUN_INTERVAL_MS = 20000;     // 20 s between RUN pings

// ── Bilge: 3 water sensors + 1 pump MOSFET ──────────────────────────────────
// Sensor probes: active LOW (commercial water modules and bare probes with
// internal pullup both pull the GPIO down when wet). Pump is active HIGH
// via the MOSFET fixed 2026-05-12 (signal wire moved to GPIO 13).
//   GPIO 5  is a strapping pin but only governs SDIO-slave boot mode (which
//           we don't use). Safe as a sensor input for normal flash boot.
//           Internal pullup gives us a defined HIGH=dry state at idle.
static const uint8_t PIN_BILGE_FWD_SENSOR  = 32;   // forward compartment probe
static const uint8_t PIN_BILGE_MID_SENSOR  = 33;   // main bilge probe (at pump)
static const uint8_t PIN_BILGE_REAR_SENSOR = 5;    // rear compartment probe
static const uint8_t PIN_BILGE_PUMP        = 13;   // MOSFET gate, active HIGH

static const uint32_t BILGE_DRY_DELAY_MS    = 5000;   // keep pumping this long after all sensors read dry
static const uint32_t BILGE_MANUAL_TIMEOUT_MS = 60000; // manual /bilge {on:true} auto-clears after this
// Run-dry protection: if a sensor is stuck wet (corrosion, broken
// wire shorted to GND, debris bridging probes), force pump off after
// this long to save the battery and the pump motor. Matches
// production firmware's BILGE_MAX_RUN_MS. After cutoff, sensors must
// actually go dry once before the pump can re-engage automatically —
// otherwise stuck-wet would just restart the timer in a loop.
static const uint32_t BILGE_MAX_RUN_MS      = 60000;

// ── Radar motor (TRS-3D mast dish) ──────────────────────────────────────────
// 3V planetary gear motor switched by a 2N2222 NPN on the low side.
// ESP32 GPIO → 1kΩ → base; motor between 3.3V rail and collector;
// emitter to GND. PWM via LEDC peripheral at 20 kHz (above audible).
//
// GPIO 2 chosen because it's the only pin truly unclaimed in config.h
// AND test_29. It's a boot-mode strapping pin but only matters when
// GPIO 0 is also LOW (download mode) — normal boot ignores it.
// Side effect: GPIO 2 drives the onboard dev-board blue LED — that LED
// will track radar PWM, a useful indicator (will appear dim at low duty).
// Supersedes the old PCA9685 ch7 / GPIO 13 MOSFET designs.
//
// NOTE: PWM-driving an inductive load (motor coil) without a flyback
// diode anti-parallel across the motor will eventually fry the 2N2222
// from back-EMF spikes. Operator (Ryan) is shipping without a diode for
// now — add a 1N4148/1N4001 across the motor before sustained use.
// LEDC API note: ESP32 Arduino core 3.x uses pin-based ledcAttach() /
// ledcWrite(pin, ...) — no manual channel allocation. Core 2.x's
// ledcSetup() + ledcAttachPin() + ledcWrite(channel, ...) is gone.
static const uint8_t  PIN_RADAR_MOTOR      = 2;
// 1 kHz instead of "above audible" because at 20 kHz the motor coil's
// L/R time constant doesn't have time to let current build per pulse —
// low duty cycles produce almost no torque. 1 kHz pulses (250 µs ON
// at 25% duty) let current actually reach steady-state, restoring
// useful low-speed torque. Tradeoff: 1 kHz is audible as a whine.
// Acceptable for a cosmetic radar; bump to 5 kHz if the whine annoys.
static const uint32_t RADAR_PWM_FREQ_HZ    = 1000;
static const uint8_t  RADAR_PWM_RESOLUTION = 8;       // 0..255 duty range
static const uint8_t  RADAR_DEFAULT_SPEED  = 25;      // %, matches first app preset

// Burst mode: PWM at radarSpeed for radarBurstMs, then off for
// radarPauseMs, repeating. Used to simulate slow radar-look rotation
// from a too-fast motor — motor never reaches terminal velocity in a
// burst, dish covers a small angle per cycle.
// Empirically (2026-05-20) at 25% duty + burst_ms=10 the motor covered
// ~120° per burst → real motor speed is closer to 2000 RPM peak than
// the 150 RPM Amazon spec. Default burst_ms=3 should give ~36° step
// (1/10 rev). Tunable live via /radar {burst_ms, pause_ms} — no
// reflash for tweaks.
// Warning: millis()-based timing in a loop that also handles WiFi/iBUS/
// IMU jitters by ~1-2 ms. At burst_ms<5 the actual burst length will
// vary, so step size is inconsistent. Acceptable for cosmetic radar.
static const uint32_t RADAR_BURST_MS_DEFAULT = 3;
static const uint32_t RADAR_PAUSE_MS_DEFAULT = 200;
static const uint32_t RADAR_BURST_MS_MIN     = 2;        // below this, millis() timing is sketchy
static const uint32_t RADAR_BURST_MS_MAX     = 5000;
static const uint32_t RADAR_PAUSE_MS_MIN     = 0;        // 0 = continuous (degenerate to smooth)
static const uint32_t RADAR_PAUSE_MS_MAX     = 60000;

// ── DF1201S audio (HardwareSerial(2); GPS displaced to SoftwareSerial) ─────
static const uint8_t  DFP_RX_PIN = 25;   // ESP32 RX ← DF1201S TX
static const uint8_t  DFP_TX_PIN = 26;   // ESP32 TX → DF1201S RX
static const uint32_t DFP_BAUD   = 115200;
static const uint8_t  DFP_VOLUME = 20;   // 0..30
static const int16_t  DFP_TRACK  = 1;    // all 3 app buttons play track 1 for now

// ── Capture ─────────────────────────────────────────────────────────────────
static const float    CAPTURE_RADIUS_M = 3.0f;

// ── Hardware ───────────────────────────────────────────────────────────────
static Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);
static Adafruit_INA219          ina219(INA219_ADDR);
static ICM_20948_I2C            myICM;
static TinyGPSPlus              gps;
static HardwareSerial           ibusSerial(1);
// GPS on SoftwareSerial @ 9600 (well within SS limits). UART2 is reserved
// for the DF1201S, which needs 115200 — SoftwareSerial at that rate is
// unreliable on ESP32 with WiFi active (confirmed empirically, see
// test_11 NOTES). Same physical pins as before — no rewiring.
static SoftwareSerial           gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
static WebServer                server(80);
static String                   boatIP;
static bool                     ina219OK = false;

// ── Mode state machine ─────────────────────────────────────────────────────
enum Mode { MODE_MANUAL, MODE_AUTO, MODE_FAILSAFE };
static Mode    mode                = MODE_MANUAL;
static bool    failsafeAckRequired = false;
static bool    ackRefusalPrinted   = false;
static uint16_t cruiseUs           = DEFAULT_CRUISE_US;

// ── Live PID gains ──────────────────────────────────────────────────────────
static float   livePidKp = DEFAULT_KP;
static float   livePidKd = DEFAULT_KD;

// ── Waypoint state ─────────────────────────────────────────────────────────
static bool    wpSet         = false;
static float   wpLat         = 0.0f;
static float   wpLon         = 0.0f;
static float   wpDistM       = 0.0f;
static float   wpBearing     = 0.0f;
// `captured` is sticky once wp_dist < CAPTURE_RADIUS_M. Cleared when the
// operator POSTs a new /waypoint or clears it. Prevents the boat from
// re-engaging cruise after hovering across the radius.
static bool    captured      = false;

// ── Position state ──────────────────────────────────────────────────────────
// gpsSimulated is sticky: once /sim_gps is POSTed in this session, real
// GPS is ignored. Bench-debug only — operator should not POST /sim_gps
// in water.
static float   boatLat       = 0.0f;
static float   boatLon       = 0.0f;
static bool    gpsValid      = false;
static bool    gpsSimulated  = false;

// ── iBUS / RC state ─────────────────────────────────────────────────────────
static uint8_t  ibusBuf[32], ibusIdx = 0;
static uint16_t ch[10] = {0};
static bool     ibusEverGood      = false;
static uint32_t lastFrameMs       = 0;
static uint32_t guardAboveSinceMs = 0;

// ── Output state ────────────────────────────────────────────────────────────
static uint16_t outRudder = NEUTRAL_US, outPort = NEUTRAL_US, outStbd = NEUTRAL_US;

// ── LED state ───────────────────────────────────────────────────────────────
static bool navOn = false, bridgeOn = false, deckOn = false;

// ── Radar state ─────────────────────────────────────────────────────────────
// Burst-only — smooth-PWM mode was removed 2026-05-20 because at this
// motor's actual speed (~2000 RPM peak) smooth PWM at any duty looks
// like a propeller. Burst with short burst_ms is the only mode that
// produces radar-look rotation.
static bool       radarOn       = false;
static uint8_t    radarSpeed    = RADAR_DEFAULT_SPEED;   // 0..100, burst-phase duty
static uint32_t   radarBurstMs  = RADAR_BURST_MS_DEFAULT;
static uint32_t   radarPauseMs  = RADAR_PAUSE_MS_DEFAULT;

// Burst state machine.
static bool       burstActive       = false;  // true during ON phase, false during pause
static uint32_t   burstPhaseStartMs = 0;

static void writeRadarDuty(uint8_t pct) {
    uint32_t maxDuty = (1u << RADAR_PWM_RESOLUTION) - 1u;        // 255 for 8-bit
    ledcWrite(PIN_RADAR_MOTOR, maxDuty * (uint32_t)pct / 100u);
}

// Called on every state change (on/off, speed, burst/pause params).
// Resets the burst phase so changes take effect immediately.
static void applyRadarOutput() {
    if (!radarOn) {
        writeRadarDuty(0);
        return;
    }
    burstActive       = true;          // start in the burn phase
    burstPhaseStartMs = millis();
    writeRadarDuty(radarSpeed);
}

// Called every loop. Steps the burst phase timer.
static void updateRadarBurst() {
    if (!radarOn) return;
    uint32_t now      = millis();
    uint32_t phaseLen = burstActive ? radarBurstMs : radarPauseMs;
    if (now - burstPhaseStartMs >= phaseLen) {
        burstActive       = !burstActive;
        burstPhaseStartMs = now;
        writeRadarDuty(burstActive ? radarSpeed : 0);
    }
}

// ── Depth state ─────────────────────────────────────────────────────────────
enum DepthMode { DEPTH_OFF = 0, DEPTH_RUN = 1 };
static DepthMode depthMode       = DEPTH_OFF;
static float     lastDepthM      = -1.0f;   // -1 = no reading
static uint32_t  lastDepthReadMs = 0;       // 0 = never read

// RCWL-1655 trigger: pull TRIG low briefly, then HIGH for 10 µs.
// Conversion uses freshwater sound speed (13.4 µs/cm round-trip) →
// metres = µs / (13.4 × 100) = µs / 1340. See SONAR_US_PER_CM note
// for why air bench tests read inflated.
static void doDepthPing() {
    digitalWrite(PIN_SONAR_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_SONAR_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_SONAR_TRIG, LOW);

    unsigned long dur = pulseIn(PIN_SONAR_ECHO, HIGH, DEPTH_PING_TIMEOUT_US);
    lastDepthReadMs = millis();
    if (dur == 0) {
        lastDepthM = -1.0f;
        Serial.println("[DEPTH] no echo");
    } else {
        lastDepthM = (float)dur / (SONAR_US_PER_CM * 100.0f);
        Serial.printf("[DEPTH] %.2f m (dur=%lu µs)\n", lastDepthM, dur);
    }
}

static void pollDepth() {
    if (depthMode != DEPTH_RUN) return;
    if (millis() - lastDepthReadMs >= DEPTH_RUN_INTERVAL_MS) {
        doDepthPing();
    }
}

// ── Bilge state ─────────────────────────────────────────────────────────────
static bool     bilgeFwdWet = false, bilgeMidWet = false, bilgeRearWet = false;
static bool     pumpOn        = false; // current pump output state
static bool     pumpManual    = false; // operator forced via /bilge
static uint32_t lastWetMs     = 0;     // last time ANY sensor was wet
static uint32_t manualUntilMs = 0;     // monotonic deadline for pumpManual auto-clear
static uint32_t pumpOnSinceMs = 0;     // when current pump-on phase began (for MAX_RUN cutoff)
// Set true when MAX_RUN cutoff fires. Auto-pump won't re-engage until
// sensors actually go dry once (resets latch). Manual /bilge still
// works — operator can override if they're sure the leak is real.
static bool     bilgeStuckLatch = false;

// ── Audio state ─────────────────────────────────────────────────────────────
// DF1201S on HardwareSerial(2) @ 115200 — test_11's proven path.
static HardwareSerial   dfSerial(2);
static DFRobot_DF1201S  DF1201S;
static bool             audioOK = false;

// ── IMU / heading hold state ────────────────────────────────────────────────
static float    fusedHeading    = 0;
static float    prevHeadingForD = 0;
static uint32_t lastDtUs        = 0;
static unsigned long lastImuUs  = 0;
static bool     headingInit     = false;

// ── INA219 cache ────────────────────────────────────────────────────────────
static float    busVoltage = 0.0f;
static float    shuntMa    = 0.0f;
static uint32_t lastInaPollMs = 0;
static const uint32_t INA_POLL_INTERVAL_MS = 250;

// ── Helpers ────────────────────────────────────────────────────────────────
static uint16_t usTicks(uint16_t us) { return (uint16_t)((us / 20000.0f) * 4096); }
static void writePCA(uint8_t c, uint16_t us) { pca.setPWM(c, 0, usTicks(us)); }

static void setRudder(uint16_t us) {
    if (us < RUDDER_MIN_US) us = RUDDER_MIN_US;
    if (us > RUDDER_MAX_US) us = RUDDER_MAX_US;
    outRudder = us;
    writePCA(CH_RUDDER, us);
}
static void setEscs(uint16_t us) {
    if (us < NEUTRAL_US) us = NEUTRAL_US;
    if (us > MAX_FWD_US) us = MAX_FWD_US;
    outPort = outStbd = us;
    writePCA(CH_ESC_PORT, us);
    writePCA(CH_ESC_STBD, us);
}

static uint16_t mapThrottleStickToEsc(uint16_t stickUs) {
    if (stickUs <= THROTTLE_IDLE_MAX) return NEUTRAL_US;
    if (stickUs >= 2000) return MAX_FWD_US;
    return (uint16_t)map(stickUs, THROTTLE_IDLE_MAX, 2000, NEUTRAL_US, MAX_FWD_US);
}
static uint16_t mapRudderStickToServo(uint16_t stickUs) {
    if (stickUs < 1000) stickUs = 1000;
    if (stickUs > 2000) stickUs = 2000;
    if (stickUs <= 1500) return (uint16_t)map(stickUs, 1000, 1500, RUDDER_MIN_US, NEUTRAL_US);
    return                       (uint16_t)map(stickUs, 1500, 2000, NEUTRAL_US, RUDDER_MAX_US);
}

static float shortestPathError(float t, float c) {
    float e = t - c;
    while (e >  180.0f) e -= 360.0f;
    while (e < -180.0f) e += 360.0f;
    return e;
}

static bool swcInManual()    { return ch[IBUS_IDX_MODE] < MODE_MAN_BELOW_US; }
static bool swcInAuto()      { return ch[IBUS_IDX_MODE] > MODE_AUTO_ABOVE_US; }

static const char* modeName(Mode m) {
    switch (m) {
      case MODE_MANUAL:   return "MANUAL";
      case MODE_AUTO:     return "AUTO";
      case MODE_FAILSAFE: return "FAILSAFE";
    }
    return "?";
}

// ── Haversine (lifted from test_25) ────────────────────────────────────────
static float haversineBearing(float fromLat, float fromLon, float toLat, float toLon) {
    float p1 = fromLat * DEG_TO_RAD;
    float p2 = toLat   * DEG_TO_RAD;
    float dl = (toLon - fromLon) * DEG_TO_RAD;
    float y  = sinf(dl) * cosf(p2);
    float x  = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
    float b  = atan2f(y, x) * RAD_TO_DEG;
    return fmodf(b + 360.0f, 360.0f);
}
static float haversineDistM(float fromLat, float fromLon, float toLat, float toLon) {
    const float R = 6371000.0f;
    float p1 = fromLat * DEG_TO_RAD;
    float p2 = toLat   * DEG_TO_RAD;
    float dp = (toLat  - fromLat) * DEG_TO_RAD;
    float dl = (toLon  - fromLon) * DEG_TO_RAD;
    float a  = sinf(dp / 2) * sinf(dp / 2)
             + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// ── iBUS parsing ───────────────────────────────────────────────────────────
static bool parseIbus() {
    if (ibusBuf[0] != 0x20 || ibusBuf[1] != 0x40) return false;
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= ibusBuf[i];
    uint16_t rx = ibusBuf[30] | (ibusBuf[31] << 8);
    if (sum != rx) return false;
    for (int i = 0; i < 10; i++)
        ch[i] = ibusBuf[2 + i*2] | (ibusBuf[3 + i*2] << 8);
    lastFrameMs = millis();
    if (!ibusEverGood) ibusEverGood = true;
    return true;
}
static void readIbus() {
    while (ibusSerial.available()) {
        uint8_t b = ibusSerial.read();
        if (ibusIdx == 0 && b != 0x20) continue;
        ibusBuf[ibusIdx++] = b;
        if (ibusIdx == 32) { parseIbus(); ibusIdx = 0; }
    }
}

// ── IMU + complementary filter ─────────────────────────────────────────────
static void updateImu() {
    static uint32_t lastImuPollMs = 0;
    if (millis() - lastImuPollMs < IMU_UPDATE_INTERVAL_MS) return;
    if (!myICM.dataReady()) return;
    lastImuPollMs = millis();
    myICM.getAGMT();
    float ax = myICM.accX(), ay = myICM.accY(), az = myICM.accZ();
    float gx = myICM.gyrX(), gy = myICM.gyrY(), gz = myICM.gyrZ();
    float mx = myICM.magX() - MAG_OFFSET_X;
    float my = myICM.magY() - MAG_OFFSET_Y;
    float mz = myICM.magZ() - MAG_OFFSET_Z;

    float ar_x = ay, ar_y = az, ar_z = ax;
    float mr_x = -mz, mr_y = -my, mr_z = -mx;
    float roll  = atan2f(ar_y, ar_z);
    float pitch = atan2f(-ar_x, sqrtf(ar_y*ar_y + ar_z*ar_z));
    float Bx = mr_x*cosf(pitch) + mr_y*sinf(roll)*sinf(pitch) + mr_z*cosf(roll)*sinf(pitch);
    float By = mr_y*cosf(roll) - mr_z*sinf(roll);
    float magH = atan2f(-By, Bx) * 180.0f / PI;
    if (magH < 0) magH += 360.0f;
    float accelMag = sqrtf(ax*ax + ay*ay + az*az);

    unsigned long nowUs = micros();
    float dt = (lastImuUs == 0) ? 0.0f : (nowUs - lastImuUs) * 1e-6f;
    lastImuUs = nowUs;
    if (!headingInit) {
        fusedHeading = magH;
        prevHeadingForD = magH;
        headingInit = true;
    } else {
        float yawRate = (accelMag > 100.0f) ? (ax*gx + ay*gy + az*gz)/accelMag : 0.0f;
        float gyroH = fusedHeading + yawRate * dt;
        while (gyroH <   0.0f) gyroH += 360.0f;
        while (gyroH >= 360.0f) gyroH -= 360.0f;
        float diff = magH - gyroH;
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        fusedHeading = gyroH + (1.0f - ALPHA) * diff;
        if (fusedHeading <   0.0f) fusedHeading += 360.0f;
        if (fusedHeading >= 360.0f) fusedHeading -= 360.0f;
    }
}

// Heading-hold output. Uses livePidKp / livePidKd so /pid edits take
// effect immediately — no reflash.
static uint16_t headingHoldUs(float target) {
    if (!headingInit) return NEUTRAL_US;
    float err = shortestPathError(target, fusedHeading);
    uint32_t nowUs = micros();
    float dt = (lastDtUs == 0) ? 0.0f : (nowUs - lastDtUs) * 1e-6f;
    lastDtUs = nowUs;
    float dErr = 0.0f;
    if (dt > 0.0f && dt < 0.5f) {
        float dH = shortestPathError(fusedHeading, prevHeadingForD);
        dErr = -dH / dt;
    }
    prevHeadingForD = fusedHeading;
    int v = (int)(NEUTRAL_US + livePidKp * err + livePidKd * dErr);
    if (v < RUDDER_MIN_US) v = RUDDER_MIN_US;
    if (v > RUDDER_MAX_US) v = RUDDER_MAX_US;
    return (uint16_t)v;
}

// ── INA219 ─────────────────────────────────────────────────────────────────
static void pollIna219() {
    if (!ina219OK) return;
    if (millis() - lastInaPollMs < INA_POLL_INTERVAL_MS) return;
    lastInaPollMs = millis();
    busVoltage = ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0f);
    shuntMa    = ina219.getCurrent_mA();
}

// ── Bilge: read sensors, run pump on any-wet-OR-manual with dry-delay ──────
// Active LOW sensors (LOW = wet). Pump auto-runs while any sensor is wet
// and for BILGE_DRY_DELAY_MS after the last wet reading. /bilge {on:true}
// forces the pump until either /bilge {on:false} or BILGE_MANUAL_TIMEOUT_MS
// elapses — prevents a forgotten "on" from draining the battery.
//
// Run-dry protection: if the pump runs continuously for BILGE_MAX_RUN_MS
// without a single dry reading, we conclude a sensor is stuck wet and
// latch auto-pump off until sensors actually go dry. Manual override
// still works as an escape hatch.
static void pollBilge() {
    uint32_t now = millis();
    bilgeFwdWet  = (digitalRead(PIN_BILGE_FWD_SENSOR)  == LOW);
    bilgeMidWet  = (digitalRead(PIN_BILGE_MID_SENSOR)  == LOW);
    bilgeRearWet = (digitalRead(PIN_BILGE_REAR_SENSOR) == LOW);

    bool anyWet = bilgeFwdWet || bilgeMidWet || bilgeRearWet;
    if (anyWet) lastWetMs = now;
    // A real dry reading clears the stuck-sensor latch.
    if (!anyWet && bilgeStuckLatch) {
        bilgeStuckLatch = false;
        Serial.println("[BILGE] sensors dry — stuck-wet latch cleared, auto-pump re-armed.");
    }

    if (pumpManual && (int32_t)(now - manualUntilMs) >= 0) {
        pumpManual = false;
        Serial.println("[BILGE] manual override auto-cleared (timeout).");
    }

    bool autoOn = !bilgeStuckLatch
                  && (anyWet || (lastWetMs != 0 && (now - lastWetMs) < BILGE_DRY_DELAY_MS));
    bool wantOn = pumpManual || autoOn;

    // Run-dry cutoff: only applies when pump is currently running AND has
    // been running for the full max-run window. Manual override exempts
    // (operator can keep pumping if they're sure).
    if (pumpOn && !pumpManual && (now - pumpOnSinceMs) >= BILGE_MAX_RUN_MS) {
        bilgeStuckLatch = true;
        wantOn = false;
        Serial.printf("[BILGE] MAX_RUN cutoff after %lu ms — sensors likely stuck wet "
                      "(fwd=%d mid=%d rear=%d). Auto-pump LATCHED OFF until sensors go dry. "
                      "Use /bilge {on:true} to override.\n",
                      now - pumpOnSinceMs,
                      (int)bilgeFwdWet, (int)bilgeMidWet, (int)bilgeRearWet);
    }

    if (wantOn != pumpOn) {
        pumpOn = wantOn;
        if (pumpOn) pumpOnSinceMs = now;
        digitalWrite(PIN_BILGE_PUMP, pumpOn ? HIGH : LOW);
        Serial.printf("[BILGE] pump %s (fwd=%d mid=%d rear=%d manual=%d)\n",
                      pumpOn ? "ON" : "off",
                      (int)bilgeFwdWet, (int)bilgeMidWet, (int)bilgeRearWet, (int)pumpManual);
    }
}

// ── Position update ────────────────────────────────────────────────────────
static void updatePosition() {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (gpsSimulated) return;
    if (gps.location.isValid() && gps.location.age() < 5000) {
        boatLat  = (float)gps.location.lat();
        boatLon  = (float)gps.location.lng();
        gpsValid = true;
    }
}

// ── Waypoint geometry update ───────────────────────────────────────────────
// Refreshes wp_dist_m / wp_bearing every loop for telemetry, and trips the
// sticky `captured` flag the first time we cross the capture radius.
static void updateWaypointGeometry() {
    if (!wpSet || !gpsValid) {
        wpDistM   = 0.0f;
        wpBearing = 0.0f;
        return;
    }
    wpDistM   = haversineDistM(boatLat, boatLon, wpLat, wpLon);
    wpBearing = haversineBearing(boatLat, boatLon, wpLat, wpLon);
    if (!captured && wpDistM < CAPTURE_RADIUS_M) {
        captured = true;
        Serial.printf("[WP] captured (dist=%.1f m). Outputs neutral.\n", wpDistM);
    }
}

// ── WiFi ────────────────────────────────────────────────────────────────────
static bool tryConnect(const char* ssid, const char* pass, int timeoutSecs) {
    WiFi.begin(ssid, pass);
    Serial.printf("[WiFi] Trying %s", ssid);
    for (int i = 0; i < timeoutSecs * 2; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            boatIP = WiFi.localIP().toString();
            Serial.printf(" OK  %s\n", boatIP.c_str());
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(" FAIL");
    WiFi.disconnect(true);
    delay(100);
    return false;
}

static void wifiSetup() {
    WiFi.mode(WIFI_STA);
    if (tryConnect(SECRET_HOME_SSID,    SECRET_HOME_PASS,    10)) return;
    if (tryConnect(SECRET_HOTSPOT_SSID, SECRET_HOTSPOT_PASS, 10)) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("LegendCutter", "coastguard");
    boatIP = WiFi.softAPIP().toString();
    Serial.printf("[WiFi] AP fallback  IP: %s\n", boatIP.c_str());
}

// ── HTTP handlers ──────────────────────────────────────────────────────────
static void addCORS() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
static void handleOptions() { addCORS(); server.send(204); }

static void handleStatus() {
    addCORS();
    server.send(200, "application/json",
        "{\"ok\":true,\"v\":\"test_29\",\"ip\":\"" + boatIP + "\"}");
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<1024> doc;
    doc["v"]            = "test_29";
    doc["uptime"]       = millis() / 1000;
    doc["heap"]         = ESP.getFreeHeap();
    doc["mode"]         = modeName(mode);
    doc["cruise_us"]    = cruiseUs;
    doc["failsafe_ack"] = failsafeAckRequired;
    doc["rc_ever_good"] = ibusEverGood;
    doc["rc_age_ms"]    = ibusEverGood ? (millis() - lastFrameMs) : 0;
    doc["rudder_us"]    = outRudder;
    doc["esc_us"]       = outPort;
    doc["ch_throttle"]  = ch[IBUS_IDX_THROTTLE];
    doc["ch_rudder"]    = ch[IBUS_IDX_RUDDER];
    doc["ch_mode"]      = ch[IBUS_IDX_MODE];
    doc["ch_guard"]     = ch[IBUS_IDX_FAILSAFE_GUARD];
    doc["nav_on"]       = navOn;
    doc["bridge_on"]    = bridgeOn;
    doc["deck_on"]      = deckOn;
    doc["audio_ok"]     = audioOK;
    doc["bilge_fwd"]    = bilgeFwdWet;
    doc["bilge_mid"]    = bilgeMidWet;
    doc["bilge_rear"]   = bilgeRearWet;
    doc["pump"]         = pumpOn;
    doc["pump_manual"]  = pumpManual;
    doc["pump_stuck"]   = bilgeStuckLatch;
    doc["radar_on"]       = radarOn;
    doc["radar_speed"]    = radarSpeed;
    doc["radar_burst_ms"] = radarBurstMs;
    doc["radar_pause_ms"] = radarPauseMs;

    doc["depth_mode"] = (depthMode == DEPTH_RUN) ? "run" : "off";
    if (lastDepthM >= 0.0f) {
        char dbuf[12];
        snprintf(dbuf, sizeof(dbuf), "%.2f", lastDepthM);
        doc["depth_m"] = dbuf;
    }
    if (lastDepthReadMs > 0) {
        doc["depth_age_ms"] = millis() - lastDepthReadMs;
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", fusedHeading); doc["heading"] = buf;
    if (ina219OK) {
        snprintf(buf, sizeof(buf), "%.2f", busVoltage);     doc["batt_v"] = buf;
        snprintf(buf, sizeof(buf), "%.2f", shuntMa / 1000.0f); doc["batt_a"] = buf;
    }

    // Position
    doc["gps_fix"]       = gpsValid;
    doc["gps_simulated"] = gpsSimulated;
    if (gpsValid) {
        snprintf(buf, sizeof(buf), "%.6f", boatLat); doc["lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", boatLon); doc["lon"] = buf;
    }
    if (!gpsSimulated) {
        // Real GPS metadata only meaningful when we're using real GPS.
        doc["sats"]      = (int)gps.satellites.value();
        if (gps.speed.isValid())  { snprintf(buf, sizeof(buf), "%.1f", gps.speed.knots());  doc["speed_kts"] = buf; }
        if (gps.course.isValid()) { snprintf(buf, sizeof(buf), "%.1f", gps.course.deg());   doc["course"]    = buf; }
    }

    // Waypoint
    doc["wp_set"]   = wpSet;
    doc["captured"] = captured;
    if (wpSet) {
        snprintf(buf, sizeof(buf), "%.6f", wpLat);     doc["wp_lat"]     = buf;
        snprintf(buf, sizeof(buf), "%.6f", wpLon);     doc["wp_lon"]     = buf;
        if (gpsValid) {
            snprintf(buf, sizeof(buf), "%.1f", wpDistM);   doc["wp_dist_m"]  = buf;
            snprintf(buf, sizeof(buf), "%.1f", wpBearing); doc["wp_bearing"] = buf;
        }
    }

    // PID (current live values)
    snprintf(buf, sizeof(buf), "%.2f", livePidKp); doc["pid_kp"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", livePidKd); doc["pid_kd"] = buf;

    char out[1024];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleCruise() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }

    uint16_t newUs = 0;
    if (req.containsKey("us")) {
        int us = req["us"].as<int>();
        if (us < NEUTRAL_US || us > MAX_FWD_US) {
            server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"us out of [1500..1800]\"}");
            return;
        }
        newUs = (uint16_t)us;
    } else if (req.containsKey("pct")) {
        float pct = req["pct"].as<float>();
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        newUs = (uint16_t)(NEUTRAL_US + (MAX_FWD_US - NEUTRAL_US) * (pct / 100.0f));
    } else {
        server.send(400, "text/plain", "Need 'us' or 'pct'");
        return;
    }

    cruiseUs = newUs;
    Serial.printf("[CRUISE] → %u µs%s\n", cruiseUs,
        (cruiseUs > AUTO_CRUISE_CAP_US) ? "  (above cap, will clamp on engage)" : "");

    StaticJsonDocument<128> resp;
    resp["ok"]        = true;
    resp["cruise_us"] = cruiseUs;
    resp["cap"]       = AUTO_CRUISE_CAP_US;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleWaypoint() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }
    // {"lat":null,"lon":null} → clear waypoint and reset capture latch.
    if (req["lat"].isNull() || req["lon"].isNull()) {
        wpSet     = false;
        captured  = false;
        wpDistM   = 0.0f;
        wpBearing = 0.0f;
        Serial.println("[WP] cleared");
        server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
        return;
    }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    wpLat    = req["lat"].as<float>();
    wpLon    = req["lon"].as<float>();
    wpSet    = true;
    captured = false;       // a new waypoint always reopens the run
    Serial.printf("[WP] set lat=%.6f lon=%.6f\n", wpLat, wpLon);

    StaticJsonDocument<128> resp;
    resp["ok"]  = true;
    resp["lat"] = wpLat;
    resp["lon"] = wpLon;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handlePid() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }
    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
    if (req.containsKey("kp")) {
        float kp = req["kp"].as<float>();
        if (kp < 0.0f || kp > 20.0f) {
            server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"kp out of [0..20]\"}");
            return;
        }
        livePidKp = kp;
    }
    if (req.containsKey("kd")) {
        float kd = req["kd"].as<float>();
        if (kd < 0.0f || kd > 30.0f) {
            server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"kd out of [0..30]\"}");
            return;
        }
        livePidKd = kd;
    }
    Serial.printf("[PID] kp=%.2f kd=%.2f\n", livePidKp, livePidKd);

    StaticJsonDocument<128> resp;
    resp["ok"] = true;
    resp["kp"] = livePidKp;
    resp["kd"] = livePidKd;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleLed() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    const char* light = req["light"] | "";
    bool        on    = req["state"]  | false;
    uint8_t     pin   = 0;
    bool*       state = nullptr;

    if      (strcmp(light, "nav")    == 0) { pin = PIN_NAV;    state = &navOn;    }
    else if (strcmp(light, "bridge") == 0) { pin = PIN_BRIDGE; state = &bridgeOn; }
    else if (strcmp(light, "deck")   == 0) { pin = PIN_DECK;   state = &deckOn;   }
    else { server.send(400, "text/plain", "Unknown light"); return; }

    *state = on;
    digitalWrite(pin, on ? HIGH : LOW);
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleDepth() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<96> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    const char* m = req["mode"] | "";
    if (strcmp(m, "stop") == 0) {
        depthMode       = DEPTH_OFF;
        lastDepthM      = -1.0f;
        lastDepthReadMs = 0;
        Serial.println("[DEPTH] stop (cleared)");
    } else if (strcmp(m, "check") == 0) {
        // One-shot: take a reading now, leave mode alone (defaults to OFF
        // after a "stop"; stays in RUN if currently running).
        doDepthPing();
    } else if (strcmp(m, "run") == 0) {
        depthMode = DEPTH_RUN;
        doDepthPing();                           // immediate first reading
        Serial.println("[DEPTH] run (20s interval)");
    } else {
        server.send(400, "application/json",
            "{\"ok\":false,\"err\":\"mode must be stop|check|run\"}");
        return;
    }

    StaticJsonDocument<128> resp;
    resp["ok"]   = true;
    resp["mode"] = (depthMode == DEPTH_RUN) ? "run" : "off";
    if (lastDepthM >= 0.0f) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.2f", lastDepthM);
        resp["depth_m"] = buf;
    }
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleRadar() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<192> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    // All fields optional; only update what was sent.
    if (req.containsKey("speed")) {
        int s = req["speed"].as<int>();
        if (s < 0)   s = 0;
        if (s > 100) s = 100;
        radarSpeed = (uint8_t)s;
    }
    if (req.containsKey("burst_ms")) {
        long b = req["burst_ms"].as<long>();
        if (b < (long)RADAR_BURST_MS_MIN) b = RADAR_BURST_MS_MIN;
        if (b > (long)RADAR_BURST_MS_MAX) b = RADAR_BURST_MS_MAX;
        radarBurstMs = (uint32_t)b;
    }
    if (req.containsKey("pause_ms")) {
        long p = req["pause_ms"].as<long>();
        if (p < (long)RADAR_PAUSE_MS_MIN) p = RADAR_PAUSE_MS_MIN;
        if (p > (long)RADAR_PAUSE_MS_MAX) p = RADAR_PAUSE_MS_MAX;
        radarPauseMs = (uint32_t)p;
    }
    if (req.containsKey("on")) {
        radarOn = req["on"].as<bool>();
    }
    applyRadarOutput();
    Serial.printf("[RADAR] %s @ %u%% burst=%lums pause=%lums\n",
                  radarOn ? "ON" : "off", radarSpeed,
                  radarBurstMs, radarPauseMs);

    StaticJsonDocument<160> resp;
    resp["ok"]       = true;
    resp["on"]       = radarOn;
    resp["speed"]    = radarSpeed;
    resp["burst_ms"] = radarBurstMs;
    resp["pause_ms"] = radarPauseMs;
    char out[160];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleAudio() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (!audioOK) {
        server.send(503, "application/json",
            "{\"ok\":false,\"err\":\"DF1201S not initialised\"}");
        return;
    }

    // Body is accepted but ignored for now — all 3 app buttons play track 1.
    DF1201S.playFileNum(DFP_TRACK);
    Serial.printf("[AUDIO] play track %d\n", DFP_TRACK);

    StaticJsonDocument<64> resp;
    resp["ok"]    = true;
    resp["track"] = DFP_TRACK;
    char out[64];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleBilge() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    bool on = req["on"] | false;
    if (on) {
        pumpManual    = true;
        manualUntilMs = millis() + BILGE_MANUAL_TIMEOUT_MS;
        Serial.printf("[BILGE] manual pump ON (auto-clears in %lu ms)\n", BILGE_MANUAL_TIMEOUT_MS);
    } else {
        pumpManual = false;
        Serial.println("[BILGE] manual pump cleared");
    }

    StaticJsonDocument<128> resp;
    resp["ok"]            = true;
    resp["pump_manual"]   = pumpManual;
    resp["timeout_ms"]    = BILGE_MANUAL_TIMEOUT_MS;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

static void handleSimGps() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    StaticJsonDocument<128> req;
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    boatLat      = req["lat"].as<float>();
    boatLon      = req["lon"].as<float>();
    gpsValid     = true;
    gpsSimulated = true;
    Serial.printf("[SIM_GPS] lat=%.6f lon=%.6f\n", boatLat, boatLon);

    StaticJsonDocument<128> resp;
    resp["ok"]            = true;
    resp["lat"]           = boatLat;
    resp["lon"]           = boatLon;
    resp["gps_simulated"] = gpsSimulated;
    char out[128];
    serializeJson(resp, out);
    server.send(200, "application/json", out);
}

// ── Mode state machine (test_27 shape, no cruise floor refusal) ────────────
static void updateMode() {
    Mode prev = mode;

    if (!ibusEverGood) {
        mode = MODE_MANUAL;
        guardAboveSinceMs = 0;
        return;
    }

    if (ch[IBUS_IDX_FAILSAFE_GUARD] > FAILSAFE_GUARD_THRESHOLD) {
        if (guardAboveSinceMs == 0) guardAboveSinceMs = millis();
        if (millis() - guardAboveSinceMs >= FAILSAFE_DETECT_MS) {
            if (mode != MODE_FAILSAFE) {
                mode = MODE_FAILSAFE;
                failsafeAckRequired = true;
                ackRefusalPrinted   = false;
                Serial.printf("\n[FAILSAFE] guard tripped — ch[5]=%u sustained %lu ms. "
                              "Outputs neutral. Flip SwA UP (MANUAL) to ACK.\n",
                              ch[IBUS_IDX_FAILSAFE_GUARD],
                              millis() - guardAboveSinceMs);
            }
            return;
        }
    } else {
        guardAboveSinceMs = 0;
    }

    if (millis() - lastFrameMs >= FAILSAFE_NO_FRAME_MS) {
        if (mode != MODE_FAILSAFE) {
            mode = MODE_FAILSAFE;
            failsafeAckRequired = true;
            ackRefusalPrinted   = false;
            Serial.printf("\n[FAILSAFE] no frames for %lu ms. "
                          "Outputs neutral. Flip SwA UP (MANUAL) to ACK after RC returns.\n",
                          millis() - lastFrameMs);
        }
        return;
    }

    if (failsafeAckRequired) {
        if (!ackRefusalPrinted) {
            Serial.println("[FAILSAFE] frames restored but ACK_REQUIRED — "
                           "flip SwA UP (MANUAL) to clear.");
            ackRefusalPrinted = true;
        }
        if (swcInManual()) {
            failsafeAckRequired = false;
            ackRefusalPrinted   = false;
            mode = MODE_MANUAL;
            Serial.println("[FAILSAFE] cleared (ACK via SwA=MANUAL). mode=MANUAL");
        }
        return;
    }

    Mode next = mode;
    if (swcInManual())     next = MODE_MANUAL;
    else if (swcInAuto())  next = MODE_AUTO;
    // SwA dead-band → keep current mode (hysteresis).
    mode = next;

    if (mode != prev) {
        if (mode == MODE_AUTO) {
            uint16_t engageUs = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
            if (wpSet && gpsValid && !captured) {
                Serial.printf("[MODE] %s → AUTO (cruise=%u µs, wp=%.6f,%.6f, dist=%.1f m)\n",
                    modeName(prev), engageUs, wpLat, wpLon, wpDistM);
            } else if (wpSet && captured) {
                Serial.printf("[MODE] %s → AUTO (waypoint already captured — outputs neutral; "
                              "POST a new /waypoint to re-arm)\n", modeName(prev));
            } else if (wpSet && !gpsValid) {
                Serial.printf("[MODE] %s → AUTO (NO GPS FIX — outputs neutral until fix)\n",
                              modeName(prev));
            } else {
                Serial.printf("[MODE] %s → AUTO (no waypoint — outputs neutral; "
                              "POST /waypoint to arm)\n", modeName(prev));
            }
        } else {
            Serial.printf("[MODE] %s → %s\n", modeName(prev), modeName(mode));
        }
    }
}

// ── Apply outputs ──────────────────────────────────────────────────────────
static void applyOutputs() {
    if (!ibusEverGood) {
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        return;
    }
    switch (mode) {
      case MODE_MANUAL:
        setRudder(mapRudderStickToServo(ch[IBUS_IDX_RUDDER]));
        setEscs  (mapThrottleStickToEsc(ch[IBUS_IDX_THROTTLE]));
        break;
      case MODE_AUTO: {
        if (wpSet && gpsValid && !captured) {
            uint16_t engageUs = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
            setEscs(engageUs);
            setRudder(headingHoldUs(wpBearing));
        } else {
            // No waypoint, no GPS, or already captured → safe-hold.
            setRudder(NEUTRAL_US);
            setEscs(NEUTRAL_US);
        }
        break;
      }
      case MODE_FAILSAFE:
        setRudder(NEUTRAL_US);
        setEscs(NEUTRAL_US);
        break;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("test_29_pool_integration");

    pinMode(PIN_NAV,    OUTPUT); digitalWrite(PIN_NAV,    LOW);
    pinMode(PIN_BRIDGE, OUTPUT); digitalWrite(PIN_BRIDGE, LOW);
    pinMode(PIN_DECK,   OUTPUT); digitalWrite(PIN_DECK,   LOW);

    pinMode(PIN_BILGE_FWD_SENSOR,  INPUT_PULLUP);
    pinMode(PIN_BILGE_MID_SENSOR,  INPUT_PULLUP);
    pinMode(PIN_BILGE_REAR_SENSOR, INPUT_PULLUP);
    pinMode(PIN_BILGE_PUMP,  OUTPUT); digitalWrite(PIN_BILGE_PUMP,  LOW);

    // Radar motor: LEDC PWM, off at boot. Core 3.x API.
    ledcAttach(PIN_RADAR_MOTOR, RADAR_PWM_FREQ_HZ, RADAR_PWM_RESOLUTION);
    ledcWrite(PIN_RADAR_MOTOR, 0);

    // Depth sonar (RCWL-1655). PULLDOWN on ECHO helps distinguish a
    // floating pin from a real idle-low signal during debug (per
    // test_04 NOTES).
    pinMode(PIN_SONAR_TRIG, OUTPUT); digitalWrite(PIN_SONAR_TRIG, LOW);
    pinMode(PIN_SONAR_ECHO, INPUT_PULLDOWN);

    Wire.begin(21, 22);
    Wire.setClock(400000);

    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    Wire.beginTransmission(0x40);
    bool pcaOK = (Wire.endTransmission() == 0);

    bool imuOK = false;
    for (int i = 0; i < 3; i++) {
        myICM.begin(Wire, 0);
        if (myICM.status == ICM_20948_Stat_Ok) { imuOK = true; break; }
        delay(500);
    }
    if (!pcaOK || !imuOK) {
        if (!pcaOK) Serial.println("FAIL: PCA9685 not at 0x40");
        if (!imuOK) Serial.println("FAIL: ICM-20948 not at 0x68");
        while (true) delay(1000);
    }

    ina219OK = ina219.begin();
    if (ina219OK) Serial.printf("[I2C] INA219 detected at 0x%02X\n", INA219_ADDR);
    else          Serial.printf("[I2C] WARN: INA219 not at 0x%02X — voltage telemetry disabled\n",
                                INA219_ADDR);

    setRudder(NEUTRAL_US); setEscs(NEUTRAL_US);
    Serial.println("Arming ESCs (3 s @ 1500 µs)...");
    delay(3000);

    ibusSerial.setRxBufferSize(1024);
    ibusSerial.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
    Serial.printf("iBUS on GPIO%d, mode=CH%d (idx %d).\n",
        IBUS_RX_PIN, IBUS_IDX_MODE + 1, IBUS_IDX_MODE);

    gpsSerial.begin(GPS_BAUD);
    Serial.printf("GPS on SoftwareSerial RX=GPIO%d TX=GPIO%d @ %lu baud\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // DF1201S audio on HardwareSerial(2) — test_11's proven path. If
    // begin() fails (wiring/power issue), /audio returns 503 but
    // autopilot, GPS, iBUS keep running.
    dfSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
    delay(1000);
    {
        uint32_t startMs = millis();
        bool ok = false;
        while (!(ok = DF1201S.begin(dfSerial))) {
            if (millis() - startMs > 3000) break;
            delay(250);
        }
        if (ok) {
            DF1201S.setVol(DFP_VOLUME);
            DF1201S.switchFunction(DF1201S.MUSIC);
            delay(2000);
            DF1201S.setPlayMode(DF1201S.SINGLE);
            DF1201S.enableAMP();
            audioOK = true;
            Serial.printf("[AUDIO] DF1201S ready (vol=%u, track=%d).\n", DFP_VOLUME, DFP_TRACK);
        } else {
            Serial.println("[AUDIO] WARN: DF1201S did not ACK within 3 s — /audio disabled.");
        }
    }

    wifiSetup();

    server.on("/status",    HTTP_GET,     handleStatus);
    server.on("/status",    HTTP_OPTIONS, handleOptions);
    server.on("/telemetry", HTTP_GET,     handleTelemetry);
    server.on("/telemetry", HTTP_OPTIONS, handleOptions);
    server.on("/cruise",    HTTP_POST,    handleCruise);
    server.on("/cruise",    HTTP_OPTIONS, handleOptions);
    server.on("/waypoint",  HTTP_POST,    handleWaypoint);
    server.on("/waypoint",  HTTP_OPTIONS, handleOptions);
    server.on("/pid",       HTTP_POST,    handlePid);
    server.on("/pid",       HTTP_OPTIONS, handleOptions);
    server.on("/sim_gps",   HTTP_POST,    handleSimGps);
    server.on("/sim_gps",   HTTP_OPTIONS, handleOptions);
    server.on("/led",       HTTP_POST,    handleLed);
    server.on("/led",       HTTP_OPTIONS, handleOptions);
    server.on("/audio",     HTTP_POST,    handleAudio);
    server.on("/audio",     HTTP_OPTIONS, handleOptions);
    server.on("/bilge",     HTTP_POST,    handleBilge);
    server.on("/bilge",     HTTP_OPTIONS, handleOptions);
    server.on("/radar",     HTTP_POST,    handleRadar);
    server.on("/radar",     HTTP_OPTIONS, handleOptions);
    server.on("/depth",     HTTP_POST,    handleDepth);
    server.on("/depth",     HTTP_OPTIONS, handleOptions);
    server.begin();
    Serial.printf("HTTP up at http://%s/\n", boatIP.c_str());
    Serial.printf("Default cruise=%u µs (cap %u). Default PID kp=%.2f kd=%.2f. Capture=%.1f m.\n",
        DEFAULT_CRUISE_US, AUTO_CRUISE_CAP_US, DEFAULT_KP, DEFAULT_KD, CAPTURE_RADIUS_M);
    Serial.println("Ready.");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    readIbus();
    updateImu();
    readIbus();
    pollIna219();
    pollBilge();
    updateRadarBurst();
    pollDepth();
    updatePosition();
    server.handleClient();

    updateMode();
    updateWaypointGeometry();
    applyOutputs();
}
