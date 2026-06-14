# firmware/tools

Two helper scripts (Python 3 stdlib only — no installs).

## `parity_check.py` — does legend_cutter still match test_29?

Static check that the production firmware (`legend_cutter/`) hasn't drifted
from the source-of-truth test sketch (`test_29_pool_integration`). Run from the
repo root:

```
python3 firmware/tools/parity_check.py
```

It reports two layers:

- **Contract** — config constants, telemetry JSON keys, HTTP routes.
- **Logic** — the computational skeleton (numbers + operators + control flow,
  identifiers masked) of every behavior-bearing function, so module-API
  renames don't show up as changes but a real math change would.

**Expected (passing) output:** telemetry keys show `+ch_gun_pan, +gun_pan_us`
(the retained test_18 turret), routes/constants clean, and ~6 functions report
`DIFFER` — those differ only by getter-call-vs-global access and serial logging
present in the test sketch. Anything else is worth a look.

## `check_boat.py` — is the running firmware healthy? (read-only)

Run from your laptop while the boat is powered and on the same WiFi:

```
python3 firmware/tools/check_boat.py <boat-ip>          # one snapshot
python3 firmware/tools/check_boat.py <boat-ip> --watch 5 # 5 samples, confirms telemetry is live
```

The boat IP prints on the boot serial line `HTTP up at http://<ip>/` and shows
in the app. The script only **GET**s `/status` and `/telemetry` — it never
POSTs, so it cannot move a motor/rudder/gun/pump. It checks that all expected
telemetry keys are present and that values are sane (mode, heading, channels,
battery, GPS, mag-cal), and with `--watch` confirms `uptime` is advancing.
Exit 0 = healthy, 1 = a check failed.
