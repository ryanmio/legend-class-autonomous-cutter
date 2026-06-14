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
 *   NEW      Capture detection (sticky), live /pid tuning.
 *
 * What's NEW vs test_27:
 *   - POST /waypoint {lat,lon} (and {lat:null,lon:null} to clear) — single
 *     waypoint controls AUTO. App's MapScreen already wires this.
 *   - Capture detection: when wp_dist < 3 m, sticky `captured` flag fires;
 *     ESCs + rudder forced neutral until operator clears or moves the
 *     waypoint. Equivalent to "mission complete" for a single-leg mission.
 *   - POST /pid {kp, kd} for live heading-hold tuning during the run. No
 *     Ki yet — we want to see how P+D behaves in water before adding I.
 *   - AUTO no longer requires cruise above any floor. Cruise=1500 (neutral)
 *     is now valid, so the static-heading-hold scenario in AUTOPILOT_PLAN
 *     test_32 step 2 ("AUTO at cruise=neutral, rudder tracks waypoint")
 *     just works. Cap is AUTO_CRUISE_CAP_US (see SAFETY below).
 *   - AUTO without a waypoint = neutral. Removes test_27's "hold heading
 *     at entry" placeholder; from here, AUTO means "drive to a waypoint."
 *   - Telemetry adds: gps_fix, lat, lon, sats, speed_kts,
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
 *   AUTO cruise cap = AUTO_CRUISE_CAP_US = 1800 µs, equal to the hard ESC
 *   clamp MAX_FWD_US = 1800 in setEscs(). The earlier 1700/1710 trim was
 *   removed 2026-06-05 ("uncap") after the harbor run showed no forward
 *   motion at the trimmed level — AUTO may now command full trimmed
 *   forward. Operator controls actual speed via /cruise.
 *   ESC outputs are inverted via ESC_DIRECTION_INVERTED at the PCA
 *   write boundary so MAX_FWD_US still means "forward fast" upstream.
 *   Captured boat freezes at neutral until operator intervenes.
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
#include <Preferences.h>
#include <math.h>
#include <esp_system.h>
#include "secrets.h"

// ── PCA9685 channels ────────────────────────────────────────────────────────
static const uint8_t  CH_ESC_PORT = 0;
static const uint8_t  CH_ESC_STBD = 1;
static const uint8_t  CH_RUDDER   = 2;

// ── Output bounds ───────────────────────────────────────────────────────────
// Trimmed 2026-05-30 after pool run #1: prior 60% cap (1800/1200) was
// problematically fast even at half stick (bow rose above the deck).
static const uint16_t RUDDER_MIN_US = 1330;
static const uint16_t RUDDER_MAX_US = 1670;
static const uint16_t NEUTRAL_US    = 1500;
static const uint16_t MAX_FWD_US    = 1800;   // pool #1 level — restoring after harbor weak-forward
// Reverse cap. AUTO never asks for reverse (cruise is always ≥ NEUTRAL_US);
// the setEscs() floor is widened to this only so MANUAL reverse can
// reach the ESCs.
static const uint16_t MIN_REV_US    = 1200;   // pool #1 level — symmetric with MAX_FWD_US

// ESCs wired such that stick-forward turned props in reverse on pool run #1.
// Mirror at the PCA write boundary so MAX_FWD_US still semantically means
// "forward fast" everywhere upstream (AUTO, diff thrust, computeThrottle).
static const bool ESC_DIRECTION_INVERTED = true;

// ── AUTO cruise selection (no floor; cap only) ─────────────────────────────
// cruise=NEUTRAL_US is valid for static heading-hold (AUTOPILOT_PLAN
// test_32 step 2). The cap prevents an in-water hands-off runaway.
static const uint16_t AUTO_CRUISE_CAP_US = 1800;  // matches MAX_FWD_US so app can dial cruise high
static const uint16_t DEFAULT_CRUISE_US  = 1720;  // fallback past the ESC deadband
static const float    DIFF_THRUST_FACTOR = 0.3f;

// ── iBUS channel indices (locked 2026-05-10) ────────────────────────────────
static const uint8_t  IBUS_IDX_RUDDER         = 0;
static const uint8_t  IBUS_IDX_REVERSE        = 1;   // CH2 right-stick V, down = reverse
static const uint8_t  IBUS_IDX_THROTTLE       = 2;
static const uint8_t  IBUS_IDX_FAILSAFE_GUARD = 5;
static const uint8_t  IBUS_IDX_MODE           = 6;
static const uint8_t  IBUS_RX_PIN             = 16;
// Reverse-interlock deadband (test_17 pattern). Right-stick V below
// (1500 - REVERSE_DEADBAND_US) AND throttle stick at idle → reverse
// engages. Throttle stick above idle → reverse blocked regardless.
static const uint16_t REVERSE_DEADBAND_US     = 30;

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
// Default mag offsets used until/unless NVS contains a fresh cal.
// Provenance unknown; preserved as fallback so flashing this firmware
// produces zero behaviour change pre-cal. Real cal via /calibrate_mag/start.
static const float    DEFAULT_MAG_OFFSET_X = -20.70f;
static const float    DEFAULT_MAG_OFFSET_Y =  -0.45f;
static const float    DEFAULT_MAG_OFFSET_Z = -17.70f;
static const float    ALPHA = 0.98f;

// Local geomagnetic facts (WMM, Chesapeake Bay area). Horizontal field
// strength is the cal-quality yardstick; declination converts the mag
// heading to true so it matches GPS bearings.
static const float    MAG_DECLINATION_DEG     = -11.0f;  // 11° W: true = magnetic − 11°
static const float    EXPECTED_HORIZ_FIELD_UT = 20.5f;

// GPS course-over-ground heading trim. While driving straight and
// forward fast enough for COG to be meaningful, a slow servo loop
// absorbs whatever heading error remains after cal + declination.
static const float    COG_TRIM_MIN_KTS      = 1.0f;
static const float    COG_TRIM_MAX_TURN_DPS = 10.0f;
static const float    COG_TRIM_GAIN         = 0.05f;   // per 1 Hz update ≈ 20 s time constant
static const float    COG_TRIM_CLAMP_DEG    = 30.0f;
static const uint32_t COG_TRIM_INTERVAL_MS  = 1000;

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
// Pump lives in the REAR compartment (moved 2026-05-27) — only the rear
// probe drives the pump; fwd/mid are informational.
// Sensor probes: active LOW (internal pullup → defined HIGH=dry idle).
// Pump is active HIGH via the MOSFET on GPIO 13.
//   GPIO 5  is a strapping pin but only governs SDIO-slave boot mode
//           (unused). Safe as a sensor input for normal flash boot.
// Pump can clear the rear in ~5 s, so we duty-cycle:
//   PULSE_ON ms ON, then PULSE_OFF ms pause, repeat. AUTO mode gives up
//   after AUTO_MAX_MS total elapsed if the rear probe is still wet — at
//   that point the operator must engage manually. Manual cycles forever
//   until the operator stops it.
static const uint8_t PIN_BILGE_FWD_SENSOR  = 32;
static const uint8_t PIN_BILGE_MID_SENSOR  = 33;
static const uint8_t PIN_BILGE_REAR_SENSOR = 5;
static const uint8_t PIN_BILGE_PUMP        = 13;

