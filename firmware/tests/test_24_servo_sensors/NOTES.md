# test_24_servo_sensors — Results

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1 | PCA9685 (0x40) AND ICM-20948 (0x68) both init on shared I2C bus | — |
| GATE 2 | WiFi connects, IP shown | — |
| GATE 3 | Set target, rotate boat ~30° off, type 'G' — rudder in correct direction and >20 µs from neutral | — |
| GATE 4 | Return boat near target, type 'G' — rudder within 30 µs of neutral | — |
| GATE 5 | App /telemetry shows heading_target, heading_error, rudder_us | — |

## Notes

*Test not yet run.*

### What this test proves

This is the first closed-loop bench demo: set a target heading, physically rotate
the boat, and the rudder deflects to correct the error — exactly how it will
behave under sail. It proves I2C bus coexistence (PCA9685 + ICM-20948 have
never shared the bus), correct controller direction, and proportional magnitude.

### Procedure

1. Flash. Serial Monitor at 115200.
2. Gate 1 prints automatically — both devices must be found or sketch halts.
3. Gate 2 — confirm IP in Serial output.
4. Wait for heading to stabilise (~5 s), then type `H <current-heading>` to lock
   the target to the current bearing (rudder should sit near neutral).
5. Slowly rotate the boat ~30° off target. Rudder should deflect.
6. Type `G` — [GATE 3] PASS if direction is correct and deflection > 20 µs.
7. Rotate boat back near target. Type `G` — [GATE 4] PASS if rudder < 30 µs
   from neutral.
8. Gate 5 — open app, check /telemetry shows heading_target, heading_error,
   rudder_us.

### Tuning Kp

Default Kp = 3.0 µs/deg → full deflection (~170 µs) at ~57° error.
If response feels too sluggish, type `K 5.0`. If the rudder slams to the stop
before you've rotated 45°, type `K 2.0`. Record the value that feels right here.

### Hardware setup

- PCA9685 SDA/SCL → GPIO21/22. Power: logic=3.3V (ESP32), V+=6V for servo rail.
- Rudder servo on PCA9685 ch2 with linkage connected (limits confirmed test_15).
- ICM-20948 on same SDA/SCL, AD0=GND (addr 0x68).
- BN-220 GPS: white wire → GPIO17 (RX), green → GPIO4 (TX).
- No iBUS receiver needed for this test.
