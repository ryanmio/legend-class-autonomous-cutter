#!/usr/bin/env bash
#
# Fix a viewer REC capture so QuickTime / iMovie / iMessage / Photos can open it.
#
# Safari's MediaRecorder writes a fragmented MP4 whose H.264 bitstream Apple's
# AVFoundation refuses to decode ("Cannot Decode"); Chrome/Firefox write WebM,
# which Apple apps don't open at all. Both are fixed by re-encoding to clean,
# faststart H.264 — that's all this script does.
#
# Usage:
#   ./fix-recording.sh <file ...>     # convert the named recording(s)
#   ./fix-recording.sh                # convert every replay-*.mp4 / *.webm in mission_logs
#
# Output: alongside each input as "<name>-fixed.mp4". Originals are left untouched.
# Needs ffmpeg:  brew install ffmpeg

set -euo pipefail

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "error: ffmpeg not found. Install it with:  brew install ffmpeg" >&2
  exit 1
fi

# mission_logs is the parent of this script's viewer/ folder.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Build the work list: explicit args, or auto-discover recordings.
files=()
if [ "$#" -gt 0 ]; then
  files=("$@")
else
  shopt -s nullglob
  for f in "$LOG_DIR"/replay-*.mp4 "$LOG_DIR"/replay-*.webm; do
    case "$f" in
      *-fixed.mp4) continue ;;   # skip our own outputs
    esac
    files+=("$f")
  done
  shopt -u nullglob
fi

if [ "${#files[@]}" -eq 0 ]; then
  echo "no recordings to convert (looked for replay-*.mp4 / *.webm in $LOG_DIR)" >&2
  exit 0
fi

for in in "${files[@]}"; do
  if [ ! -f "$in" ]; then
    echo "skip: not a file: $in" >&2
    continue
  fi
  out="${in%.*}-fixed.mp4"
  echo "converting: $in"
  ffmpeg -y -v error -i "$in" \
    -c:v libx264 -preset medium -crf 20 -profile:v high -pix_fmt yuv420p \
    -movflags +faststart -an "$out"
  echo "  -> $out"
done

echo "done."
