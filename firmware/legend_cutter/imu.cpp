// imu.cpp
// ICM-20948 9-DOF + complementary filter + PD heading-hold + onboard mag
// calibration (NVS-backed) + GPS-COG true-heading trim.
// Faithful port of the test_29 IMU section (the PASS'd magcal2 build).

#include "imu.h"
#include "config.h"
#include "gps.h"
#include "motors.h"
#include <Wire.h>
#include "ICM_20948.h"
#include <Preferences.h>
#include <math.h>

static ICM_20948_I2C myICM;

// ── Heading / filter state ─────────────────────────────────────────────────
static float        fusedHeading    = 0.0f;
static float        prevHeadingForD = 0.0f;
// Turn rate (deg/s) measured once per IMU update so dt matches the heading
// cadence. Sampling at loop rate produced spiky garbage (see test_29 note).
static float        headingRateDps  = 0.0f;
static unsigned long lastImuUs      = 0;
static uint32_t     lastImuPollMs   = 0;
static bool         headingInit     = false;

static float        cogTrimDeg      = 0.0f;
static uint32_t     lastCogTrimMs   = 0;
static float        savedCogTrim    = 0.0f;   // last value written to NVS
static uint32_t     lastCogSaveMs   = 0;

static float        livePidKp = DEFAULT_KP;
static float        livePidKd = DEFAULT_KD;

// ── Mag calibration state (NVS-backed, runtime mutable) ────────────────────
enum MagCalState { MAG_CAL_IDLE, MAG_CAL_COLLECTING, MAG_CAL_DONE, MAG_CAL_FAILED };
static MagCalState magCalState = MAG_CAL_IDLE;

static float    magOffX = DEFAULT_MAG_OFFSET_X;
static float    magOffY = DEFAULT_MAG_OFFSET_Y;
static float    magOffZ = DEFAULT_MAG_OFFSET_Z;
static float    magBaselineUT = 0.0f;   // 0 = no cal recorded yet
static uint32_t magCalTs = 0;
static bool     magFromNVS = false;

// Cal-in-progress scratch (only valid during MAG_CAL_COLLECTING).
static float    calMinX, calMinY, calMinZ, calMaxX, calMaxY, calMaxZ;
static int      calSampleCnt = 0;
static unsigned long calStartMs = 0;
static uint16_t calSectorMask = 0;
static float    calBinCenterY = 0.0f, calBinCenterZ = 0.0f;
static bool     calHavePrev = false;
static float    calPrevX = 0.0f, calPrevY = 0.0f, calPrevZ = 0.0f;
static char     magCalFailBuf[96] = "";
static const char* magCalFailReason = "";

static float    magCalRadiusUT = 0.0f;  // mean horizontal circle radius from the spin
static float    magCalCircPct  = 0.0f;  // Y-vs-Z radius mismatch (soft-iron indicator)
enum MagCalQuality { MAG_QUAL_UNKNOWN = 0, MAG_QUAL_GOOD, MAG_QUAL_FAIR, MAG_QUAL_POOR };
static uint8_t  magCalQuality  = MAG_QUAL_UNKNOWN;

static float    liveMagUT = 0.0f;

static Preferences magPrefs;

// ── Helpers ────────────────────────────────────────────────────────────────
static float shortestPathError(float t, float c) {
    float e = t - c;
    while (e >  180.0f) e -= 360.0f;
    while (e < -180.0f) e += 360.0f;
    return e;
}

static const char* magCalStateNameOf(MagCalState s) {
    return s == MAG_CAL_IDLE       ? "idle"
         : s == MAG_CAL_COLLECTING ? "collecting"
         : s == MAG_CAL_DONE       ? "done"
                                   : "failed";
}
static const char* magCalQualityNameOf(uint8_t q) {
    return q == MAG_QUAL_GOOD ? "good"
         : q == MAG_QUAL_FAIR ? "fair"
         : q == MAG_QUAL_POOR ? "poor" : "unknown";
}

// ── NVS ────────────────────────────────────────────────────────────────────
static void magCalLoadFromNVS() {
    magPrefs.begin("imu_cal", true);
    bool present = magPrefs.isKey("off_x") && magPrefs.isKey("base_uT");
    if (present) {
        magOffX        = magPrefs.getFloat("off_x", DEFAULT_MAG_OFFSET_X);
        magOffY        = magPrefs.getFloat("off_y", DEFAULT_MAG_OFFSET_Y);
        magOffZ        = magPrefs.getFloat("off_z", DEFAULT_MAG_OFFSET_Z);
        magBaselineUT  = magPrefs.getFloat("base_uT", 0.0f);
        magCalTs       = magPrefs.getUInt ("cal_ts", 0);
        magCalRadiusUT = magPrefs.getFloat("rad_uT", 0.0f);
        magCalCircPct  = magPrefs.getFloat("circ_pct", 0.0f);
        magCalQuality  = (uint8_t)magPrefs.getUChar("quality", MAG_QUAL_UNKNOWN);
        // Trim belongs to this mag frame, so it rides along with the cal.
        cogTrimDeg     = magPrefs.getFloat("cog_trim", 0.0f);
        savedCogTrim   = cogTrimDeg;
        magFromNVS = true;
    }
    magPrefs.end();
    Serial.printf("[mag-cal] %s  off=(%.2f, %.2f, %.2f)  base=%.2f uT  cog_trim=%.1f deg\n",
                  magFromNVS ? "loaded from NVS" : "NVS empty, using hardcoded defaults",
                  magOffX, magOffY, magOffZ, magBaselineUT, cogTrimDeg);
}

