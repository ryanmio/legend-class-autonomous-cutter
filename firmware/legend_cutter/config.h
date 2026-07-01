// config.h
// Central pin definitions, PCA9685 channel assignments, and tuning constants
// for the Legend Class Autonomous Cutter firmware.
//
// Source of truth: tests/test_29_pool_integration (the PASS'd
// test_29-pool2.6-magcal2 build). Every value here is copied from that
// sketch's inline constants — when hardware changes, edit this file; never
// scatter magic numbers into module files.

#pragma once

#include <stdint.h>

// ── Build identification ───────────────────────────────────────────────────
#define FIRMWARE_VERSION "0.7.4"
#define VESSEL_NAME      "Legend Cutter"

// ── I2C ────────────────────────────────────────────────────────────────────
static const uint8_t  I2C_SDA_PIN = 21;
static const uint8_t  I2C_SCL_PIN = 22;
static const uint32_t I2C_FREQ_HZ = 400000;
// Bound the bus so a stuck slave can never hang the control loop. ESP32's Wire
// already defaults to a timeout; this pins it explicitly (hygiene — not the
// cause of the v0.5.x stalls, which were network-send blocking).
static const uint16_t I2C_TIMEOUT_MS = 25;

static const uint8_t PCA9685_ADDR  = 0x40;
static const uint8_t INA219_ADDR   = 0x41;
static const uint8_t ICM20948_ADDR = 0x68;

// ── iBUS receiver (Flysky FS-iA10B) ────────────────────────────────────────
static const uint8_t  IBUS_RX_PIN = 16;
static const uint32_t IBUS_BAUD   = 115200;

// Channel indices (0-based). Final map locked 2026-05-10.
//   CH1 idx 0  rudder        (right-stick H)
//   CH2 idx 1  reverse       (right-stick V, down=reverse)
//   CH3 idx 2  throttle      (left-stick V)
//   CH5 idx 4  knob          (deck-gun pan, test_18)
//   CH6 idx 5  failsafe guard (SwD; up≈1000 µs nominal, ≥1500 µs = TX gone)
//   CH7 idx 6  mode switch   (SwA; up≈1000 µs=MANUAL, down≈2000 µs=AUTO)
static const uint8_t IBUS_IDX_RUDDER         = 0;
static const uint8_t IBUS_IDX_REVERSE        = 1;
static const uint8_t IBUS_IDX_THROTTLE       = 2;
static const uint8_t IBUS_IDX_GUN_PAN        = 4;
static const uint8_t IBUS_IDX_FAILSAFE_GUARD = 5;
static const uint8_t IBUS_IDX_MODE           = 6;
// Reverse-interlock deadband (test_17 pattern). Right-stick V below
// (NEUTRAL_US - REVERSE_DEADBAND_US) AND throttle at idle → reverse engages.
static const uint16_t REVERSE_DEADBAND_US     = 30;

// ── PCA9685 channel assignments ────────────────────────────────────────────
static const uint8_t CH_ESC_PORT = 0;
static const uint8_t CH_ESC_STBD = 1;
static const uint8_t CH_RUDDER   = 2;
static const uint8_t CH_GUN_PAN  = 8;   // positional 9g micro (test_18 PASS 2026-05-04)

static const uint16_t PCA9685_FREQ_HZ = 50;

// ── Output bounds (µs) ─────────────────────────────────────────────────────
static const uint16_t RUDDER_MIN_US = 1330;
static const uint16_t RUDDER_MAX_US = 1670;
static const uint16_t NEUTRAL_US    = 1500;
static const uint16_t MAX_FWD_US    = 1800;
// AUTO never asks for reverse; MIN_REV_US widens the setEscs floor so MANUAL
// reverse (right-stick V down past deadband) can reach the ESCs.
static const uint16_t MIN_REV_US    = 1200;

// ESCs wired such that stick-forward turned props in reverse on pool run #1.
// Mirror at the PCA write boundary so upstream math keeps "MAX_FWD_US = forward fast".
static const bool ESC_DIRECTION_INVERTED = true;

// AUTO cruise selection (no floor; cap only). cruise=NEUTRAL_US is valid for
// static heading-hold. Cap prevents an in-water hands-off runaway.
static const uint16_t AUTO_CRUISE_CAP_US = 1800;   // matches MAX_FWD_US
static const uint16_t DEFAULT_CRUISE_US  = 1720;   // fallback past the ESC deadband
static const float    DIFF_THRUST_FACTOR = 0.3f;