static const uint32_t BILGE_PULSE_ON_MS  = 6000;
static const uint32_t BILGE_PULSE_OFF_MS = 6000;
static const uint32_t BILGE_AUTO_MAX_MS  = 60000;
// Declared up here (not in the state block below) so the Arduino IDE's
// auto-generated forward declarations at the top of the file can resolve
// these types when pollBilge / bilgeEnterPhase / handleBilge reference them.
enum BilgePhase  { BILGE_PHASE_OFF = 0, BILGE_PHASE_ON = 1, BILGE_PHASE_PAUSE = 2 };
enum BilgeSource { BILGE_SRC_NONE = 0, BILGE_SRC_AUTO = 1, BILGE_SRC_MANUAL = 2 };

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
// Index-based playback via playFileNum (AT+PLAYNUM). Path-based
// (AT+PLAYFILE) has known firmware bugs on this module — DFRobot Issue
// #5 documents the chip silently falling back to "file 1" on any path
// mis-resolution, which is exactly what bit us on the first water test.
// Indices reflect FAT write order, which is fixed deterministically by
// audio-assets/dfplayer/load.sh — keep this map and that script's
// TRACKS list in sync.
// Indices reflect the current device state — odd-only because macOS
// Finder put AppleDouble (._HORN.MP3 etc) companions in the even
// slots when the files were dragged on. After a clean wipe + reload
// with xattr -cr (see audio-assets/dfplayer/NOTES.md) these become
// 1/2/3. NOTES.md has the diagnostic snippet for verifying.
static const int16_t  DFP_HORN_INDEX  = 1;   // HORN.MP3
static const int16_t  DFP_GUN_INDEX   = 3;   // GUN.MP3  (index 2 = ._HORN.MP3)
static const int16_t  DFP_BOARD_INDEX = 5;   // BOARD.MP3 (index 4 = ._GUN.MP3)

// ── Capture ─────────────────────────────────────────────────────────────────
static const float    CAPTURE_RADIUS_M = 3.0f;
// Fat-finger guard: refuse waypoints farther than this from the boat.
static const float    MAX_WP_DIST_M    = 1000.0f;

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
static uint32_t                 sessionId = 0;  // hardware-random per boot; app uses to detect mid-flight reboots

// ── Mode state machine ─────────────────────────────────────────────────────
enum Mode { MODE_MANUAL, MODE_AUTO, MODE_FAILSAFE };
static Mode    mode                = MODE_MANUAL;
static bool    failsafeAckRequired = false;
static bool    ackRefusalPrinted   = false;
static uint16_t cruiseUs           = DEFAULT_CRUISE_US;

// ── Live PID gains ──────────────────────────────────────────────────────────
static float   livePidKp = DEFAULT_KP;
static float   livePidKd = DEFAULT_KD;

// ── Mag calibration (NVS-backed, runtime mutable) ──────────────────────────
// Loaded from NVS namespace "imu_cal" at boot. If NVS is empty, falls back
// to DEFAULT_MAG_OFFSET_{X,Y,Z} so this firmware behaves identically to
// pre-cal-feature firmware on first flash. /calibrate_mag/start triggers an
// onboard rotation-coverage cal that overwrites these and saves.
enum MagCalState { MAG_CAL_IDLE, MAG_CAL_COLLECTING, MAG_CAL_DONE, MAG_CAL_FAILED };
static MagCalState magCalState = MAG_CAL_IDLE;
static const char* magCalStateName(MagCalState s) {
    return s == MAG_CAL_IDLE       ? "idle"
         : s == MAG_CAL_COLLECTING ? "collecting"
         : s == MAG_CAL_DONE       ? "done"
                                   : "failed";
}
static float    magOffX = DEFAULT_MAG_OFFSET_X;
static float    magOffY = DEFAULT_MAG_OFFSET_Y;
static float    magOffZ = DEFAULT_MAG_OFFSET_Z;
static float    magBaselineUT = 0.0f;   // 0 = no cal recorded yet
static uint32_t magCalTs = 0;
static bool     magFromNVS = false;     // true if we loaded a real cal at boot

// Cal-in-progress scratch (only valid during MAG_CAL_COLLECTING)
static float    calMinX, calMinY, calMinZ, calMaxX, calMaxY, calMaxZ;
static int      calSampleCnt = 0;
static unsigned long calStartMs = 0;
// Rotation coverage: the horizontal field vector (chip Y/Z plane — chip X
// is vertical) is binned into 30° sectors around the running circle
// center. The cal finishes when every sector has been visited, so
// progress reflects actual rotation, not elapsed time.
static uint16_t calSectorMask = 0;
static float    calBinCenterY = 0.0f, calBinCenterZ = 0.0f;
static bool     calHavePrev = false;
static float    calPrevX = 0.0f, calPrevY = 0.0f, calPrevZ = 0.0f;
static char     magCalFailBuf[96] = "";
static const char* magCalFailReason = "";
// Cal quality results (persisted to NVS alongside the offsets)
static float    magCalRadiusUT = 0.0f;  // mean horizontal circle radius from the spin
static float    magCalCircPct  = 0.0f;  // Y-vs-Z radius mismatch (soft-iron indicator)
enum MagCalQuality { MAG_QUAL_UNKNOWN = 0, MAG_QUAL_GOOD, MAG_QUAL_FAIR, MAG_QUAL_POOR };
static uint8_t  magCalQuality  = MAG_QUAL_UNKNOWN;
static const char* magCalQualityName(uint8_t q) {
    return q == MAG_QUAL_GOOD ? "good"
         : q == MAG_QUAL_FAIR ? "fair"
         : q == MAG_QUAL_POOR ? "poor" : "unknown";
}
// Cached live mag magnitude (post-offset) — emitted in /telemetry for the
// app's drift health check.
static float    liveMagUT = 0.0f;

// Cal tuning. Coverage and range gates run on chip Y and Z — the
// horizontal pair. Chip X (vertical) is excluded by design.
static const unsigned long MAG_CAL_MIN_MS          = 10000;
static const unsigned long MAG_CAL_TIMEOUT_MS      = 90000;
static const float         MAG_CAL_MIN_RANGE       = 20.0f;  // µT, per horizontal axis
static const int           MAG_CAL_SECTORS         = 12;
static const float         MAG_CAL_SPIKE_UT        = 5.0f;   // per-axis sample-to-sample reject
static const float         MAG_CAL_CENTER_SHIFT_UT = 3.0f;   // re-bin coverage if center drifts

static Preferences         magPrefs;

// ── Waypoint state ─────────────────────────────────────────────────────────
static bool    wpSet         = false;
static float   wpLat         = 0.0f;
static float   wpLon         = 0.0f;
static float   wpDistM       = 0.0f;
static float   wpBearing     = 0.0f;
// `captured` is sticky once EITHER the distance trigger (wp_dist <
// CAPTURE_RADIUS_M) OR the perpendicular-crossing trigger fires.
// Cleared when the operator POSTs a new /waypoint or clears it.
// Prevents the boat from re-engaging cruise after hovering across.
static bool    captured      = false;

