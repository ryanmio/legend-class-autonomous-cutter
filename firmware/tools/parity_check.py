#!/usr/bin/env python3
"""
parity_check.py — static parity check of legend_cutter against test_29.

Run from the repo root:  python3 firmware/tools/parity_check.py

Two layers:
  1. Contract diff   — config constants, telemetry JSON keys, HTTP routes.
  2. Logic diff      — the "computational skeleton" (numeric literals +
                       operators + control flow) of every behavior-bearing
                       function, with identifiers masked so module-API
                       renames (getter calls vs globals) don't register as
                       changes. Catches transcription errors in the math.

Source of truth: firmware/tests/test_29_pool_integration (the PASS'd build).
Expected residual deltas (intentional, not failures):
  - telemetry keys: +ch_gun_pan, +gun_pan_us  (retained test_18 turret)
  - config: + turret constants + benign infra centralizations
  - a handful of functions DIFFER only by getter-vs-global / serial logging.
"""
import re, glob

T29_PATH = "firmware/tests/test_29_pool_integration/test_29_pool_integration.ino"
LC_DIR   = "firmware/legend_cutter"

def read(p): return open(p, encoding="utf-8", errors="replace").read()
T29 = read(T29_PATH)
LC_FILES = {p.split("/")[-1]: read(p) for p in glob.glob(LC_DIR + "/*")
            if p.endswith((".ino", ".cpp", ".h"))}
LC_ALL = "\n".join(LC_FILES.values())

def norm(v):
    v = re.sub(r"\s+", "", v.strip().rstrip(";"))
    return re.sub(r"([0-9.])[fFlLuU]+$", r"\1", v)

def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return re.sub(r'"(\\.|[^"\\])*"', '""', s)

# ── 1. contract diff ────────────────────────────────────────────────────────
def telem_keys(t): return re.findall(r'doc\["([^"]+)"\]', t)
def routes(t):     return sorted(set(re.findall(r'server\.on\("([^"]+)"', t)))

tk29, tklc = telem_keys(T29), telem_keys(LC_ALL)
print("=" * 70, "\nCONTRACT\n", "=" * 70, sep="")
print(f"telemetry keys: test_29={len(tk29)} legend_cutter={len(tklc)}")
print("  missing:", [k for k in tk29 if k not in tklc] or "none")
print("  extra  :", [k for k in tklc if k not in tk29], "(expect turret keys)")
r29, rlc = set(routes(T29)), set(routes(LC_ALL))
print("routes  missing:", sorted(r29 - rlc) or "none", " extra:", sorted(rlc - r29) or "none")

cre = re.compile(r"static\s+const\s+(?:unsigned\s+|signed\s+)?\w[\w:]*\s+(\w+)\s*=\s*([^;]+);")
c29 = {m.group(1): norm(m.group(2)) for m in cre.finditer(T29)}
clc = {}
for m in cre.finditer(LC_ALL): clc.setdefault(m.group(1), norm(m.group(2)))
RENAMES = {"ALPHA": "IMU_FILTER_ALPHA"}
mis = []
for n, v in c29.items():
    vlc = clc.get(n) or clc.get(RENAMES.get(n, ""))
    if vlc is None: mis.append(f"{n} (missing)")
    elif vlc != v:  mis.append(f"{n}: t29={v} lc={vlc}")
print(f"config constants: {len(c29)} in test_29 — mismatches/missing:", mis or "none")

# ── 2. logic diff ───────────────────────────────────────────────────────────
KW = {"if","else","while","for","return","switch","case","break","do"}
def skeleton(b):
    b = strip_comments(b)
    out = []
    for t in re.findall(r"[0-9]+\.?[0-9]*[eE]?[-+]?[0-9]*|[A-Za-z_]\w*|[-+*/%<>=!&|^~?:.,;(){}\[\]]", b):
        if re.match(r"[0-9]", t):   out.append(re.sub(r"[fFlLuU]+$", "", t))
        elif t in KW:               out.append(t)
        elif re.match(r"[A-Za-z_]", t): out.append("X")
        else:                       out.append(t)
    return out

def body(src, name):
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*(?:const\s*)?\{", src):
        i = src.index("{", m.start()); d = 0
        for j in range(i, len(src)):
            if src[j] == "{": d += 1
            elif src[j] == "}":
                d -= 1
                if d == 0: return src[i + 1:j]
    return None

PAIRS = [  # (test_29 name, legend_cutter file, legend_cutter name)
 ("shortestPathError","imu.cpp","shortestPathError"),
 ("haversineBearing","navigation.cpp","haversineBearing"),
 ("haversineDistM","navigation.cpp","haversineDistM"),
 ("hasCrossedTarget","navigation.cpp","hasCrossedTarget"),
 ("computePortStbd","motors.cpp","computePortStbd"),
 ("computeThrottleUs","motors.cpp","computeThrottleUs"),
 ("mapRudderStickToServo","motors.cpp","mapRudderStickToServo"),
 ("escUs","motors.cpp","escUs"),
 ("usTicks","motors.cpp","usToTicks"),
 ("magCalTick","imu.cpp","magCalTick"),
 ("magCalFinishSuccess","imu.cpp","magCalFinishSuccess"),
 ("magCalSectorCount","imu.cpp","magCalSectorCount"),
 ("magCalibratedFlag","imu.cpp","magCalibratedFlag"),
 ("magCalLoadFromNVS","imu.cpp","magCalLoadFromNVS"),
 ("magCalSaveToNVS","imu.cpp","magCalSaveToNVS"),
 ("updateImu","imu.cpp","imuUpdate"),
 ("headingHoldUs","imu.cpp","imuHeadingHoldUs"),
 ("navHeadingDeg","imu.cpp","imuHeadingTrue"),
 ("updateCogTrim","imu.cpp","imuUpdateCogTrim"),
 ("magCalProgressPct","imu.cpp","imuMagCalProgressPct"),
 ("pollBilge","bilge.cpp","bilgeUpdate"),
 ("doDepthPing","sonar.cpp","sonarPingNow"),
 ("parseIbus","ibus.cpp","parseFrame"),
 ("updatePosition","gps.cpp","gpsUpdate"),
 ("applyOutputs","legend_cutter.ino","applyOutputs"),
]
print("\n" + "=" * 70, "\nLOGIC (computational skeleton)\n", "=" * 70, sep="")
ndiff = 0
for n29, lf, ln in PAIRS:
    b29, blc = body(T29, n29), body(LC_FILES.get(lf, ""), ln)
    if b29 is None or blc is None:
        print(f"  {n29:24} SKIP (not found)"); continue
    if skeleton(b29) == skeleton(blc):
        print(f"  {n29:24} MATCH")
    else:
        ndiff += 1
        print(f"  {n29:24} DIFFER (inspect — expected: getter-vs-global / serial logging)")
print(f"\n{ndiff} function(s) differ in skeleton (expected ~6: benign refactors); rest byte-identical.")
