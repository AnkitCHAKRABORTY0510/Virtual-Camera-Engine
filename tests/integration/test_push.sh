#!/usr/bin/env bash
# =============================================================================
# test_push.sh — script input end to end (Phase 8)
#
#   python producer (60 fps, frame numbers as barcodes)
#        -> Unix socket -> engine (camera at 30 fps, real time) -> raw file
#
# Checks: the engine accepts the producer, keeps ITS OWN 30 fps clock, and the
# frames it shows are in order (frame numbers never go backwards) and complete
# (every frame's barcode is readable = no torn frames).
#
# Usage: test_push.sh <path/to/virtual-camera> <source-dir>
# =============================================================================
set -uo pipefail
APP="${1:?}"
SOURCE="${2:?}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; kill $engine 2>/dev/null' EXIT

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found: skipping"
    exit 0
fi

SOCKET="$WORK/vcam.sock"
"$APP" --source-type push --socket "$SOCKET" --fps 30 --duration 2 --output file --output-file "$WORK/push.raw" \
    --pixel-format yuyv --report-json "$WORK/report.json" --quiet --no-stdin > "$WORK/engine.txt" 2>&1 &
engine=$!

python3 "$SOURCE/tools/examples/push_frames.py" --socket "$SOCKET" --width 320 --height 240 --fps 60 --seconds 4 \
    > "$WORK/producer.txt" 2>&1
wait "$engine"
code=$?
if [ "$code" -ne 0 ]; then
    echo "FAIL engine exit $code"; cat "$WORK/engine.txt"; exit 1
fi

python3 - "$WORK/push.raw" "$SOURCE/tools" <<'EOF'
import sys
sys.path.insert(0, sys.argv[2])
from vcam_client import read_barcode_luma

width, height = 320, 240
frame_bytes = width * height * 2  # YUYV
data = open(sys.argv[1], "rb").read()
frames = len(data) // frame_bytes
numbers = []
for i in range(frames):
    frame = data[i * frame_bytes:(i + 1) * frame_bytes]
    numbers.append(read_barcode_luma(lambda x, y: frame[(y * width + x) * 2], width, height))

unreadable = numbers.count(None)
readable = [n for n in numbers if n is not None]
backwards = sum(1 for a, b in zip(readable, readable[1:]) if b < a)
print("camera frames: %d (2 s at 30 fps), unreadable: %d, backwards: %d, first..last: %s..%s"
      % (frames, unreadable, backwards, readable[0] if readable else "-", readable[-1] if readable else "-"))
ok = 58 <= frames <= 61 and unreadable == 0 and backwards == 0
print("ok   push input end to end" if ok else "FAIL push input end to end")
sys.exit(0 if ok else 1)
EOF
