# test_04_sonar — RCWL-1655 Depth Sonar

## Hardware
- Sensor: RCWL-1655 (not JSN-SR04T as originally specced — update CLAUDE.md)
- VCC: 5V
- TRIG (labeled TRIG_RX_SCL_I/O on board) → GPIO 27
- ECHO (labeled ECHO_TX_SDA on board) → GPIO 14
- GND → GND

## Bench test result
Working as of 2026-03-15. Reads distance correctly in air pointing at flat surface ~30cm away.

## Debug notes
- ECHO and TRIG were initially swapped — read the sensor's pin labels carefully, they are not in the same order as a standard HC-SR04
- INPUT_PULLDOWN on ECHO pin helps tell floating from a real idle-low signal during debug
- If ECHO idle=0 but raw_us=0 every ping, check physical connector seating on sensor — ours came loose

## Water deployment checklist
- Change `SOUND_SPEED_US_CM` in sketch from `58.0` (air) to `13.4` (freshwater)
- Same value lives in `config.h` as `SONAR_SOUND_SPEED_US_CM` — keep both in sync
- Mount flush with hull, transducer face perpendicular to the bottom
- Min range ~2 cm, max ~450 cm