// Crossing-trigger state: the boat's position is recorded when AUTO
// engages (first fix after engage) and treated as the "start" of the
// leg. The capture fires if the boat passes the perpendicular line
// through the waypoint (perpendicular to start→waypoint). This
// prevents endless circling when GPS noise (~2-3 m) is close to the
// capture radius (3 m). Capture detection only runs in AUTO — driving
// past the waypoint manually must not mark the leg complete.
static bool    startValid    = false;
static float   startLat      = 0.0f;
static float   startLon      = 0.0f;
// Which trigger fired the most-recent capture. Surfaced in telemetry
// so the CSV log can replay which mechanism ended each leg — pool
// has no serial console.
enum CapturedBy { CAPTURED_BY_NONE = 0, CAPTURED_BY_DISTANCE, CAPTURED_BY_CROSSING };
static CapturedBy capturedBy = CAPTURED_BY_NONE;
// 1 degree of latitude ≈ 111,111 m anywhere on earth. Longitude scales
// by cos(lat). Flat-earth approximation is plenty accurate over the
// few-meter scale of a pool waypoint leg.
static const float METERS_PER_DEG_LAT = 111111.0f;
static const float MIN_LEG_M2         = 1.0f;   // skip crossing check on legs < 1 m

// ── Position state ──────────────────────────────────────────────────────────
static float   boatLat       = 0.0f;
static float   boatLon       = 0.0f;
static bool    gpsValid      = false;

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
// BilgePhase / BilgeSource enums are declared up with the bilge constants
// so the IDE's auto-generated forward declarations can resolve them.
static bool     bilgeFwdWet = false, bilgeMidWet = false, bilgeRearWet = false;
static BilgePhase  bilgePhase    = BILGE_PHASE_OFF;
static BilgeSource bilgeSource   = BILGE_SRC_NONE;
static uint8_t     bilgeCycle    = 0;
static uint32_t    bilgePhaseStartMs    = 0;
static uint32_t    bilgeSequenceStartMs = 0;
// Set true when AUTO cycles for AUTO_MAX_MS and rear is still wet —
// auto won't re-engage until rear actually reads dry once. Manual /bilge
// {on:true} clears the latch.
static bool     bilgeStuckLatch = false;

// ── Audio state ─────────────────────────────────────────────────────────────
// DF1201S on HardwareSerial(2) @ 115200 — test_11's proven path.
static HardwareSerial   dfSerial(2);
static DFRobot_DF1201S  DF1201S;
static bool             audioOK = false;

// ── IMU / heading hold state ────────────────────────────────────────────────
static float    fusedHeading    = 0;
static float    prevHeadingForD = 0;
// Turn rate (deg/s) measured once per IMU update so the dt always matches
// the heading-update cadence. Sampling this in headingHoldUs() at loop
// rate (~1-5 ms) produced spiky garbage: dH was 0 on most loops, then a
// full 20 ms heading step divided by a 2 ms loop dt — ~10x inflated.
static float    headingRateDps  = 0.0f;
static unsigned long lastImuUs  = 0;
static bool     headingInit     = false;
// GPS-COG heading trim: residual correction learned while underway.
// Volatile by design — resets each boot and after recal; declination
// covers the bulk, this absorbs what's left of the cal error.
static float    cogTrimDeg      = 0.0f;
static uint32_t lastCogTrimMs   = 0;

// ── INA219 cache ────────────────────────────────────────────────────────────
static float    busVoltage = 0.0f;
static float    shuntMa    = 0.0f;
static uint32_t lastInaPollMs = 0;
static const uint32_t INA_POLL_INTERVAL_MS = 250;

// ── Helpers ────────────────────────────────────────────────────────────────
static uint16_t usTicks(uint16_t us) { return (uint16_t)((us / 20000.0f) * 4096); }
static void writePCA(uint8_t c, uint16_t us) { pca.setPWM(c, 0, usTicks(us)); }
// Mirror at PCA boundary when ESCs are wired backwards. Clamps stay on the
// un-mirrored value so MAX_FWD_US / MIN_REV_US still mean what they say.
static uint16_t escUs(uint16_t us) {
    return ESC_DIRECTION_INVERTED ? (uint16_t)(3000 - us) : us;
}

static void setRudder(uint16_t us) {
    if (us < RUDDER_MIN_US) us = RUDDER_MIN_US;
    if (us > RUDDER_MAX_US) us = RUDDER_MAX_US;
    outRudder = us;
    writePCA(CH_RUDDER, us);
}
static void setEscs(uint16_t us) {
    // Floor widened to MIN_REV_US so MANUAL reverse can reach the ESCs.
    // Callers in AUTO / FAILSAFE must pass values ≥ NEUTRAL_US (they do).
    if (us < MIN_REV_US) us = MIN_REV_US;
    if (us > MAX_FWD_US) us = MAX_FWD_US;
    outPort = outStbd = us;
    writePCA(CH_ESC_PORT, escUs(us));
    writePCA(CH_ESC_STBD, escUs(us));
}
static void setEscsPortStbd(uint16_t portUs, uint16_t stbdUs) {
    if (portUs < MIN_REV_US) portUs = MIN_REV_US;
    if (portUs > MAX_FWD_US) portUs = MAX_FWD_US;
    if (stbdUs < MIN_REV_US) stbdUs = MIN_REV_US;
    if (stbdUs > MAX_FWD_US) stbdUs = MAX_FWD_US;
    outPort = portUs; outStbd = stbdUs;
    writePCA(CH_ESC_PORT, escUs(portUs));
    writePCA(CH_ESC_STBD, escUs(stbdUs));
}
static void computePortStbd(uint16_t throttleUs, uint16_t rudderUs,
                             uint16_t &portUs, uint16_t &stbdUs) {
    float rudderDelta = (float)((int)rudderUs - 1500) / 500.0f;
    float diffUs = rudderDelta * DIFF_THRUST_FACTOR * ((int)throttleUs - 1500);
    int port = (int)throttleUs + (int)diffUs;
    int stbd = (int)throttleUs - (int)diffUs;
    if (port < MIN_REV_US) port = MIN_REV_US;
    if (port > MAX_FWD_US) port = MAX_FWD_US;
    if (stbd < MIN_REV_US) stbd = MIN_REV_US;
    if (stbd > MAX_FWD_US) stbd = MAX_FWD_US;
    portUs = (uint16_t)port;
    stbdUs = (uint16_t)stbd;
}

