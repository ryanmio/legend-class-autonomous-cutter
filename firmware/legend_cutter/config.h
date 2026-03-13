// config.h
// Central pin definitions, PCA9685 channel assignments, and tuning constants
// for the Legend Class Autonomous Cutter firmware.
// Edit this file when hardware changes — never scatter magic numbers in module files.

#pragma once

// ==================== BUILD IDENTIFICATION ====================
#define FIRMWARE_VERSION  "0.1.0"
#define VESSEL_NAME       "Legend Cutter"
#define OTA_HOSTNAME      "legend-cutter"

// ==================== ESP32 PIN DEFINITIONS ====================

// iBUS receiver (Flysky FS-iA10B, iBUS SERVO port)
#define IBUS_RX_PIN       16   // UART1 RX — 5V signal, use 1K+2K voltage divider

// GPS (BN-220 u-blox NEO-M8N)
#define GPS_RX_PIN         4   // UART2 RX — NMEA from GPS
#define GPS_TX_PIN        17   // UART2 TX — config commands to GPS (if needed)
#define GPS_BAUD        9600   // Default BN-220 baud rate

// I2C bus — shared by PCA9685, INA219, ICM-20948
#define I2C_SDA_PIN       21
#define I2C_SCL_PIN       22
#define I2C_FREQ        400000 // 400 kHz; keep total bus length < 30 cm

// I2C device addresses
#define PCA9685_ADDR    0x40
#define INA219_ADDR     0x41
#define ICM20948_ADDR   0x68   // AD0 low; use 0x69 if AD0 high

// DFPlayer Mini (software serial, 9600 baud)
#define DFPLAYER_TX_PIN   25   // ESP32 TX → DFPlayer RX
#define DFPLAYER_RX_PIN   26   // ESP32 RX ← DFPlayer TX

// Ultrasonic depth sonar (JSN-SR04T V3)
#define SONAR_TRIG_PIN    27
#define SONAR_ECHO_PIN    14
// Speed of sound in freshwater ~1480 m/s → ~13.4 µs/cm (NOT the default air value of 58)
#define SONAR_SOUND_SPEED_US_CM  13.4f

// Water intrusion sensors (ADC1 pins — ADC2 unavailable when WiFi active)
#define WATER_SENSOR_FWD_PIN  32  // Forward bilge — GPIO32 has internal pullup
#define WATER_SENSOR_AFT_PIN  33  // Aft bilge

// MOSFET gate outputs
#define BILGE_PUMP_PIN    12   // Active HIGH → runs bilge pump direct from 4S
#define RADAR_MOTOR_PIN   13   // Active HIGH → mast radar motor

// ==================== PCA9685 CHANNEL ASSIGNMENTS ====================
// All servos and ESCs are driven through the PCA9685 I2C PWM driver.
// Standard servo pulse range: 1000 µs (full reverse/left) to 2000 µs (full forward/right), 1500 µs = neutral.

#define CH_ESC_PORT        0   // Port (left) motor ESC
#define CH_ESC_STBD        1   // Starboard (right) motor ESC — counter-rotating prop
#define CH_RUDDER          2   // 20 kg waterproof rudder servo (twin rudders via tiller linkage)
#define CH_GUN_PAN         3   // Deck gun pan (MG90S, below-deck, drives turret via hollow shaft)
#define CH_GUN_TILT        4   // Deck gun tilt (2.1 g micro servo, pushrod through central tube)
#define CH_CIWS_PAN        5   // Phalanx CIWS pan servo
#define CH_CIWS_SPIN       6   // Phalanx barrel spin (6 mm coreless via L9110S — use full-on PWM)
#define CH_RADAR           7   // Top radar rotation motor
#define CH_BAY_DOOR_PORT   8   // Port bay door winch (FS90R continuous rotation)
#define CH_BAY_DOOR_STBD   9   // Starboard bay door winch (FS90R continuous rotation)
#define CH_ANCHOR_FWD     10   // Forward anchor winch (continuous rotation)
#define CH_ANCHOR_AFT     11   // Aft anchor winch (continuous rotation)
// 12–15: reserved for expansion (searchlight, ramp doors, etc.)

// PWM pulse widths (µs) — adjust per actual servo calibration
#define PWM_NEUTRAL       1500
#define PWM_MIN           1000
#define PWM_MAX           2000
#define PCA9685_FREQ        50  // Hz — standard 50 Hz for servos

// ==================== iBUS PROTOCOL ====================
#define IBUS_BAUD        115200
#define IBUS_CHANNELS        10
// Channel indices (0-based) from Flysky FS-i6X transmitter
#define IBUS_CH_THROTTLE      2   // Right stick vertical
#define IBUS_CH_RUDDER        3   // Right stick horizontal (or left stick)
#define IBUS_CH_MODE          4   // SWA/SWB switch — MANUAL vs AUTONOMOUS
// iBUS raw value range: 1000–2000

// ==================== CONTROL MIXING ====================
// Differential thrust: blend port/stbd ESC speeds with rudder input for tight turns.
// 0.0 = no differential (rudder only), 1.0 = full differential contribution.
#define DIFF_THRUST_FACTOR    0.3f

// Heading-hold PID defaults (stored in NVS, overridable from app)
#define PID_KP_DEFAULT        2.0f
#define PID_KI_DEFAULT        0.05f
#define PID_KD_DEFAULT        0.5f

// Waypoint capture radius (metres) — when to advance to next waypoint
#define WAYPOINT_CAPTURE_M    3.0f

// Low-voltage return-to-home threshold (volts)
#define BATTERY_RTH_VOLTAGE   13.0f   // 4S LiPo ~3.25 V/cell
#define BATTERY_ALARM_VOLTAGE 13.6f   // Warn before RTH

// Battery voltage divider ratio — adjust to match physical resistors
// INA219 input range: measure voltage at shunt input terminal
#define BATTERY_VOLTAGE_SCALE 1.0f   // INA219 reads bus voltage directly

// ==================== BILGE PUMP ====================
#define BILGE_DRY_DELAY_MS    5000   // Run pump this long after sensors read dry before stopping
#define BILGE_MAX_RUN_MS     60000   // Emergency shutoff (run-dry protection)

// ==================== FAILSAFE ====================
// If iBUS frames stop arriving for this long, engage failsafe (throttle off, rudder centre)
#define IBUS_FAILSAFE_MS      500

// ==================== WIFI / OTA ====================
// ESP32 runs as a WiFi Access Point for direct phone-to-boat connection.
// Credentials defined in secrets.h (not committed).
#define WIFI_AP_CHANNEL       6
#define WIFI_AP_MAX_CONN      2

// WebSocket telemetry port
#define TELEMETRY_WS_PORT    81
// HTTP control API port
#define HTTP_PORT            80
// Telemetry broadcast interval (ms)
#define TELEMETRY_INTERVAL_MS  100  // 10 Hz

// ==================== DFPlayer TRACK NUMBERS ====================
// Files on SD card stored as 0001.mp3, 0002.mp3, etc.
#define AUDIO_ENGINE_IDLE      1
#define AUDIO_ENGINE_REV       2
#define AUDIO_HORN             3
#define AUDIO_GUN_FIRE         4
#define AUDIO_CIWS_BRRT        5
#define AUDIO_GENERAL_ALARM    6
#define AUDIO_BOSUNS_WHISTLE   7
#define AUDIO_LRAD_HAIL_1      8
#define AUDIO_LRAD_HAIL_2      9
