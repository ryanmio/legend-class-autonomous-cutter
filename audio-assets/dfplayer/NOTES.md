# DFPlayer Pro (DF1201S) audio — what we learned the hard way

This folder holds the three sound effects the boat plays via the app
(horn / board / gun) plus a load script. If the audio breaks again,
**read this whole file before changing firmware** — almost every
"bug" we hit was about the device's storage state, not the code.

---

## Hardware identity (don't get this wrong)

This boat uses the **DFPlayer Pro** (DFRobot SKU DFR0768, chip
DF1201S). It is **not** the cheaper DFPlayer Mini. They look similar
and people call both "DFPlayer," but they are incompatible:

| | DFPlayer Mini | DFPlayer Pro (DF1201S) |
|---|---|---|
| Baud | 9600 | **115200** |
| Protocol | binary frames `0x7E ... 0xEF` | AT command strings (`AT+...\r\n`) |
| Library | `DFRobotDFPlayerMini` | **`DFRobot_DF1201S`** |
| Storage | SD card only | **128 MB internal flash** + optional SD |

Pointing the wrong driver at the wrong module = no audio + garbage on
serial. We've made this mistake twice now (`legend_cutter/audio.cpp`
is still wrong — written for the Mini).

---

## How playback works

The DF1201S plays files by **index**, where the index is the FAT
directory **write order** — not the filename. `playFileNum(1)` plays
the first file written to storage, regardless of what it's called.

There is also `playSpecFile("/path/file.mp3")` (path-based playback)
in the library. **Do not use it.** See gotcha #3.

---

## The four gotchas that bit us

### 1. macOS Finder creates invisible companion files

When you drag any file from Mac → a FAT volume in Finder, macOS
silently writes a hidden sidecar `._FILENAME.MP3` next to it (an
"AppleDouble" file containing extended attributes Finder wants to
preserve). These do **not** appear in Finder even with hidden files
toggled on, but the DF1201S indexes them like real files.

Result: drag HORN → GUN → BOARD and the actual index map becomes:

| index | file |
|---|---|
| 1 | HORN.MP3 |
| 2 | ._HORN.MP3 (invisible companion) |
| 3 | GUN.MP3 |
| 4 | ._GUN.MP3 |
| 5 | BOARD.MP3 |
| 6 | ._BOARD.MP3 |

So `playFileNum(2)` doesn't play GUN — it plays a tiny ~80-byte
metadata file and the chip silently skips to the next real audio.

**Fix:** strip Mac metadata from the source files before dragging:
```
xattr -cr audio-assets/dfplayer/
```
…or use the load.sh script which copies with `cp -X` (which strips
xattrs during the copy).

### 2. Internal flash hoards old files forever

The DF1201S has 128 MB of internal flash. If the device was ever
used on another project (in our case the Edmund Fitzgerald boat),
**every file from that project is still on it.** Plugging the device
into a fresh Mac and dragging new files just adds to the pile — your
new files end up at indices >20 because all the old ones come first.

**Fix:** erase the volume in Disk Utility before reloading. Format
as **MS-DOS (FAT)**. This actually nukes the internal flash.

### 3. DFRobot Issue #5 — path-based playback is broken