// Reverse-interlock pattern from test_17: forward throttle (left stick
// above idle) ALWAYS wins. Reverse (right-stick V down past deadband)
// only engages when the throttle stick is at idle. This prevents
// slamming forward→reverse instantly.
static bool throttleAtIdle(uint16_t leftStickUs) {
    return leftStickUs <= THROTTLE_IDLE_MAX;
}
static bool rightStickCommandingReverse(uint16_t rightStickVUs) {
    return rightStickVUs < (uint16_t)(NEUTRAL_US - REVERSE_DEADBAND_US);
}
static uint16_t computeThrottleUs(uint16_t leftStickUs, uint16_t rightStickVUs) {
    if (!throttleAtIdle(leftStickUs)) {
        // Forward: scale [THROTTLE_IDLE_MAX..2000] → [NEUTRAL_US..MAX_FWD_US].
        if (leftStickUs >= 2000) return MAX_FWD_US;
        return (uint16_t)map(leftStickUs, THROTTLE_IDLE_MAX, 2000, NEUTRAL_US, MAX_FWD_US);
    }
    // Throttle idle — right stick V controls reverse (interlock satisfied).
    if (rightStickCommandingReverse(rightStickVUs)) {
        uint16_t rev = rightStickVUs;
        if (rev < MIN_REV_US) rev = MIN_REV_US;
        if (rev > NEUTRAL_US) rev = NEUTRAL_US;
        return rev;
    }
    return NEUTRAL_US;
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

// ── Mag calibration (NVS-backed, app-triggered) ────────────────────────────
static bool magCalibratedFlag() {
    return magBaselineUT > 0.0f
        && (magOffX != 0.0f || magOffY != 0.0f || magOffZ != 0.0f);
}

static void magCalLoadFromNVS() {
    magPrefs.begin("imu_cal", true);
    bool present = magPrefs.isKey("off_x") && magPrefs.isKey("base_uT");
    if (present) {
        magOffX       = magPrefs.getFloat("off_x", DEFAULT_MAG_OFFSET_X);
        magOffY       = magPrefs.getFloat("off_y", DEFAULT_MAG_OFFSET_Y);
        magOffZ       = magPrefs.getFloat("off_z", DEFAULT_MAG_OFFSET_Z);
        magBaselineUT = magPrefs.getFloat("base_uT", 0.0f);
        magCalTs      = magPrefs.getUInt ("cal_ts", 0);
        magCalRadiusUT = magPrefs.getFloat("rad_uT", 0.0f);
        magCalCircPct  = magPrefs.getFloat("circ_pct", 0.0f);
        magCalQuality  = (uint8_t)magPrefs.getUChar("quality", MAG_QUAL_UNKNOWN);
        magFromNVS = true;
    }
    magPrefs.end();
    Serial.printf("[mag-cal] %s  off=(%.2f, %.2f, %.2f)  base=%.2f uT\n",
                  magFromNVS ? "loaded from NVS" : "NVS empty, using hardcoded defaults",
                  magOffX, magOffY, magOffZ, magBaselineUT);
}

static void magCalSaveToNVS() {
    magPrefs.begin("imu_cal", false);
    magPrefs.putFloat("off_x",   magOffX);
    magPrefs.putFloat("off_y",   magOffY);
    magPrefs.putFloat("off_z",   magOffZ);
    magPrefs.putFloat("base_uT", magBaselineUT);
    magPrefs.putUInt ("cal_ts",  magCalTs);
    magPrefs.putFloat("rad_uT",   magCalRadiusUT);
    magPrefs.putFloat("circ_pct", magCalCircPct);
    magPrefs.putUChar("quality",  magCalQuality);
    magPrefs.end();
    magFromNVS = true;
    Serial.printf("[mag-cal] saved: off=(%.2f, %.2f, %.2f) base=%.2f uT\n",
                  magOffX, magOffY, magOffZ, magBaselineUT);
}

static void magCalBegin() {
    calMinX =  1e9f; calMinY =  1e9f; calMinZ =  1e9f;
    calMaxX = -1e9f; calMaxY = -1e9f; calMaxZ = -1e9f;
    calSampleCnt = 0;
    calSectorMask = 0;
    calBinCenterY = 0.0f; calBinCenterZ = 0.0f;
    calHavePrev = false;
    calStartMs = millis();
    magCalFailReason = "";
    magCalState = MAG_CAL_COLLECTING;
    Serial.println("[mag-cal] START — rotate boat through a full 360°");
}

static void magCalFinishSuccess() {
    // Chip X is vertical: a flat spin can't separate its offset from
    // earth's vertical field, so the X center absorbs both. Heading on
    // level water only uses Y/Z; the X term matters only under tilt.
    magOffX = (calMinX + calMaxX) / 2.0f;
    magOffY = (calMinY + calMaxY) / 2.0f;
    magOffZ = (calMinZ + calMaxZ) / 2.0f;

    float radY = (calMaxY - calMinY) * 0.5f;
    float radZ = (calMaxZ - calMinZ) * 0.5f;
    magCalRadiusUT = (radY + radZ) * 0.5f;
    magCalCircPct  = (magCalRadiusUT > 0.1f)
                   ? fabsf(radY - radZ) / magCalRadiusUT * 100.0f : 100.0f;
    float radErr = fabsf(magCalRadiusUT - EXPECTED_HORIZ_FIELD_UT) / EXPECTED_HORIZ_FIELD_UT;
    magCalQuality = (radErr < 0.30f && magCalCircPct < 20.0f) ? MAG_QUAL_GOOD
                  : (radErr < 0.50f && magCalCircPct < 35.0f) ? MAG_QUAL_FAIR
                                                              : MAG_QUAL_POOR;
    // Level-water |B| should sit near the circle radius at every heading —
    // that's the health yardstick the app and MC gates check against.
    magBaselineUT = magCalRadiusUT;
    magCalTs = millis() / 1000;
    magCalSaveToNVS();
    headingInit = false;   // re-seed the fused heading from the new offsets
    cogTrimDeg  = 0.0f;    // learned residual belongs to the old mag frame
    magCalState = MAG_CAL_DONE;
    Serial.printf("[mag-cal] DONE  samples=%d  radius=%.1f µT (expect ~%.1f)  circ=%.0f%%  quality=%s\n",
                  calSampleCnt, magCalRadiusUT, EXPECTED_HORIZ_FIELD_UT,
                  magCalCircPct, magCalQualityName(magCalQuality));
}

static void magCalFinishFail(const char* why) {
    magCalFailReason = why;
    magCalState = MAG_CAL_FAILED;
    Serial.printf("[mag-cal] FAILED: %s\n", why);
}

static int magCalSectorCount() {
    int n = 0;
    for (int i = 0; i < MAG_CAL_SECTORS; i++)
        if (calSectorMask & (1u << i)) n++;
    return n;
}

// Called from updateImu() every IMU sample. Cheap when state==idle/done.
static void magCalTick(float rawX, float rawY, float rawZ) {
    if (magCalState != MAG_CAL_COLLECTING) return;

    // Spike filter: a slow spin moves the field well under 1 µT per 20 ms
    // sample, so a bigger jump is interference, not rotation.
    if (calHavePrev &&
        (fabsf(rawX - calPrevX) > MAG_CAL_SPIKE_UT ||
         fabsf(rawY - calPrevY) > MAG_CAL_SPIKE_UT ||
         fabsf(rawZ - calPrevZ) > MAG_CAL_SPIKE_UT)) {
        calPrevX = rawX; calPrevY = rawY; calPrevZ = rawZ;
        return;
    }
    calPrevX = rawX; calPrevY = rawY; calPrevZ = rawZ;
    calHavePrev = true;

    if (rawX < calMinX) calMinX = rawX;  if (rawX > calMaxX) calMaxX = rawX;
    if (rawY < calMinY) calMinY = rawY;  if (rawY > calMaxY) calMaxY = rawY;
    if (rawZ < calMinZ) calMinZ = rawZ;  if (rawZ > calMaxZ) calMaxZ = rawZ;
    calSampleCnt++;

    // Sector coverage on the horizontal (chip Y/Z) circle. Binning waits
    // until both ranges are credible enough to estimate a center; if the
    // center estimate later drifts, coverage restarts — another turn
    // refills it in seconds.
    float rangeY = calMaxY - calMinY, rangeZ = calMaxZ - calMinZ;
    if (rangeY > MAG_CAL_MIN_RANGE * 0.5f && rangeZ > MAG_CAL_MIN_RANGE * 0.5f) {
        float cy = (calMinY + calMaxY) * 0.5f;
        float cz = (calMinZ + calMaxZ) * 0.5f;
        if (fabsf(cy - calBinCenterY) > MAG_CAL_CENTER_SHIFT_UT ||
            fabsf(cz - calBinCenterZ) > MAG_CAL_CENTER_SHIFT_UT) {
            calSectorMask = 0;
            calBinCenterY = cy;
            calBinCenterZ = cz;
        }
        float ang = atan2f(rawZ - cz, rawY - cy);          // -π..π
        int sector = (int)((ang + PI) / (2.0f * PI) * MAG_CAL_SECTORS);
        if (sector < 0) sector = 0;
        if (sector >= MAG_CAL_SECTORS) sector = MAG_CAL_SECTORS - 1;
        calSectorMask |= (1u << sector);
    }

    unsigned long elapsed = millis() - calStartMs;
    if (elapsed > MAG_CAL_TIMEOUT_MS) {
        if (rangeY < MAG_CAL_MIN_RANGE || rangeZ < MAG_CAL_MIN_RANGE) {
            snprintf(magCalFailBuf, sizeof(magCalFailBuf),
                     "weak signal: horiz range %.0f/%.0f uT (expect ~%.0f) - move away from metal",
                     rangeY, rangeZ, MAG_CAL_MIN_RANGE * 2.0f);
        } else {
            snprintf(magCalFailBuf, sizeof(magCalFailBuf),
                     "incomplete rotation: %d/%d directions covered - keep turning the boat",
                     magCalSectorCount(), MAG_CAL_SECTORS);
        }
        magCalFinishFail(magCalFailBuf);
        return;
    }
    if (elapsed > MAG_CAL_MIN_MS
        && calSectorMask == (uint16_t)((1u << MAG_CAL_SECTORS) - 1)
        && rangeY > MAG_CAL_MIN_RANGE && rangeZ > MAG_CAL_MIN_RANGE) {
        magCalFinishSuccess();
    }
}

// Percent of the 360° rotation actually covered — sectors visited / total.
static int magCalProgressPct() {
    if (magCalState == MAG_CAL_DONE) return 100;
    if (magCalState != MAG_CAL_COLLECTING) return 0;
    return magCalSectorCount() * 100 / MAG_CAL_SECTORS;
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
    float magRawX = myICM.magX();
    float magRawY = myICM.magY();
    float magRawZ = myICM.magZ();
    float mx = magRawX - magOffX;
    float my = magRawY - magOffY;
    float mz = magRawZ - magOffZ;
    liveMagUT = sqrtf(mx*mx + my*my + mz*mz);
    magCalTick(magRawX, magRawY, magRawZ);

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
        if (dt > 0.0f) {
            headingRateDps = shortestPathError(fusedHeading, prevHeadingForD) / dt;
        }
        prevHeadingForD = fusedHeading;
    }
}

