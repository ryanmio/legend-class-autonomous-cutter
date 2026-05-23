#!/usr/bin/env bash
# load.sh — copy DFPlayer-Pro audio tracks onto a mounted DF1201S in a
# deterministic order so playFileNum(N) is stable.
#
# Why this script exists:
#   The DF1201S indexes files by FAT write order, not filename. Dragging
#   a folder in Finder copies in alphabetical order — usually — but the
#   moment a macOS dot-file (._FILE, .DS_Store) sneaks in, the index
#   shifts silently. We hit this on the first water test (test_29).
#   path-based playback (AT+PLAYFILE) has its own DFRobot Issue #5 quirks
#   that aren't worth fighting. Index-based is what EF shipped on.
#
# Index map after this script runs:
#   playFileNum(1) → HORN.MP3   (app: horn)
#   playFileNum(2) → GUN.MP3    (app: gun)
#   playFileNum(3) → BOARD.MP3  (app: board)
#
# Usage:
#   1. Plug the DF1201S into USB, wait for it to mount.
#   2. ./load.sh /Volumes/<mount-name>
#   3. Re-flash test_29 and test all three sound buttons.

set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 /Volumes/<DF1201S-mount-point>"
    echo
    echo "Plug the DF1201S in via USB; it should appear under /Volumes/."
    echo "Then re-run this script with that path."
    exit 1
fi

if [[ ! -d "$DEVICE" ]]; then
    echo "error: $DEVICE is not a directory."
    exit 1
fi

# DF1201S has 128MB of internal storage. Refuse anything that looks
# substantially larger so we never wipe the wrong volume by mistake.
size_kb=$(df -k "$DEVICE" 2>/dev/null | tail -1 | awk '{print $2}')
if [[ -z "$size_kb" ]] || (( size_kb > 1024 * 1024 )); then
    echo "error: $DEVICE looks too big to be a DF1201S (${size_kb} KiB total)."
    echo "       refusing to wipe. confirm the mount path."
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Canonical write order. Edit this list (and the firmware indices in
# test_29_pool_integration.ino) together — they have to agree.
TRACKS=(HORN.MP3 GUN.MP3 BOARD.MP3)

for t in "${TRACKS[@]}"; do
    if [[ ! -f "$t" ]]; then
        echo "error: missing source file: $SCRIPT_DIR/$t"
        exit 1
    fi
done

echo "[1/5] Wiping existing audio + macOS junk on $DEVICE ..."
# Glob is case-insensitive on FAT, but bash isn't — match both.
rm -f "$DEVICE"/*.mp3 "$DEVICE"/*.MP3 2>/dev/null || true
rm -rf "$DEVICE"/SFX "$DEVICE"/sfx 2>/dev/null || true
# Strip AppleDouble resource forks and .DS_Store before we add new files.
rm -f "$DEVICE"/.DS_Store "$DEVICE"/._* 2>/dev/null || true
sync

echo "[2/5] Copying tracks one-at-a-time (cp -X, sync between) ..."
for i in "${!TRACKS[@]}"; do
    t="${TRACKS[$i]}"
    idx=$((i + 1))
    printf "      -> index %d: %s\n" "$idx" "$t"
    # -X: omit macOS extended attributes (resource forks etc.) so the
    #     DF1201S sees a clean FAT entry and nothing else.
    cp -X "$t" "$DEVICE/"
    sync
done

echo "[3/5] dot_clean (kill any AppleDouble entries Finder may have written) ..."
dot_clean -m "$DEVICE" || true

echo "[4/5] Final contents on device:"
ls -la "$DEVICE" | grep -iE '\.mp3$|^total' || true

echo "[5/5] Ejecting ..."
if ! diskutil eject "$DEVICE"; then
    echo "  (eject failed — eject manually from Finder before unplugging.)"
fi

echo
echo "Done. Index map (must match firmware):"
echo "  playFileNum(1) = HORN.MP3   → app button 'horn'"
echo "  playFileNum(2) = GUN.MP3    → app button 'gun'"
echo "  playFileNum(3) = BOARD.MP3  → app button 'board'"
