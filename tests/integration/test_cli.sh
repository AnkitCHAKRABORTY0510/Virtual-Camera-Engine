#!/usr/bin/env bash
# =============================================================================
# test_cli.sh — run the complete virtual-camera application like a user would
# (no V4L2 device needed: output goes to a raw file or nowhere).
#
# Usage: test_cli.sh <path/to/virtual-camera> <media-dir> <source-dir>
# =============================================================================
set -uo pipefail

APP="${1:?usage: test_cli.sh <virtual-camera> <media-dir> <source-dir>}"
MEDIA="${2:?}"
SOURCE="${3:?}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
failures=0

# expect <exit-code> <description> <command...>
expect() {
    local wanted="$1" description="$2"
    shift 2
    "$@" > "$WORK/out.txt" 2>&1 < /dev/null
    local got=$?
    if [ "$got" -eq "$wanted" ]; then
        echo "ok   $description (exit $got)"
    else
        echo "FAIL $description: exit $got, expected $wanted"
        tail -5 "$WORK/out.txt" | sed 's/^/     | /'
        failures=$((failures + 1))
    fi
}

# check <description> <condition...>
check() {
    local description="$1"
    shift
    if "$@"; then
        echo "ok   $description"
    else
        echo "FAIL $description"
        failures=$((failures + 1))
    fi
}

expect 0 "--help" "$APP" --help
expect 0 "--version" "$APP" --version
expect 1 "unknown option" "$APP" --colour red
expect 1 "fast mode without an end" "$APP" --input pattern --fast --output null --on-eof loop
expect 2 "missing input file" "$APP" --input /no/such/file.mp4 --output null --no-stdin
expect 2 "odd size with yuyv" "$APP" --input "$MEDIA/odd.mkv" --output null --no-stdin
expect 3 "missing camera device" "$APP" --input pattern --device /dev/video_missing --no-stdin

# Pattern -> raw file: 60 frames of 128x64 YUYV = 60 * 16384 bytes.
expect 0 "pattern to raw file (fast)" "$APP" --input pattern --pattern-size 128x64 --pattern-frames 60 \
    --fast --on-eof stop --output file --output-file "$WORK/pattern.raw" --quiet --no-stdin
check "raw file has 60 frames" test "$(stat -c %s "$WORK/pattern.raw")" -eq $((60 * 16384))

# Video file at a different output rate: 90 frames at 30 fps shown at 15 fps = 45 frames.
expect 0 "video 30 -> 15 fps (fast)" "$APP" --input "$MEDIA/cfr30.mp4" --fps 15 --fast --on-eof stop \
    --output file --output-file "$WORK/video.raw" --quiet --no-stdin
check "15 fps output has 45 frames" test "$(stat -c %s "$WORK/video.raw")" -eq $((45 * 320 * 240 * 2))

# Image sequence directory, disk buffer, I420.
expect 0 "image sequence, disk buffer" "$APP" --input "$MEDIA/images" --source-fps 25 --buffer-mode disk \
    --spill-dir "$WORK" --pixel-format i420 --fast --on-eof stop --output file --output-file "$WORK/images.raw" \
    --quiet --no-stdin
check "image output has 12 frames" test "$(stat -c %s "$WORK/images.raw")" -eq $((12 * 320 * 240 * 3 / 2))

# Config file + command-line override + JSON report, real time for 1 s.
cat > "$WORK/config.yaml" <<EOF
input:
  type: pattern
  pattern_size: 160x120
output:
  backend: null
  fps: 10
playback:
  duration: 5
EOF
expect 0 "config file, real time 1 s" "$APP" --config "$WORK/config.yaml" --duration 1 \
    --report-json "$WORK/report.json" --timing-csv "$WORK/frames.csv" --quiet --no-stdin
check "report JSON written" grep -q '"frames_published":1[01]' "$WORK/report.json"
check "per-frame CSV written" test "$(wc -l < "$WORK/frames.csv")" -ge 10

# Every example config parses (the input path in them is a placeholder).
for example in "$SOURCE"/config/examples/*.yaml; do
    expect 0 "example config parses: $(basename "$example")" "$APP" --config "$example" --help
done

# SIGINT during streaming -> clean exit 0 and a final report.
"$APP" --input pattern --pattern-size 160x120 --output null --no-stdin > "$WORK/sigint.txt" 2>&1 &
pid=$!
sleep 1.5
kill -INT "$pid"
wait "$pid"
code=$?
check "Ctrl+C stops cleanly (exit $code)" test "$code" -eq 0
check "final report after Ctrl+C" grep -q "Timing report" "$WORK/sigint.txt"

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all CLI checks passed"