// Best estimate of TRUE heading: fused mag/gyro heading + declination +
// the GPS-COG residual trim. This is what navigation steers with and
// what telemetry reports — waypoint bearings are true, so heading must be.
static float navHeadingDeg() {
    float h = fusedHeading + MAG_DECLINATION_DEG + cogTrimDeg;
    while (h <    0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    return h;
}

// Learn the residual heading error from GPS course-over-ground, 1 Hz.
// Gated to moments when COG actually means heading: fix valid, fast
// enough, driving roughly straight, and under forward thrust (COG flips
// 180° in reverse). At rest the trim just holds its last value.
static void updateCogTrim() {
    if (millis() - lastCogTrimMs < COG_TRIM_INTERVAL_MS) return;
    lastCogTrimMs = millis();
    if (!headingInit || !gpsValid) return;
    if (!gps.speed.isValid() || !gps.course.isValid()) return;
    if (gps.speed.knots() < COG_TRIM_MIN_KTS) return;
    if (fabsf(headingRateDps) > COG_TRIM_MAX_TURN_DPS) return;
    if (((outPort + outStbd) / 2) <= NEUTRAL_US + 25) return;
    float err = shortestPathError((float)gps.course.deg(), navHeadingDeg());
    cogTrimDeg += COG_TRIM_GAIN * err;
    if (cogTrimDeg >  COG_TRIM_CLAMP_DEG) cogTrimDeg =  COG_TRIM_CLAMP_DEG;
    if (cogTrimDeg < -COG_TRIM_CLAMP_DEG) cogTrimDeg = -COG_TRIM_CLAMP_DEG;
}

// Heading-hold output. Uses livePidKp / livePidKd so /pid edits take
// effect immediately — no reflash. D-term uses headingRateDps, which is
// computed in updateImu() at the IMU cadence (see note at its decl).
static uint16_t headingHoldUs(float target) {
    if (!headingInit) return NEUTRAL_US;
    float err  = shortestPathError(target, navHeadingDeg());
    float dErr = -headingRateDps;
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

// ── Bilge: duty-cycled pump driven by the REAR probe (only) ────────────────
// Phase transitions:
//   OFF  --(rear wet & !stuck) OR manual-on--> ON  (cycle=1, sequenceStart=now)
//   ON   --PULSE_ON elapsed--> PAUSE
//   PAUSE --PULSE_OFF elapsed:
//             AUTO + still wet + (now-seqStart) < AUTO_MAX_MS --> ON  (cycle++)
//             AUTO + dry                                      --> OFF
//             AUTO + still wet + cap reached                  --> OFF + stuck
//             MANUAL                                          --> ON  (cycle++)
// Operator stop (/bilge {on:false}) → OFF immediately.
// A real dry rear reading clears the stuck latch.
static void writePump(bool on) {
    digitalWrite(PIN_BILGE_PUMP, on ? HIGH : LOW);
}
static void bilgeEnterPhase(BilgePhase p, uint32_t now) {
    bilgePhase = p;
    bilgePhaseStartMs = now;
    writePump(p == BILGE_PHASE_ON);
}
static void bilgeStopAll() {
    bilgePhase  = BILGE_PHASE_OFF;
    bilgeSource = BILGE_SRC_NONE;
    bilgeCycle  = 0;
    writePump(false);
}

static void pollBilge() {
    uint32_t now = millis();
    bilgeFwdWet  = (digitalRead(PIN_BILGE_FWD_SENSOR)  == LOW);
    bilgeMidWet  = (digitalRead(PIN_BILGE_MID_SENSOR)  == LOW);
    bilgeRearWet = (digitalRead(PIN_BILGE_REAR_SENSOR) == LOW);

    if (!bilgeRearWet && bilgeStuckLatch) {
        bilgeStuckLatch = false;
        Serial.println("[BILGE] rear dry — stuck-wet latch cleared, auto-pump re-armed.");
    }

    BilgeSource want;
    if (bilgeSource == BILGE_SRC_MANUAL)        want = BILGE_SRC_MANUAL;
    else if (bilgeRearWet && !bilgeStuckLatch)  want = BILGE_SRC_AUTO;
    else                                        want = BILGE_SRC_NONE;

    if (want == BILGE_SRC_NONE) {
        if (bilgePhase != BILGE_PHASE_OFF) {
            bilgeStopAll();
            Serial.println("[BILGE] sequence ended.");
        }
        return;
    }

    if (bilgeSource != want) {
        bilgeSource          = want;
        bilgeCycle           = 1;
        bilgeSequenceStartMs = now;
        bilgeEnterPhase(BILGE_PHASE_ON, now);
        Serial.printf("[BILGE] %s start — cycle 1 ON.\n",
                      want == BILGE_SRC_MANUAL ? "MANUAL" : "AUTO");
        return;
    }

    uint32_t phaseElapsed = now - bilgePhaseStartMs;
    if (bilgePhase == BILGE_PHASE_ON && phaseElapsed >= BILGE_PULSE_ON_MS) {
        bilgeEnterPhase(BILGE_PHASE_PAUSE, now);
        return;
    }
    if (bilgePhase == BILGE_PHASE_PAUSE && phaseElapsed >= BILGE_PULSE_OFF_MS) {
        if (bilgeSource == BILGE_SRC_AUTO) {
            if (!bilgeRearWet) {
                bilgeStopAll();
                Serial.println("[BILGE] AUTO done — rear dry.");
                return;
            }
            if ((now - bilgeSequenceStartMs) >= BILGE_AUTO_MAX_MS) {
                bilgeStuckLatch = true;
                bilgeStopAll();
                Serial.printf("[BILGE] AUTO cap hit (%lu ms) — rear still wet. "
                              "Latched OFF; operator must engage manually.\n",
                              now - bilgeSequenceStartMs);
                return;
            }
        }
        bilgeCycle++;
        bilgeEnterPhase(BILGE_PHASE_ON, now);
    }
}

// ── Position update ────────────────────────────────────────────────────────
static void updatePosition() {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (gps.location.isValid() && gps.location.age() < 5000) {
        boatLat  = (float)gps.location.lat();
        boatLon  = (float)gps.location.lng();
        gpsValid = true;
    } else if (gpsValid) {
        // Fix went stale (>5 s) or invalid. Clear gpsValid so AUTO's
        // existing !gpsValid branch in applyOutputs() safe-holds at
        // neutral instead of steering on frozen coordinates.
        gpsValid = false;
        Serial.println("[GPS] fix lost (age > 5s) — AUTO will safe-hold until fix returns");
    }
}

// ── Waypoint geometry update ───────────────────────────────────────────────
// Refreshes wp_dist_m / wp_bearing every loop for telemetry, and trips the
// sticky `captured` flag fires on EITHER trigger:
//   (1) distance: wp_dist < CAPTURE_RADIUS_M (existing behavior)
//   (2) crossing: boat passes the perpendicular line at the waypoint,
//       perpendicular to start→waypoint. Prevents endless circling.
//
// Whichever fires first wins. The crossing trigger uses a flat-earth
// projection (boat - start) · (waypoint - start), captured fires when
// the dot product ≥ |waypoint - start|² (i.e. boat is at or past the
// waypoint along the leg axis).
static bool hasCrossedTarget() {
    if (!startValid) return false;
    const float cosLat = cosf(startLat * DEG_TO_RAD);
    const float ax = (wpLon   - startLon) * METERS_PER_DEG_LAT * cosLat;
    const float ay = (wpLat   - startLat) * METERS_PER_DEG_LAT;
    const float legMag2 = ax * ax + ay * ay;
    if (legMag2 < MIN_LEG_M2) return false;   // degenerate leg, skip
    const float px = (boatLon - startLon) * METERS_PER_DEG_LAT * cosLat;
    const float py = (boatLat - startLat) * METERS_PER_DEG_LAT;
    return (px * ax + py * ay) >= legMag2;
}

static void updateWaypointGeometry() {
    if (!wpSet || !gpsValid) {
        wpDistM   = 0.0f;
        wpBearing = 0.0f;
        return;
    }
    wpDistM   = haversineDistM(boatLat, boatLon, wpLat, wpLon);
    wpBearing = haversineBearing(boatLat, boatLon, wpLat, wpLon);

    // Backstop for the fat-finger guard: a waypoint accepted before the
    // first GPS fix gets distance-checked here once position is known.
    if (wpDistM > MAX_WP_DIST_M) {
        Serial.printf("[WP] auto-cleared — %.0f m away (max %.0f)\n", wpDistM, MAX_WP_DIST_M);
        wpSet      = false;
        captured   = false;
        capturedBy = CAPTURED_BY_NONE;
        startValid = false;
        wpDistM    = 0.0f;
        wpBearing  = 0.0f;
        return;
    }

    // Everything below is capture detection — armed only while AUTO is
    // actually driving the leg. MANUAL/FAILSAFE still get live dist/bearing
    // telemetry above, but can't trip `captured`.
    if (mode != MODE_AUTO) return;

    // Record leg-start position on the first GPS fix after AUTO engage.
    if (!startValid) {
        startLat   = boatLat;
        startLon   = boatLon;
        startValid = true;
        Serial.printf("[WP] leg start recorded: %.6f, %.6f → %.6f, %.6f\n",
                      startLat, startLon, wpLat, wpLon);
    }

    if (captured) return;

    if (wpDistM < CAPTURE_RADIUS_M) {
        captured   = true;
        capturedBy = CAPTURED_BY_DISTANCE;
        Serial.printf("[WP] captured by DISTANCE (dist=%.1f m). Outputs neutral.\n", wpDistM);
        return;
    }
    if (hasCrossedTarget()) {
        captured   = true;
        capturedBy = CAPTURED_BY_CROSSING;
        Serial.printf("[WP] captured by CROSSING (dist=%.1f m, missed radius). Outputs neutral.\n", wpDistM);
        return;
    }
}

// ── WiFi ────────────────────────────────────────────────────────────────────
// Scan-first connect (ported from EdmundFitzgeraldController, which works in the
// field). Priority: home SSID exact match → any SSID containing "iPhone"
// (handles curly-vs-straight apostrophe and phone renames) → blind fallback to
// configured hotspot SSID.
//
// BLOCKING — called from setup() only (boat is neutral on the bench then).
// In-water reconnects go through wifiMaintain(), which never blocks: WiFi
// is telemetry-only and AUTO must keep running on RC + GPS alone.
static String lastSsid;
static String lastPass;
static bool   wifiWasConnected = false;

// Boot-time nav-light signal so the operator can read WiFi state across
// the pool: 1 flash when scanning starts, 2 flashes once connected.
// Setup-only (boat neutral on the bench); leaves the LED off afterward.
static void blinkNav(int times) {
    for (int i = 0; i < times; i++) {
        digitalWrite(PIN_NAV, HIGH); delay(120);
        digitalWrite(PIN_NAV, LOW);  delay(120);
    }
}

static void wifiConnect() {
    Serial.println();
    Serial.println("[WiFi] Scanning…");
    blinkNav(1);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    Serial.printf("[WiFi] %d networks found\n", n);

    String ssid;
    String pass;
    bool foundHome = false;

    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        Serial.printf("  [%d] '%s' RSSI=%d\n", i, s.c_str(), WiFi.RSSI(i));
        if (s == SECRET_HOME_SSID) {
            ssid = s; pass = SECRET_HOME_PASS; foundHome = true;
            Serial.printf("[WiFi] matched home: %s\n", s.c_str());
            break;
        }
    }

    if (!foundHome) {
        for (int i = 0; i < n; i++) {
            String s = WiFi.SSID(i);
            if (s.indexOf("iPhone") >= 0 || s == SECRET_HOTSPOT_SSID) {
                ssid = s; pass = SECRET_HOTSPOT_PASS;
                Serial.printf("[WiFi] matched hotspot: %s\n", s.c_str());
                break;
            }
        }
    }

    if (ssid.length() == 0) {
        ssid = SECRET_HOTSPOT_SSID;
        pass = SECRET_HOTSPOT_PASS;
        Serial.printf("[WiFi] no scan match — blind-connecting: %s\n", ssid.c_str());
    }

    lastSsid = ssid;
    lastPass = pass;
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("[WiFi] connecting");
    for (int i = 0; i < 40; i++) {       // 20 s
        if (WiFi.status() == WL_CONNECTED) {
            boatIP = WiFi.localIP().toString();
            wifiWasConnected = true;
            Serial.printf(" OK %s\n", boatIP.c_str());
            blinkNav(2);
            return;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf(" FAIL (status=%d)\n", WiFi.status());
}

static void wifiSetup() { wifiConnect(); }

// Non-blocking reconnect, called every loop. On WiFi loss it re-issues
// WiFi.begin() (returns immediately; radio retries on its own) at most
// once per 30 s using the credentials picked at boot. No scan, no delay —
// outputs, iBUS, and failsafe detection keep running the whole time.
static void wifiMaintain() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            boatIP = WiFi.localIP().toString();
            Serial.printf("[WiFi] reconnected: %s\n", boatIP.c_str());
        }
        return;
    }
    if (wifiWasConnected) {
        wifiWasConnected = false;
        Serial.println("[WiFi] lost — retrying in background; boat unaffected");
    }
    static uint32_t lastAttemptMs = 0;
    if (millis() - lastAttemptMs >= 30000 && lastSsid.length() > 0) {
        lastAttemptMs = millis();
        WiFi.begin(lastSsid.c_str(), lastPass.c_str());
    }
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
        "{\"ok\":true,\"v\":\"test_29-pool2.6-magcal2\",\"ip\":\"" + boatIP + "\"}");
}