// Persist just the learned COG trim — called on a throttle as it drifts, so it
// survives a power cycle (NVS namespace shared with the mag cal).
static void cogTrimSaveToNVS() {
    magPrefs.begin("imu_cal", false);
    magPrefs.putFloat("cog_trim", cogTrimDeg);
    magPrefs.end();
    savedCogTrim  = cogTrimDeg;
    lastCogSaveMs = millis();
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
    magPrefs.putFloat("cog_trim", cogTrimDeg);   // reset to 0 by a fresh cal
    magPrefs.end();
    savedCogTrim  = cogTrimDeg;
    lastCogSaveMs = millis();
    magFromNVS = true;
    Serial.printf("[mag-cal] saved: off=(%.2f, %.2f, %.2f) base=%.2f uT\n",
                  magOffX, magOffY, magOffZ, magBaselineUT);
}

// ── Mag calibration ─────────────────────────────────────────────────────────
static bool magCalibratedFlag() {
    return magBaselineUT > 0.0f
        && (magOffX != 0.0f || magOffY != 0.0f || magOffZ != 0.0f);
}

static void magCalFinishSuccess() {
    // Chip X is vertical: a flat spin can't separate its offset from earth's
    // vertical field, so the X center absorbs both. Level-water heading only
    // uses Y/Z; the X term matters only under tilt.
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
    magBaselineUT = magCalRadiusUT;
    magCalTs = millis() / 1000;
    magCalSaveToNVS();
    headingInit = false;   // re-seed the fused heading from the new offsets
    cogTrimDeg  = 0.0f;    // learned residual belongs to the old mag frame
    magCalState = MAG_CAL_DONE;
    Serial.printf("[mag-cal] DONE  samples=%d  radius=%.1f µT (expect ~%.1f)  circ=%.0f%%  quality=%s\n",
                  calSampleCnt, magCalRadiusUT, EXPECTED_HORIZ_FIELD_UT,
                  magCalCircPct, magCalQualityNameOf(magCalQuality));
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

// Called from imuUpdate() every IMU sample. Cheap when state != collecting.
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

    // Sector coverage on the horizontal (chip Y/Z) circle.
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

void imuMagCalBegin() {
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

bool imuMagCalAbort() {
    if (magCalState != MAG_CAL_COLLECTING) {
        if (magCalState == MAG_CAL_FAILED) magCalState = MAG_CAL_IDLE;
        return false;
    }
    magCalFinishFail("operator aborted");
    magCalState = MAG_CAL_IDLE;
    return true;
}

bool imuMagCalCollecting() { return magCalState == MAG_CAL_COLLECTING; }

// ── Init ─────────────────────────────────────────────────────────────────
bool imuBegin() {
    // Mag offsets — NVS wins if present, else the hardcoded defaults already
    // loaded by their initializers remain in effect.
    magCalLoadFromNVS();
    for (int i = 0; i < 3; i++) {
        myICM.begin(Wire, 0);
        if (myICM.status == ICM_20948_Stat_Ok) return true;
        delay(500);
    }
    return false;
}

// ── Complementary filter ────────────────────────────────────────────────────
void imuUpdate() {
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
        fusedHeading    = magH;
        prevHeadingForD = magH;
        headingInit     = true;
    } else {
        float yawRate = (accelMag > 100.0f) ? (ax*gx + ay*gy + az*gz)/accelMag : 0.0f;
        float gyroH = fusedHeading + yawRate * dt;
        while (gyroH <   0.0f) gyroH += 360.0f;
        while (gyroH >= 360.0f) gyroH -= 360.0f;
        float diff = magH - gyroH;
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        fusedHeading = gyroH + (1.0f - IMU_FILTER_ALPHA) * diff;
        if (fusedHeading <   0.0f) fusedHeading += 360.0f;
        if (fusedHeading >= 360.0f) fusedHeading -= 360.0f;
        if (dt > 0.0f) {
            headingRateDps = shortestPathError(fusedHeading, prevHeadingForD) / dt;
        }
        prevHeadingForD = fusedHeading;
    }
}

// Best estimate of TRUE heading: fused mag/gyro + declination + COG residual.
float imuHeadingTrue() {
    float h = fusedHeading + MAG_DECLINATION_DEG + cogTrimDeg;
    while (h <    0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    return h;
}
float imuHeadingMag()   { return fusedHeading; }
float imuCogTrim()      { return cogTrimDeg; }
bool  imuHeadingReady() { return headingInit; }

// Learn the residual heading error from GPS course-over-ground, 1 Hz. Gated
// to moments when COG actually means heading: fix valid, fast enough, driving
// roughly straight, and under forward thrust (COG flips 180° in reverse).
void imuUpdateCogTrim() {
    if (millis() - lastCogTrimMs < COG_TRIM_INTERVAL_MS) return;
    lastCogTrimMs = millis();
    if (!headingInit || !gpsValid()) return;
    if (!gpsSpeedValid() || !gpsCourseValid()) return;
    if (gpsSpeedKnots() < COG_TRIM_MIN_KTS) return;
    if (fabsf(headingRateDps) > COG_TRIM_MAX_TURN_DPS) return;
    if (((motorsPortUs() + motorsStbdUs()) / 2) <= NEUTRAL_US + 25) return;
    float err = shortestPathError(gpsCourseDeg(), imuHeadingTrue());
    cogTrimDeg += COG_TRIM_GAIN * err;
    if (cogTrimDeg >  COG_TRIM_CLAMP_DEG) cogTrimDeg =  COG_TRIM_CLAMP_DEG;
    if (cogTrimDeg < -COG_TRIM_CLAMP_DEG) cogTrimDeg = -COG_TRIM_CLAMP_DEG;

    if (millis() - lastCogSaveMs >= COG_TRIM_SAVE_INTERVAL_MS &&
        fabsf(cogTrimDeg - savedCogTrim) >= COG_TRIM_SAVE_MIN_DELTA_DEG) {
        cogTrimSaveToNVS();
    }
}

// Heading-hold output. D-term uses headingRateDps from imuUpdate() (IMU
// cadence), not a loop-rate recompute. Slew limiting prevents instant
// full-deflection steps; deadband suppresses rudder hunting near setpoint.
static uint16_t lastAutoRudder = NEUTRAL_US;
static uint32_t lastSlewMs     = 0;

uint16_t imuHeadingHoldUs(float target) {
    if (!headingInit) {
        lastAutoRudder = NEUTRAL_US;
        return NEUTRAL_US;
    }
    float err  = shortestPathError(target, imuHeadingTrue());
    float dErr = -headingRateDps;

    int desired;
    if (fabsf(err) < AUTO_HEADING_DEADBAND_DEG) {
        desired = NEUTRAL_US;
    } else {
        desired = (int)(NEUTRAL_US + livePidKp * err + livePidKd * dErr);
        if (desired < (int)RUDDER_MIN_US) desired = (int)RUDDER_MIN_US;
        if (desired > (int)RUDDER_MAX_US) desired = (int)RUDDER_MAX_US;
    }

    uint32_t now = millis();
    float    dt  = (now - lastSlewMs) * 0.001f;
    lastSlewMs = now;
    if (dt > 0.5f) {            // AUTO was disengaged — reset slew state
        lastAutoRudder = NEUTRAL_US;
        dt = 0.0f;
    }
    int maxDelta = (int)(AUTO_RUDDER_SLEW_US_PER_S * dt + 0.5f);
    int delta    = desired - (int)lastAutoRudder;
    if (delta >  maxDelta) delta =  maxDelta;
    if (delta < -maxDelta) delta = -maxDelta;
    int v = (int)lastAutoRudder + delta;
    if (v < (int)RUDDER_MIN_US) v = (int)RUDDER_MIN_US;
    if (v > (int)RUDDER_MAX_US) v = (int)RUDDER_MAX_US;
    lastAutoRudder = (uint16_t)v;
    return lastAutoRudder;
}

void  setPidGains(float kp, float kd) { livePidKp = kp; livePidKd = kd; }
float pidKp() { return livePidKp; }
float pidKd() { return livePidKd; }

// ── Mag-cal telemetry getters ──────────────────────────────────────────────
const char* imuMagCalStateName()    { return magCalStateNameOf(magCalState); }
int imuMagCalProgressPct() {
    if (magCalState == MAG_CAL_DONE) return 100;
    if (magCalState != MAG_CAL_COLLECTING) return 0;
    return magCalSectorCount() * 100 / MAG_CAL_SECTORS;
}
bool        imuMagCalibrated()      { return magCalibratedFlag(); }
uint32_t    imuMagCalTs()           { return magCalTs; }
bool        imuMagFromNVS()         { return magFromNVS; }
uint16_t    imuMagCalMask()         { return calSectorMask; }
bool        imuMagCalFailed()       { return magCalState == MAG_CAL_FAILED; }
const char* imuMagCalFailReason()   { return magCalFailReason; }
const char* imuMagCalQualityName()  { return magCalQualityNameOf(magCalQuality); }
bool        imuMagCalQualityKnown() { return magCalQuality != MAG_QUAL_UNKNOWN; }
float       imuMagCalRadiusUT()     { return magCalRadiusUT; }
float       imuMagCalCircPct()      { return magCalCircPct; }
float       imuMagOffX()            { return magOffX; }
float       imuMagOffY()            { return magOffY; }
float       imuMagOffZ()            { return magOffZ; }
float       imuMagBaselineUT()      { return magBaselineUT; }
float       imuLiveMagUT()          { return liveMagUT; }
