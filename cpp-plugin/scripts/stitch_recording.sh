#!/bin/bash
#
# Combines a scope-recording session (a PNG frame sequence + frames_manifest.csv
# from ScopeFrameRecorder, plus input.wav/output.wav from AudioSessionRecorder --
# see Source/DSP/ScopeFrameRecorder.h and Source/DSP/AudioSessionRecorder.h) into
# a single MP4 you can scrub through frame-by-frame later.
#
# Deliberately an offline step, not something the plugin does live: muxing here
# means the real-time-adjacent CorrectorWorker thread and the message-thread UI
# Timer only ever have to do the cheap half of this (write raw frames/audio),
# never touch a video encoder, and a stitch that goes wrong (bad ffmpeg args, a
# missing codec) can be re-run against the same raw session data without having
# re-recorded anything.
#
# Usage: ./scripts/stitch_recording.sh <session-dir> [output.mp4]
#   session-dir defaults to nothing -- must be passed explicitly.
#   output.mp4  defaults to <session-dir>/scope.mp4
#
# Requires ffmpeg (brew install ffmpeg).

set -euo pipefail

if [ -z "${1:-}" ]; then
  echo "Usage: $0 <session-dir> [output.mp4]"
  echo "  session-dir: a folder created by the plugin's Record button, e.g."
  echo "    ~/Library/Application Support/Pitchzazz/Recordings/2026-08-23_10-42-05"
  exit 1
fi

SESSION_DIR="$1"
OUTPUT_MP4="${2:-$SESSION_DIR/scope.mp4}"

if [ ! -d "$SESSION_DIR" ]; then
  echo "Session directory not found: $SESSION_DIR"
  exit 1
fi

MANIFEST="$SESSION_DIR/frames_manifest.csv"
if [ ! -f "$MANIFEST" ]; then
  echo "No frames_manifest.csv in $SESSION_DIR -- was this a recording session?"
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg not found. Install it with: brew install ffmpeg"
  exit 1
fi

# Frame count and observed rate, purely to report to the user -- ffmpeg
# itself is driven directly off the manifest below, not this estimate.
FRAME_COUNT=$(($(wc -l < "$MANIFEST") - 1))
LAST_MS=$(tail -n 1 "$MANIFEST" | cut -d',' -f2)
echo "Session: $SESSION_DIR"
echo "Frames: $FRAME_COUNT over ~${LAST_MS}ms"

# ScopeFrameRecorder can drop a frame under load (see its class doc), so the
# manifest's own elapsed_ms column -- not a fixed 30fps assumption -- is the
# source of truth for timing. Build an ffmpeg concat-demuxer script that gives
# each frame the exact duration until the next one actually arrived.
CONCAT_FILE=$(mktemp)
trap 'rm -f "$CONCAT_FILE"' EXIT

python3 - "$MANIFEST" "$SESSION_DIR" > "$CONCAT_FILE" <<'PYEOF'
import csv
import sys

manifest_path, session_dir = sys.argv[1], sys.argv[2]
with open(manifest_path, newline="") as f:
    rows = list(csv.DictReader(f))

for i, row in enumerate(rows):
    ms = float(row["elapsed_ms"])
    next_ms = float(rows[i + 1]["elapsed_ms"]) if i + 1 < len(rows) else ms + 33.0
    duration = max(next_ms - ms, 1.0) / 1000.0
    print(f"file '{session_dir}/{row['filename']}'")
    print(f"duration {duration:.4f}")

if rows:
    print(f"file '{session_dir}/{rows[-1]['filename']}'")
PYEOF

# Prefer output.wav (what you'd actually hear) as the audio track; fall back
# to input.wav if only that exists.
AUDIO_FILE="$SESSION_DIR/output.wav"
if [ ! -f "$AUDIO_FILE" ]; then
  AUDIO_FILE="$SESSION_DIR/input.wav"
fi

if [ -f "$AUDIO_FILE" ]; then
  ffmpeg -y -f concat -safe 0 -i "$CONCAT_FILE" -i "$AUDIO_FILE" \
    -vf "format=yuv420p" -c:v libx264 -c:a aac -shortest "$OUTPUT_MP4"
else
  echo "No audio found in $SESSION_DIR -- writing video-only MP4."
  ffmpeg -y -f concat -safe 0 -i "$CONCAT_FILE" -vf "format=yuv420p" -c:v libx264 "$OUTPUT_MP4"
fi

echo "Wrote $OUTPUT_MP4"