// Stick safety.
static const uint16_t THROTTLE_IDLE_MAX = 1100;

// SwA hysteresis.
static const uint16_t MODE_MAN_BELOW_US  = 1450;
static const uint16_t MODE_AUTO_ABOVE_US = 1550;

// Failsafe thresholds.
static const uint16_t FAILSAFE_GUARD_THRESHOLD = 1500;
static const uint32_t FAILSAFE_DETECT_MS       = 500;
static const uint32_t FAILSAFE_NO_FRAME_MS     = 3000;

// ── GPS (BN-220) ───────────────────────────────────────────────────────────
// Wire colors on this batch are reversed from convention: white=TX, green=RX.
//   white (BN-220 TX) → GPIO 17 (ESP32 RX)
//   green (BN-220 RX) → GPIO 4  (ESP32 TX, optional)
// SoftwareSerial is used: UART2 is owned by the DF1201S audio module which
// needs 115200 (SoftwareSerial @ 115200 + WiFi is unreliable, test_11).
static const uint8_t  GPS_RX_PIN = 17;
static const uint8_t  GPS_TX_PIN = 4;
static const uint32_t GPS_BAUD   = 9600;

// ── IMU (ICM-20948) + heading hold ─────────────────────────────────────────
static const uint32_t IMU_UPDATE_INTERVAL_MS = 20;
// Default heading-hold gains; live-tunable via POST /pid.
static const float    DEFAULT_KP   = 1.5f;
static const float    DEFAULT_KD   = 8.0f;
// AUTO steering smoothing: deadband suppresses hunting near setpoint;
// slew caps how fast the rudder servo can move (µs/s, 0→full in ~2 s).
static const float    AUTO_HEADING_DEADBAND_DEG  = 5.0f;
static const float    AUTO_RUDDER_SLEW_US_PER_S  = 85.0f;
// Cap a single slew timestep so one stalled control tick can't slam the rudder;
// 0.5 s → at most a 25%-throw step. Never zero dt (that pinned the rudder).
static const float    AUTO_SLEW_DT_CAP_S         = 0.5f;
// AUTO near-center damping via DECOUPLED differential thrust. The rudder's
// deadband zeroes its PD output near center, so the motors carry the through-
// crossing damping that kills the weave — keeping the fragile rudder parked.
// Driven by the raw (non-deadbanded) PD command; µs split = gain × PD command.
// Start the gain conservative: too-weak shows as residual weave with the split
// NOT at clamp (raise gain); the split clamps below the low-side prop-bite floor
// and within forward headroom, symmetric so average thrust (cruise) is held.
static const float    AUTO_DIFF_GAIN          = 0.25f;
static const uint16_t AUTO_DIFF_MAX_SPLIT_US  = 80;     // conservative authority cap
static const uint16_t AUTO_DIFF_LOW_FLOOR_US  = 1560;   // low motor stays in clean forward thrust
// Default mag offsets — used only as the fallback when NVS holds no cal, so
// flashing produces zero behavior change pre-cal. Real cal via
// /calibrate_mag/start overwrites these and saves to NVS.
static const float    DEFAULT_MAG_OFFSET_X = -20.70f;
static const float    DEFAULT_MAG_OFFSET_Y =  -0.45f;
static const float    DEFAULT_MAG_OFFSET_Z = -17.70f;
static const float    IMU_FILTER_ALPHA = 0.98f;

// Local geomagnetic facts (WMM, Chesapeake Bay area). Horizontal field
// strength is the cal-quality yardstick; declination converts the mag
// heading to true so it matches GPS bearings.
static const float    MAG_DECLINATION_DEG     = -11.0f;  // 11° W: true = magnetic − 11°
static const float    EXPECTED_HORIZ_FIELD_UT = 20.5f;

// GPS course-over-ground heading trim. While driving straight and forward
// fast enough for COG to be meaningful, a slow servo loop absorbs whatever
// heading error remains after cal + declination.
static const float    COG_TRIM_MIN_KTS      = 1.0f;
static const float    COG_TRIM_MAX_TURN_DPS = 10.0f;
static const float    COG_TRIM_GAIN         = 0.05f;   // per 1 Hz update ≈ 20 s time constant
static const float    COG_TRIM_CLAMP_DEG    = 30.0f;
static const uint32_t COG_TRIM_INTERVAL_MS  = 1000;
// Persist the learned trim so the first AUTO of a run starts from the last
// run's value instead of 0 (the bias is fixed-boat, constant trip to trip).
// Throttled to spare NVS: only on meaningful drift, and not too often.
static const float    COG_TRIM_SAVE_MIN_DELTA_DEG = 0.5f;
static const uint32_t COG_TRIM_SAVE_INTERVAL_MS   = 30000;