static void handleTelemetry() {
    addCORS();
    StaticJsonDocument<2048> doc;
    doc["v"]            = "test_29-pool2.6-magcal2";
    doc["session_id"]   = sessionId;
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
    doc["ch_reverse"]   = ch[IBUS_IDX_REVERSE];
    doc["ch_mode"]      = ch[IBUS_IDX_MODE];
    doc["ch_guard"]     = ch[IBUS_IDX_FAILSAFE_GUARD];
    doc["nav_on"]       = navOn;
    doc["bridge_on"]    = bridgeOn;
    doc["deck_on"]      = deckOn;
    doc["audio_ok"]     = audioOK;
    doc["bilge_fwd"]    = bilgeFwdWet;
    doc["bilge_mid"]    = bilgeMidWet;
    doc["bilge_rear"]   = bilgeRearWet;
    doc["pump"]         = (bilgePhase == BILGE_PHASE_ON);
    doc["pump_manual"]  = (bilgeSource == BILGE_SRC_MANUAL);
    doc["pump_stuck"]   = bilgeStuckLatch;
    doc["pump_phase"]   = (bilgePhase == BILGE_PHASE_ON)    ? "on"
                        : (bilgePhase == BILGE_PHASE_PAUSE) ? "pause"
                        : "off";
    if (bilgePhase != BILGE_PHASE_OFF) {
        doc["pump_cycle"]    = bilgeCycle;
        doc["pump_phase_ms"] = millis() - bilgePhaseStartMs;
    }
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
    // `heading` is the boat's best TRUE heading (mag + declination + COG
    // trim) — directly comparable to `course` and `wp_bearing`.
    snprintf(buf, sizeof(buf), "%.1f", navHeadingDeg()); doc["heading"]     = buf;
    snprintf(buf, sizeof(buf), "%.1f", fusedHeading);    doc["heading_mag"] = buf;
    snprintf(buf, sizeof(buf), "%.1f", cogTrimDeg);      doc["cog_trim"]    = buf;
    if (ina219OK) {
        snprintf(buf, sizeof(buf), "%.2f", busVoltage);     doc["batt_v"] = buf;
        snprintf(buf, sizeof(buf), "%.2f", shuntMa / 1000.0f); doc["batt_a"] = buf;
    }

    // Position
    doc["gps_fix"]       = gpsValid;
    if (gpsValid) {
        snprintf(buf, sizeof(buf), "%.6f", boatLat); doc["lat"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", boatLon); doc["lon"] = buf;
    }
    doc["sats"]      = (int)gps.satellites.value();
    if (gps.speed.isValid())  { snprintf(buf, sizeof(buf), "%.1f", gps.speed.knots());  doc["speed_kts"] = buf; }
    if (gps.course.isValid()) { snprintf(buf, sizeof(buf), "%.1f", gps.course.deg());   doc["course"]    = buf; }

    // Waypoint
    doc["wp_set"]      = wpSet;
    doc["captured"]    = captured;
    doc["captured_by"] = (capturedBy == CAPTURED_BY_DISTANCE) ? "distance"
                       : (capturedBy == CAPTURED_BY_CROSSING) ? "crossing"
                       : "none";
    if (wpSet) {
        snprintf(buf, sizeof(buf), "%.6f", wpLat);     doc["wp_lat"]     = buf;
        snprintf(buf, sizeof(buf), "%.6f", wpLon);     doc["wp_lon"]     = buf;
        if (gpsValid) {
            snprintf(buf, sizeof(buf), "%.1f", wpDistM);   doc["wp_dist_m"]  = buf;
            snprintf(buf, sizeof(buf), "%.1f", wpBearing); doc["wp_bearing"] = buf;
        }
        // Leg start (recorded on first GPS fix after /waypoint). Lets
        // the app draw the leg line + perpendicular capture line at
        // the waypoint for visualisation.
        if (startValid) {
            snprintf(buf, sizeof(buf), "%.6f", startLat); doc["wp_start_lat"] = buf;
            snprintf(buf, sizeof(buf), "%.6f", startLon); doc["wp_start_lon"] = buf;
        }
    }

    // PID (current live values)
    snprintf(buf, sizeof(buf), "%.2f", livePidKp); doc["pid_kp"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", livePidKd); doc["pid_kd"] = buf;

    // ── Mag calibration / health ─────────────────────────────────────────
    doc["mag_cal_state"]    = magCalStateName(magCalState);
    doc["mag_cal_progress"] = magCalProgressPct();
    doc["mag_calibrated"]   = magCalibratedFlag();
    doc["mag_cal_ts"]       = magCalTs;
    doc["mag_from_nvs"]     = magFromNVS;
    if (magCalState == MAG_CAL_COLLECTING) doc["mag_cal_mask"] = calSectorMask;
    if (magCalState == MAG_CAL_FAILED) doc["mag_cal_fail"] = magCalFailReason;
    doc["mag_cal_quality"] = magCalQualityName(magCalQuality);
    if (magCalQuality != MAG_QUAL_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%.1f", magCalRadiusUT); doc["mag_cal_radius_uT"] = buf;
        snprintf(buf, sizeof(buf), "%.0f", magCalCircPct);  doc["mag_cal_circ_pct"]  = buf;
    }
    snprintf(buf, sizeof(buf), "%.2f", magOffX);       doc["mag_off_x"]       = buf;
    snprintf(buf, sizeof(buf), "%.2f", magOffY);       doc["mag_off_y"]       = buf;
    snprintf(buf, sizeof(buf), "%.2f", magOffZ);       doc["mag_off_z"]       = buf;
    snprintf(buf, sizeof(buf), "%.2f", magBaselineUT); doc["mag_baseline_uT"] = buf;
    snprintf(buf, sizeof(buf), "%.2f", liveMagUT);     doc["mag_uT"]          = buf;

    // Warn (over serial, only when near the edge) if the payload ever
    // grows close to the buffer — a truncated message is invalid JSON the
    // app can't parse, which would blind the operator. Quiet in normal use.
    size_t jsonLen = measureJson(doc);
    if (jsonLen > 1800) Serial.printf("[telemetry] WARN json=%u bytes near 2048 buffer\n", (unsigned)jsonLen);

    char out[2048];
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleCalibrateMagStart() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (magCalState == MAG_CAL_COLLECTING) {
        server.send(200, "application/json",
            "{\"ok\":true,\"state\":\"collecting\",\"note\":\"already running\"}");
        return;
    }
    magCalBegin();
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"collecting\"}");
}

static void handleCalibrateMagAbort() {
    addCORS();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    if (server.method() != HTTP_POST)    { server.send(405, "text/plain", "Method Not Allowed"); return; }

    if (magCalState != MAG_CAL_COLLECTING) {
        if (magCalState == MAG_CAL_FAILED) magCalState = MAG_CAL_IDLE;
        server.send(200, "application/json",
            "{\"ok\":true,\"state\":\"idle\",\"note\":\"not collecting\"}");
        return;
    }
    magCalFinishFail("operator aborted");
    magCalState = MAG_CAL_IDLE;
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"idle\"}");
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
            char err[80];
            snprintf(err, sizeof(err),
                "{\"ok\":false,\"err\":\"us out of [%u..%u]\"}",
                NEUTRAL_US, MAX_FWD_US);
            server.send(400, "application/json", err);
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
        wpSet      = false;
        captured   = false;
        capturedBy = CAPTURED_BY_NONE;
        startValid = false;
        wpDistM    = 0.0f;
        wpBearing  = 0.0f;
        Serial.println("[WP] cleared");
        server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
        return;
    }
    if (!req.containsKey("lat") || !req.containsKey("lon")) {
        server.send(400, "text/plain", "Need lat and lon");
        return;
    }
    float newLat = req["lat"].as<float>();
    float newLon = req["lon"].as<float>();
    // Fat-finger guard — only checkable when we know where the boat is.
    // The no-fix case is backstopped in updateWaypointGeometry().
    if (gpsValid) {
        float d = haversineDistM(boatLat, boatLon, newLat, newLon);
        if (d > MAX_WP_DIST_M) {
            Serial.printf("[WP] REJECTED lat=%.6f lon=%.6f — %.0f m away (max %.0f)\n",
                          newLat, newLon, d, MAX_WP_DIST_M);
            char err[96];
            snprintf(err, sizeof(err),
                "{\"ok\":false,\"err\":\"waypoint %.0f m away (max %.0f m)\"}",
                d, MAX_WP_DIST_M);
            server.send(400, "application/json", err);
            return;
        }
    }
    wpLat      = newLat;
    wpLon      = newLon;
    wpSet      = true;
    captured   = false;     // a new waypoint always reopens the run
    capturedBy = CAPTURED_BY_NONE;
    startValid = false;     // re-record leg start on next GPS update
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

    StaticJsonDocument<96> req;
    if (deserializeJson(req, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
    const char* sound = req["sound"] | "";

    StaticJsonDocument<96> resp;
    resp["ok"]    = true;
    resp["sound"] = sound;

    int16_t track = 0;
    if      (!strcmp(sound, "horn"))  track = DFP_HORN_INDEX;
    else if (!strcmp(sound, "gun"))   track = DFP_GUN_INDEX;
    else if (!strcmp(sound, "board")) track = DFP_BOARD_INDEX;

    if (track > 0) {
        // EF commit 54dc3ca: pause before play. Without this, a press
        // that lands while a previous track is still playing gets
        // silently dropped by the chip.
        DF1201S.pause();
        delay(50);
        DF1201S.playFileNum(track);
        resp["track"] = track;
        Serial.printf("[AUDIO] play %s (index %d)\n", sound, track);
    } else {
        server.send(400, "application/json",
            "{\"ok\":false,\"err\":\"unknown sound\"}");
        return;
    }

    char out[96];
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
    uint32_t now = millis();
    if (on) {
        bilgeSource          = BILGE_SRC_MANUAL;
        bilgeCycle           = 1;
        bilgeSequenceStartMs = now;
        bilgeStuckLatch      = false;   // operator override
        bilgeEnterPhase(BILGE_PHASE_ON, now);
        Serial.println("[BILGE] MANUAL start (cycles 6s on / 6s off until stopped).");
    } else {
        bilgeStopAll();
        Serial.println("[BILGE] manual stopped — cycles cleared.");
    }

    StaticJsonDocument<128> resp;
    resp["ok"]          = true;
    resp["pump_manual"] = (bilgeSource == BILGE_SRC_MANUAL);
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
            // Re-record the leg start at engage position — the crossing
            // line must be perpendicular to the path AUTO will actually
            // drive, not to wherever the boat was when /waypoint landed.
            startValid = false;
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
      case MODE_MANUAL: {
        uint16_t throttleUs = computeThrottleUs(ch[IBUS_IDX_THROTTLE], ch[IBUS_IDX_REVERSE]);
        uint16_t rudderUs   = mapRudderStickToServo(ch[IBUS_IDX_RUDDER]);
        uint16_t portUs, stbdUs;
        computePortStbd(throttleUs, rudderUs, portUs, stbdUs);
        setRudder(rudderUs);
        setEscsPortStbd(portUs, stbdUs);
        break;
      }
      case MODE_AUTO: {
        if (wpSet && gpsValid && !captured) {
            uint16_t engageUs  = (cruiseUs > AUTO_CRUISE_CAP_US) ? AUTO_CRUISE_CAP_US : cruiseUs;
            uint16_t rudderUs  = headingHoldUs(wpBearing);
            uint16_t portUs, stbdUs;
            computePortStbd(engageUs, rudderUs, portUs, stbdUs);
            setRudder(rudderUs);
            setEscsPortStbd(portUs, stbdUs);
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
    Serial.println("test_29_pool_integration v test_29-pool2.6-magcal2");
    sessionId = esp_random();   // app uses this to detect mid-flight reboots

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

    // Mag offsets — NVS wins if present, otherwise the hardcoded defaults
    // already loaded by their initializers remain in effect.
    magCalLoadFromNVS();

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
            Serial.printf("[AUDIO] DF1201S ready (vol=%u, indices: horn=%d gun=%d board=%d).\n",
                          DFP_VOLUME, DFP_HORN_INDEX, DFP_GUN_INDEX, DFP_BOARD_INDEX);
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
    server.on("/calibrate_mag/start", HTTP_POST,    handleCalibrateMagStart);
    server.on("/calibrate_mag/start", HTTP_OPTIONS, handleOptions);
    server.on("/calibrate_mag/abort", HTTP_POST,    handleCalibrateMagAbort);
    server.on("/calibrate_mag/abort", HTTP_OPTIONS, handleOptions);
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
    updateCogTrim();
    server.handleClient();

    wifiMaintain();

    updateMode();
    updateWaypointGeometry();
    applyOutputs();
}
