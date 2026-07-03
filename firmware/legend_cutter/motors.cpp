// motors.cpp
// PCA9685 @ 50 Hz drives ESCs (ch0/1) and rudder (ch2). Throttle / rudder
// mixing per test_17 (reverse interlock) and test_29 AUTO (differential
// thrust factor 0.3).

#include "motors.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

static Adafruit_PWMServoDriver pca(PCA9685_ADDR);
static uint16_t outRudder = NEUTRAL_US;
static uint16_t outPort   = NEUTRAL_US;
static uint16_t outStbd   = NEUTRAL_US;
// RAM only: boots at AUTO_DIFF_GAIN, live-tunable via POST /pid; a reboot
// restores the flashed default so the boat never depends on the app.
static float    liveDiffGain = AUTO_DIFF_GAIN;

static uint16_t usToTicks(uint16_t us) {
    return (uint16_t)((us / 20000.0f) * 4096);
}

void pcaWriteUs(uint8_t channel, uint16_t us) {
    pca.setPWM(channel, 0, usToTicks(us));
}

static uint16_t escUs(uint16_t us) {
    return ESC_DIRECTION_INVERTED ? (uint16_t)(3000 - us) : us;
}

bool motorsBegin() {
    // Wire is brought up in legend_cutter.ino setup() before any module init.
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(PCA9685_FREQ_HZ);
    Wire.beginTransmission(PCA9685_ADDR);
    if (Wire.endTransmission() != 0) return false;

    setRudder(NEUTRAL_US);
    setEscs(NEUTRAL_US);
    return true;
}

void setRudder(uint16_t us) {
    if (us < RUDDER_MIN_US) us = RUDDER_MIN_US;
    if (us > RUDDER_MAX_US) us = RUDDER_MAX_US;
    outRudder = us;
    pcaWriteUs(CH_RUDDER, us);
}

void setEscs(uint16_t us) {
    if (us < MIN_REV_US) us = MIN_REV_US;
    if (us > MAX_FWD_US) us = MAX_FWD_US;
    outPort = outStbd = us;
    pcaWriteUs(CH_ESC_PORT, escUs(us));
    pcaWriteUs(CH_ESC_STBD, escUs(us));
}

void setEscsPortStbd(uint16_t portUs, uint16_t stbdUs) {
    if (portUs < MIN_REV_US) portUs = MIN_REV_US;
    if (portUs > MAX_FWD_US) portUs = MAX_FWD_US;
    if (stbdUs < MIN_REV_US) stbdUs = MIN_REV_US;
    if (stbdUs > MAX_FWD_US) stbdUs = MAX_FWD_US;
    outPort = portUs;
    outStbd = stbdUs;
    pcaWriteUs(CH_ESC_PORT, escUs(portUs));
    pcaWriteUs(CH_ESC_STBD, escUs(stbdUs));
}

void computePortStbd(uint16_t throttleUs, uint16_t rudderUs,
                     uint16_t& portUs, uint16_t& stbdUs) {
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

// AUTO near-center damper: decoupled differential thrust from the raw PD yaw
// command (not the deadbanded rudder, so it stays live through the crossing).
// Split is symmetric about throttle (average thrust held = cruise unchanged),
// capped by AUTO_DIFF_MAX_SPLIT_US and by the headroom that keeps the high motor
// ≤ MAX_FWD_US and the low motor ≥ AUTO_DIFF_LOW_FLOOR_US (clean forward thrust,
// never the prop-bite/reverse cutoff). Positive yawCmd → port up / stbd down,
// the same steering sense as the rudder.
void computeDiffThrust(uint16_t throttleUs, float yawCmd,
                       uint16_t& portUs, uint16_t& stbdUs) {
    int diff = (int)(liveDiffGain * yawCmd);
    int headUp  = (int)MAX_FWD_US - (int)throttleUs;          // port room up
    int headDn  = (int)throttleUs - (int)AUTO_DIFF_LOW_FLOOR_US;  // stbd room down
    int maxSplit = headUp < headDn ? headUp : headDn;
    if (maxSplit > (int)AUTO_DIFF_MAX_SPLIT_US) maxSplit = (int)AUTO_DIFF_MAX_SPLIT_US;
    if (maxSplit < 0) maxSplit = 0;                           // no clean headroom → no split
    if (diff >  maxSplit) diff =  maxSplit;
    if (diff < -maxSplit) diff = -maxSplit;
    portUs = (uint16_t)((int)throttleUs + diff);
    stbdUs = (uint16_t)((int)throttleUs - diff);
}

// Reverse-interlock pattern from test_17: forward throttle (left stick
// above idle) always wins. Reverse (right-stick V down past deadband)
// only engages when the throttle stick is at idle.
static bool throttleAtIdle(uint16_t leftStickUs) {
    return leftStickUs <= THROTTLE_IDLE_MAX;
}
static bool rightStickCommandingReverse(uint16_t rightStickVUs) {
    return rightStickVUs < (uint16_t)(NEUTRAL_US - REVERSE_DEADBAND_US);
}

uint16_t computeThrottleUs(uint16_t leftStickUs, uint16_t rightStickVUs) {
    if (!throttleAtIdle(leftStickUs)) {
        if (leftStickUs >= 2000) return MAX_FWD_US;
        return (uint16_t)map(leftStickUs, THROTTLE_IDLE_MAX, 2000, NEUTRAL_US, MAX_FWD_US);
    }
    if (rightStickCommandingReverse(rightStickVUs)) {
        uint16_t rev = rightStickVUs;
        if (rev < MIN_REV_US) rev = MIN_REV_US;
        if (rev > NEUTRAL_US) rev = NEUTRAL_US;
        return rev;
    }
    return NEUTRAL_US;
}

uint16_t mapRudderStickToServo(uint16_t stickUs) {
    if (stickUs < 1000) stickUs = 1000;
    if (stickUs > 2000) stickUs = 2000;
    if (stickUs <= 1500) return (uint16_t)map(stickUs, 1000, 1500, RUDDER_MIN_US, NEUTRAL_US);
    return                       (uint16_t)map(stickUs, 1500, 2000, NEUTRAL_US, RUDDER_MAX_US);
}

uint16_t motorsRudderUs() { return outRudder; }
uint16_t motorsPortUs()   { return outPort;   }
uint16_t motorsStbdUs()   { return outStbd;   }

void  motorsSetAutoDiffGain(float gain) { liveDiffGain = gain; }
float motorsAutoDiffGain()              { return liveDiffGain; }