// Onboard mag calibration. Coverage + range gates run on chip Y/Z (the
// horizontal pair); chip X (vertical) is excluded by design.
static const uint32_t MAG_CAL_MIN_MS          = 10000;
static const uint32_t MAG_CAL_TIMEOUT_MS      = 90000;
static const float    MAG_CAL_MIN_RANGE       = 20.0f;  // µT, per horizontal axis
static const int      MAG_CAL_SECTORS         = 12;
static const float    MAG_CAL_SPIKE_UT        = 5.0f;   // per-axis sample-to-sample reject
static const float    MAG_CAL_CENTER_SHIFT_UT = 3.0f;   // re-bin coverage if center drifts

// ── Battery (INA219) ───────────────────────────────────────────────────────
static const uint32_t INA_POLL_INTERVAL_MS = 250;

// ── Depth sonar (RCWL-1655) ────────────────────────────────────────────────
// RCWL-1655 — NOT a JSN-SR04T. Confirmed bench-good 2026-03-15 in test_04.
// Sound speed: 13.4 µs/cm freshwater round-trip. AIR is ~4.3× slower, so
// bench tests show distances ~4.3× LARGER than reality.
static const uint8_t  PIN_SONAR_TRIG        = 27;
static const uint8_t  PIN_SONAR_ECHO        = 14;
static const float    SONAR_US_PER_CM       = 13.4f;
static const uint32_t DEPTH_PING_TIMEOUT_US = 40000;
static const uint32_t DEPTH_RUN_INTERVAL_MS = 20000;
static const float    DEPTH_MIN_VALID_M     = 1.00f;  // below = near-field floor; report no-echo not false-shallow

// ── Bilge ──────────────────────────────────────────────────────────────────
// 3 active-LOW water probes + 1 active-HIGH pump MOSFET. Pump lives in the
// REAR compartment (moved 2026-05-27) — only the rear sensor drives the
// pump; fwd/mid are informational.
//   GPIO 5 is a strapping pin but only governs SDIO-slave boot mode (unused);
//   safe as a sensor input for normal flash boot.
// Pump can clear the rear compartment in ~5 s, so we duty-cycle:
//   BILGE_PULSE_ON_MS ON, then BILGE_PULSE_OFF_MS pause, repeat. Auto mode
//   gives up after BILGE_AUTO_MAX_MS total elapsed if the rear probe is still
//   wet (operator must then engage manually; manual cycles forever).
static const uint8_t  PIN_BILGE_FWD_SENSOR    = 32;
static const uint8_t  PIN_BILGE_MID_SENSOR    = 33;
static const uint8_t  PIN_BILGE_REAR_SENSOR   = 5;
static const uint8_t  PIN_BILGE_PUMP          = 13;
static const uint32_t BILGE_PULSE_ON_MS       = 6000;
static const uint32_t BILGE_PULSE_OFF_MS      = 6000;
static const uint32_t BILGE_AUTO_MAX_MS       = 60000;

// ── Radar (mast dish, 2N2222 low-side PWM on GPIO 2) ───────────────────────
// 1 kHz instead of "above audible": at 20 kHz the coil L/R doesn't let
// current build per pulse — low duty produces no torque. 1 kHz is audible
// as a whine but produces useful low-speed torque.
// Add a 1N4148/1N4001 flyback diode across the motor before sustained use.
static const uint8_t  PIN_RADAR_MOTOR        = 2;
static const uint32_t RADAR_PWM_FREQ_HZ      = 1000;
static const uint8_t  RADAR_PWM_RESOLUTION   = 8;
static const uint8_t  RADAR_DEFAULT_SPEED    = 25;
static const uint32_t RADAR_BURST_MS_DEFAULT = 3;
static const uint32_t RADAR_PAUSE_MS_DEFAULT = 200;
static const uint32_t RADAR_BURST_MS_MIN     = 2;
static const uint32_t RADAR_BURST_MS_MAX     = 5000;
static const uint32_t RADAR_PAUSE_MS_MIN     = 0;
static const uint32_t RADAR_PAUSE_MS_MAX     = 60000;

// ── LED light circuits ─────────────────────────────────────────────────────
static const uint8_t PIN_NAV    = 18;
static const uint8_t PIN_BRIDGE = 19;
static const uint8_t PIN_DECK   = 23;