The library's `playSpecFile("/HORN.MP3")` (which sends
`AT+PLAYFILE=/HORN.MP3` to the chip) has unfixed bugs documented in
[DFRobot/DFRobot_DF1201S#5](https://github.com/DFRobot/DFRobot_DF1201S/issues/5)
(open since 2022). On *any* path mis-resolution — filename too long,
subfolder too deep, hidden companion present, Mac extended-attribute
nonsense — the chip silently falls back to **playing file 1** with no
error. Multiple users have confirmed; maintainer claims it works,
library still v1.0.2.

We hit this hard during initial debugging. **Use index-based
playback only** (`playFileNum`).

### 4. playFileNum dropped while a previous track is playing

If you call `playFileNum(N)` while the chip is still playing a
previous track, the chip silently ignores the new command and keeps
playing the old one. (Documented in EF commit `54dc3ca`.)

**Fix in firmware** (already applied in `handleAudio`):
```cpp
DF1201S.pause();
delay(50);
DF1201S.playFileNum(track);
```

---

## How to (re)load the audio files cleanly

Result you want: device contains **only** HORN.MP3, GUN.MP3,
BOARD.MP3, with no companions and no leftovers, so indices are
exactly **1, 2, 3**.

1. Plug the DFPlayer Pro into your Mac via USB. It mounts as a FAT
   volume (usually called `NO NAME`).
2. Open **Disk Utility**. Find the volume in the sidebar under
   "External" — click it, click **Erase** at the top, set Format to
   **MS-DOS (FAT)**, click Erase.
3. In a terminal at the repo root, strip Mac metadata from the
   source files:
   ```
   xattr -cr audio-assets/dfplayer/
   ```
4. In Finder, drag the three files **one at a time, in this order**:
   - `HORN.MP3` first → becomes index 1
   - `GUN.MP3` second → becomes index 2
   - `BOARD.MP3` third → becomes index 3
5. Right-click the volume in Finder → **Eject**.
6. Plug the DFPlayer back into the boat, power on, then verify (see
   below).

**Alternative**: if your terminal has "Removable Volumes" permission
in System Settings → Privacy & Security → Files and Folders, you can
do steps 3–5 in one shot with `./load.sh "/Volumes/NO NAME"`.

---

## Diagnosing if it breaks again

The DF1201S can tell you what's actually at each index. The
diagnostic endpoint is **not** in the production sketch any more,
but the snippet below can be pasted back into any sketch that
already has the DF1201S library initialized. Look at git commit
`ceb37794` for the full original handler if you want the exact
JSON-response wiring.

```cpp
// One-shot probe: force SINGLE mode (EF 6353ed9), call playFileNum,
// give the chip 200ms to load, then ask what it actually selected.
for (int i = 1; i <= 8; i++) {
    DF1201S.pause();
    delay(50);
    DF1201S.setPlayMode(DF1201S.SINGLE);
    delay(50);
    DF1201S.playFileNum(i);
    delay(200);
    Serial.printf("idx %d -> num=%u  name=%s  total=%us\n",
                  i,
                  DF1201S.getCurFileNumber(),
                  DF1201S.getFileName().c_str(),
                  DF1201S.getTotalTime());
    DF1201S.pause();  // stop the snippet before next probe
    delay(50);
}
```

`DF1201S.getTotalFile()` also returns the chip's total file count.

What the output tells you:
- **`total` > 3** → leftover files in internal flash from a previous
  project. Wipe via Disk Utility (Erase as MS-DOS FAT).
- **`req` and `num` don't match, or only odd indices populated** →
  AppleDouble (`._*`) companions present. Wipe + reload with
  `xattr -cr audio-assets/dfplayer/` *before* dragging.
- **`req=1` returns something other than HORN.MP3** → load order
  was wrong. Wipe + reload in HORN → GUN → BOARD order.

---

## Firmware index mapping

Defined in `firmware/tests/test_29_pool_integration/test_29_pool_integration.ino`:

```cpp
static const int16_t DFP_HORN_INDEX  = 1;   // HORN.MP3
static const int16_t DFP_GUN_INDEX   = 3;   // GUN.MP3
static const int16_t DFP_BOARD_INDEX = 5;   // BOARD.MP3
```

These reflect the *current* state of the device — AppleDouble
companions sit at the even indices, so real files land on odds. If
you ever wipe + reload the device using the procedure above with
`xattr -cr` (so no companions get written), the indices will become
**1, 2, 3** and you'll need to update the constants to match. Drop
in the diagnostic snippet (see below) any time you need to confirm.

---

## Reference commits / sources

- DFRobot library Issue #5 (path playback):
  https://github.com/DFRobot/DFRobot_DF1201S/issues/5
- DFRobot wiki (DFR0768): https://wiki.dfrobot.com/dfr0768/
- Edmund Fitzgerald commits that informed this work
  (`~/Documents/GitHub/EdmundFitzgeraldController`):
  - `6353ed9` — indexing investigation, `cp -X` + `dot_clean`,
    "phantom files at indices 5-6 are old internal storage"
  - `54dc3ca` — pause-before-play, dropped commands while playing
  - `a3b20a0` — easter egg (one file via path, sidesteps Issue #5)
  - `54b67fb` — production ready with verified index map 1-4
