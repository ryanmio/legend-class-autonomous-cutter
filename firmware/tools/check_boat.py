#!/usr/bin/env python3
"""
check_boat.py — read-only firmware health check over WiFi.

Hits the boat's HTTP API and validates that telemetry is streaming and sane.
READ-ONLY: only GETs /status and /telemetry. It never POSTs, so it cannot move
a motor, rudder, gun, or pump — safe to run with the boat powered on the bench.

Usage:
    python3 firmware/tools/check_boat.py <boat-ip> [--watch N]

    <boat-ip>   shown on the boot serial line "HTTP up at http://<ip>/", or in
                the app. e.g. 192.168.1.42
    --watch N   sample N times, 1 s apart, to confirm telemetry is live and
                uptime is advancing (default 1).

Exit code 0 = all required checks passed; 1 = something failed.
"""
import sys, json, time, urllib.request

REQUIRED = [  # keys test_29 (+ turret) always emits, regardless of state
 "v","session_id","uptime","heap","mode","cruise_us","failsafe_ack",
 "rc_ever_good","rc_age_ms","rudder_us","esc_us",
 "ch_throttle","ch_rudder","ch_reverse","ch_mode","ch_guard",
 "ch_gun_pan","gun_pan_us",
 "nav_on","bridge_on","deck_on","audio_ok",
 "bilge_fwd","bilge_mid","bilge_rear","pump","pump_manual","pump_stuck","pump_phase",
 "radar_on","radar_speed","radar_burst_ms","radar_pause_ms","depth_mode",
 "heading","heading_mag","cog_trim",
 "gps_fix","sats","wp_set","captured","captured_by","pid_kp","pid_kd",
 "mag_cal_state","mag_cal_progress","mag_calibrated","mag_cal_ts","mag_from_nvs",
 "mag_cal_quality","mag_off_x","mag_off_y","mag_off_z","mag_baseline_uT","mag_uT",
]

def get(ip, path, timeout=4):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode())

passes = fails = warns = 0
def ok(msg):   global passes; passes += 1; print(f"  \033[32mOK  \033[0m {msg}")
def warn(msg): global warns;  warns  += 1; print(f"  \033[33mWARN\033[0m {msg}")
def fail(msg): global fails;  fails  += 1; print(f"  \033[31mFAIL\033[0m {msg}")

def fnum(t, k):
    try: return float(t[k])
    except (KeyError, ValueError, TypeError): return None

def check(ip):
    print(f"→ http://{ip}\n--- /status ---")
    try:
        s = get(ip, "/status")
        ok(f"/status reachable — firmware v={s.get('v')} ip={s.get('ip')}") if s.get("ok") else fail(f"/status ok!=true: {s}")
    except Exception as e:
        fail(f"/status unreachable: {e}  (wrong IP? boat not on this network?)"); return

    print("--- /telemetry ---")
    try:
        t = get(ip, "/telemetry")
    except Exception as e:
        fail(f"/telemetry failed to fetch/parse: {e}"); return

    missing = [k for k in REQUIRED if k not in t]
    ok(f"all {len(REQUIRED)} required keys present") if not missing else fail(f"missing keys: {missing}")

    if t.get("mode") in ("MANUAL","AUTO","FAILSAFE"): ok(f"mode = {t['mode']}")
    else: fail(f"mode invalid: {t.get('mode')!r}")

    h = fnum(t, "heading")
    if h is not None and 0 <= h < 360: ok(f"heading(true) = {h}°  heading_mag = {t.get('heading_mag')}°  cog_trim = {t.get('cog_trim')}°")
    else: fail(f"heading out of range: {t.get('heading')}")

    for k in ("rudder_us","esc_us"):
        v = fnum(t, k)
        if v is None or 1000 <= v <= 2000: ok(f"{k} = {t.get(k)} µs")
        else: warn(f"{k} = {t.get(k)} µs (outside 1000..2000)")

    if t.get("rc_ever_good"):
        age = fnum(t, "rc_age_ms")
        (ok if (age is not None and age < 1000) else warn)(f"RC link: ever_good=true, last frame {t.get('rc_age_ms')} ms ago"
            + ("  (stale — TX off / out of range?)" if (age or 0) >= 1000 else ""))
        chs = {k: t.get(k) for k in ("ch_throttle","ch_rudder","ch_reverse","ch_mode","ch_guard","ch_gun_pan")}
        bad = {k:v for k,v in chs.items() if not (isinstance(v,int) and 900 <= v <= 2100)}
        ok(f"channels in band: {chs}") if not bad else warn(f"channels out of 900..2100: {bad}")
    else:
        warn("RC never acquired (rc_ever_good=false) — TX on and bound?")

    bv = fnum(t, "batt_v")
    if "batt_v" in t: (ok if (bv and 5 <= bv <= 30) else warn)(f"battery = {t.get('batt_v')} V / {t.get('batt_a')} A")
    else: warn("no batt_v — INA219 not detected (telemetry says voltage disabled)")

    ok(f"audio_ok = {t['audio_ok']}") if t.get("audio_ok") else warn("audio_ok=false — DF1201S didn't ACK")

    if t.get("gps_fix"): ok(f"GPS fix: {t.get('lat')},{t.get('lon')}  sats={t.get('sats')}")
    else: warn(f"no GPS fix yet — sats={t.get('sats')} (needs sky view; fine indoors)")

    mcs = t.get("mag_cal_state")
    ok(f"mag: state={mcs} calibrated={t.get('mag_calibrated')} from_nvs={t.get('mag_from_nvs')} quality={t.get('mag_cal_quality')} |B|={t.get('mag_uT')}µT") \
        if mcs in ("idle","collecting","done","failed") else fail(f"mag_cal_state invalid: {mcs!r}")
    return t

ip = None; watch = 1
args = sys.argv[1:]
for a in args:
    if a.startswith("--watch"): watch = int(a.split("=")[-1]) if "=" in a else int(args[args.index(a)+1])
    elif not a.startswith("-") and not a.isdigit(): ip = a
if not ip:
    print(__doc__); sys.exit(2)

uptimes = []
for i in range(watch):
    if watch > 1: print(f"\n===== sample {i+1}/{watch} =====")
    t = check(ip)
    if t: uptimes.append(t.get("uptime"))
    if i < watch - 1: time.sleep(1)

if watch > 1 and len(uptimes) >= 2:
    print()
    (ok if uptimes[-1] != uptimes[0] else fail)(
        f"uptime advanced {uptimes[0]}→{uptimes[-1]} s — telemetry is live" if uptimes[-1] != uptimes[0]
        else f"uptime stuck at {uptimes[0]} s — telemetry not refreshing / boat rebooting?")

print(f"\n=== {passes} ok, {warns} warn, {fails} fail ===")
sys.exit(1 if fails else 0)
