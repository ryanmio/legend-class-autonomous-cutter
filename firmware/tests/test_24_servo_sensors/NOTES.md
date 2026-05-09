# test_24_servo_sensors — Results

## Gates

| Gate | Description | Result |
|------|-------------|--------|
| GATE 1 | PCA9685 (0x40) AND ICM-20948 (0x68) both init on shared I2C bus | — |
| GATE 2 | WiFi connects, IP shown | — |
| GATE 3 | Type 'R': rudder sweeps 1330–1670 µs, zero I2C errors | — |
| GATE 4 | Heading variance < 5° during the sweep | — |
| GATE 5 | App /telemetry shows rudder_us field while servo active | — |

## Notes

*Test not yet run.*

### What this test proves

PCA9685 and ICM-20948 have never run simultaneously on the I2C bus. The FIRMWARE_AUDIT (§6) flags "I2C bus under load" as a required pre-water test. This test is that check.

### Hardware setup

- PCA9685 SDA/SCL → GPIO21/22. Power: logic=3.3V (ESP32), V+=6V for servo rail.
- Rudder servo on PCA9685 ch2 with linkage connected (limits were found installed).
- ICM-20948 on same SDA/SCL pins, AD0=GND (addr 0x68).
- BN-220 GPS: white wire → GPIO17 (RX), green → GPIO4 (TX).
- No iBUS receiver needed for this test.