// ── DF1201S audio (HardwareSerial2) ────────────────────────────────────────
// AT-protocol DFPlayer Pro at 115200. NOT the DFPlayer Mini. Index-based
// playback only — path-based has known firmware bugs on this module.
// Indices reflect FAT write order; AppleDouble files from macOS occupy the
// even slots until a clean `xattr -cr` wipe (then odd-only becomes 1/2/3).
static const uint8_t  DFP_RX_PIN      = 25;
static const uint8_t  DFP_TX_PIN      = 26;
static const uint32_t DFP_BAUD        = 115200;
static const uint8_t  DFP_VOLUME      = 20;
static const int16_t  DFP_HORN_INDEX  = 1;
static const int16_t  DFP_GUN_INDEX   = 3;
static const int16_t  DFP_BOARD_INDEX = 5;

// ── Deck gun pan (test_18 PASS 2026-05-04) ─────────────────────────────────
// Positional 9g micro on PCA ch8 (swapped from FS90R continuous 2026-05-04).
// Knob CH5 → servo angle directly; PAN_REVERSE compensates this build's
// linkage direction. Center deadband filters TX jitter; holds 1500 on RC loss.
static const uint16_t GUN_PAN_MIN_US      = 1000;
static const uint16_t GUN_PAN_MAX_US      = 2000;
static const uint16_t GUN_PAN_DEADBAND_US = 15;
static const bool     GUN_PAN_REVERSE     = true;

// ── Navigation / waypoint ──────────────────────────────────────────────────
// Max waypoints in one mission (matches the test_28 sequencer oracle). A single
// waypoint is just a 1-point mission. 32 * 8 B = 256 B RAM.
static const uint8_t MAX_WAYPOINTS = 32;
static const float CAPTURE_RADIUS_M = 5.0f;
// Inside this range the steering setpoint latches to the approach heading and
// stops chasing the (now hypersensitive) instantaneous bearing — kills the
// close-in circling. Must stay > CAPTURE_RADIUS_M.
static const float AUTO_APPROACH_LOCK_M = 10.0f;
// Fat-finger guard: refuse / auto-clear waypoints farther than this from the boat.
static const float MAX_WP_DIST_M    = 1000.0f;

// ── HTTP / WiFi ────────────────────────────────────────────────────────────
// Home WiFi first (bench), iPhone hotspot second (water). No AP fallback by
// design — if neither connects, autopilot + RC failsafe still work; HTTP/
// telemetry are simply absent until the network returns.
static const uint16_t HTTP_PORT = 80;

// ── Concurrency: network task + command queue ──────────────────────────────
// All networking (WebServer + WiFi maintain) runs in a task pinned to core 0
// (the WiFi/lwIP core); the Arduino loop() — RC, FSM, failsafe, outputs — stays
// on core 1 and calls ZERO network functions, so a blocked socket send can
// never stall control. Operator commands cross core 0 → core 1 through a single
// lock-free single-producer/single-consumer queue (cmd.h); telemetry crosses
// core 1 → core 0 as lock-free, expendable reads.
static const uint16_t NET_TASK_STACK = 8192;   // bytes: WebServer + ArduinoJson + /history String
static const uint8_t  NET_TASK_CORE  = 0;      // WiFi/lwIP core; loop() runs on core 1
static const uint8_t  CMD_QUEUE_LEN   = 8;     // SPSC ring depth (commands are human-paced)

// ── Onboard telemetry history (store-and-sync) ─────────────────────────────
// RAM ring buffer of compact per-second records, recorded continuously
// regardless of WiFi state. On reconnect the app pulls the gap via
// GET /history?since_ms=<uptimeMs> so the flight log stays unbroken across a
// WiFi dropout. RAM-only by design: a reboot clears it, but the app already
// treats a reboot (session_id change) as a flight boundary, so nothing the
// system keeps today is lost. CAPACITY records × ~48 B ≈ the static footprint,
// which shares dram0 with the WiFi stack's working memory — so this can't grow
// freely: ~1200 is the safe ceiling. 1500+ links but starves the heap and WiFi
// gets flaky at runtime; 2400 overflows the build outright. Leave at 1200.
//   1200 @ 1 Hz ≈ 20 min (~57 KB static) — covers any realistic single dropout,
//   since the app backfills on each reconnect (the ring only spans one gap).
static const uint16_t HISTLOG_CAPACITY    = 1200;
static const uint32_t HISTLOG_INTERVAL_MS = 1000;
// Max records returned per /history response; the app pages with since_ms.
static const uint16_t HISTLOG_PAGE_MAX    = 100;
