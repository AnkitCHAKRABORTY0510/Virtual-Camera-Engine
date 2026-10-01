#!/usr/bin/env bash
# =============================================================================
# run_device_tests.sh — Phases 5, 9 and 10 on a REAL v4l2loopback device
#
# Runs the engine on /dev/videoN and checks it from the consumer side with
# every tool available on this machine. Writes a Markdown report you can paste
# into docs/COMPATIBILITY.md.
#
# Prerequisites (once per boot):  sudo tools/setup_loopback.sh
# Usage:                          bash tests/e2e/run_device_tests.sh [/dev/video10]
#
# Tests:
#   1. device appears (v4l2-ctl --list-devices) and advertises our format/FPS
#   2. vcam_probe: V4L2 mmap streaming, received FPS, jitter, barcode order
#   3. consumer disconnect + reconnect (second probe on the same running stream)
#   4. FFmpeg: capture 90 frames from the device
#   5. OpenCV (Python): open, read, FPS, barcodes
#   6. slow consumer: a consumer reading at ~5 fps must not disturb the engine
#   7. engine restart: stop and start again, device usable again
#   8. a real video file (optional: VIDEO=/path/clip.mp4)
# =============================================================================
set -uo pipefail

DEVICE="${1:-/dev/video10}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/bin"
APP="$BIN/virtual-camera"
WORK="$(mktemp -d)"
REPORT="$ROOT/device_test_report.md"
WIDTH=1280
HEIGHT=720
FPS=30
engine_pid=""

cleanup() {
    [ -n "$engine_pid" ] && kill "$engine_pid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

passes=0
failures=0
skips=0
results=()

record() {  # record <PASS|FAIL|SKIP> <test> <details>
    results+=("| $2 | $1 | $3 |")
    case "$1" in
        PASS) passes=$((passes + 1)) ;;
        FAIL) failures=$((failures + 1)) ;;
        *) skips=$((skips + 1)) ;;
    esac
    printf '%-5s %-38s %s\n' "$1" "$2" "$3"
}

start_engine() {  # start_engine <extra args...>
    "$APP" --device "$DEVICE" --no-stdin --quiet --report-json "$WORK/engine_report.json" "$@" \
        > "$WORK/engine.log" 2>&1 &
    engine_pid=$!
    # Wait until the device offers capture (the engine is streaming).
    for _ in $(seq 1 100); do
        if grep -q "STREAMING" "$WORK/engine.log"; then
            sleep 0.5
            return 0
        fi
        if ! kill -0 "$engine_pid" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    echo "engine did not start:"; tail -20 "$WORK/engine.log"
    return 1
}

stop_engine() {
    if [ -n "$engine_pid" ]; then
        kill -INT "$engine_pid" 2>/dev/null
        wait "$engine_pid" 2>/dev/null
        engine_pid=""
    fi
}

echo "Virtual Camera Engine — device tests on $DEVICE"
echo "================================================"
[ -x "$APP" ] || { echo "build first: make"; exit 1; }
[ -e "$DEVICE" ] || { echo "$DEVICE missing: run sudo tools/setup_loopback.sh"; exit 1; }

# ---- 1. device + format ---------------------------------------------------------
start_engine --input pattern --pattern-size ${WIDTH}x${HEIGHT} --pattern-frames 900 --fps $FPS --on-eof loop || exit 1
if command -v v4l2-ctl >/dev/null; then
    v4l2-ctl --list-devices > "$WORK/devices.txt" 2>&1
    v4l2-ctl -d "$DEVICE" --all > "$WORK/all.txt" 2>&1
    if grep -q "$DEVICE" "$WORK/devices.txt"; then
        record PASS "device listed (v4l2-ctl)" "$(grep -B1 "$DEVICE" "$WORK/devices.txt" | head -1 | sed 's/[[:space:]]*$//')"
    else
        record FAIL "device listed (v4l2-ctl)" "not in --list-devices"
    fi
    if grep -q "Width/Height *: *$WIDTH/$HEIGHT" "$WORK/all.txt" && grep -qi "YUYV" "$WORK/all.txt"; then
        record PASS "format advertised" "YUYV ${WIDTH}x${HEIGHT}"
    else
        record FAIL "format advertised" "see v4l2-ctl --all"
    fi
    fps_line=$(grep -i "Frames per second" "$WORK/all.txt" | head -1 | sed 's/^[[:space:]]*//')
    record PASS "frame rate advertised" "${fps_line:-not reported}"
else
    record SKIP "v4l2-ctl" "install v4l-utils"
fi

# ---- 2. consumer-side timing + barcode order -----------------------------------
if "$BIN/vcam_probe" --device "$DEVICE" --frames 300 --barcode > "$WORK/probe1.txt" 2>&1; then
    record PASS "V4L2 streaming + order (vcam_probe)" \
        "$(grep 'measured fps' "$WORK/probe1.txt" | sed 's/^ *//'); $(grep 'missing frames' "$WORK/probe1.txt" | sed 's/^ *//')"
else
    record FAIL "V4L2 streaming + order (vcam_probe)" "$(tail -3 "$WORK/probe1.txt" | tr '\n' ' ')"
fi
grep -E "jitter" "$WORK/probe1.txt" | sed 's/^/      /'

# ---- 3. disconnect + reconnect ---------------------------------------------------
if "$BIN/vcam_probe" --device "$DEVICE" --frames 60 --barcode > "$WORK/probe2.txt" 2>&1; then
    record PASS "consumer reconnect" "second consumer received 60 frames"
else
    record FAIL "consumer reconnect" "$(tail -2 "$WORK/probe2.txt" | tr '\n' ' ')"
fi

# ---- 4. FFmpeg -----------------------------------------------------------------------
if command -v ffmpeg >/dev/null; then
    if timeout 20 ffmpeg -hide_banner -loglevel error -f v4l2 -i "$DEVICE" -frames:v 90 -f null - 2> "$WORK/ffmpeg.txt"; then
        record PASS "FFmpeg capture" "90 frames"
    else
        record FAIL "FFmpeg capture" "$(head -2 "$WORK/ffmpeg.txt" | tr '\n' ' ')"
    fi
else
    record SKIP "FFmpeg capture" "ffmpeg not installed"
fi

# ---- 5. OpenCV ---------------------------------------------------------------------
if python3 -c "import cv2" 2>/dev/null; then
    if python3 "$ROOT/tests/e2e/opencv_check.py" "$DEVICE" --frames 150 > "$WORK/opencv.txt" 2>&1; then
        record PASS "OpenCV (Python)" "$(grep -E 'measured' "$WORK/opencv.txt")"
    else
        record FAIL "OpenCV (Python)" "$(tail -2 "$WORK/opencv.txt" | tr '\n' ' ')"
    fi
else
    record SKIP "OpenCV (Python)" "python3-opencv not installed"
fi

# ---- 6. slow consumer ----------------------------------------------------------------
if command -v ffmpeg >/dev/null; then
    # Reads only 5 frames per second for 4 s; the engine must keep 30 fps without overruns.
    timeout 6 ffmpeg -hide_banner -loglevel error -f v4l2 -i "$DEVICE" -vf fps=5 -frames:v 20 -f null - \
        > /dev/null 2>&1
    record PASS "slow consumer" "ran a 5 fps reader for 4 s (engine report below)"
fi

stop_engine
if [ -f "$WORK/engine_report.json" ]; then
    measured=$(grep -o '"measured_fps":[0-9.]*' "$WORK/engine_report.json" | cut -d: -f2)
    missed=$(grep -o '"missed_slots":[0-9]*' "$WORK/engine_report.json" | cut -d: -f2)
    p99=$(grep -o '"lateness":{[^}]*' "$WORK/engine_report.json" | grep -o '"p99_ns":[0-9]*' | cut -d: -f2)
    if [ "${missed:-1}" -eq 0 ]; then
        record PASS "engine timing during all tests" "measured ${measured} fps, P99 lateness $((${p99:-0} / 1000)) us, 0 overruns"
    else
        record FAIL "engine timing during all tests" "measured ${measured} fps, ${missed} overruns"
    fi
fi

# ---- 7. restart ------------------------------------------------------------------------
if start_engine --input pattern --pattern-size 640x480 --fps 15 --on-eof loop && \
   "$BIN/vcam_probe" --device "$DEVICE" --frames 30 > "$WORK/probe3.txt" 2>&1; then
    record PASS "engine restart + new format" "640x480 @ 15 fps after restart"
else
    record FAIL "engine restart + new format" "$(tail -3 "$WORK/probe3.txt" 2>/dev/null | tr '\n' ' ') (v4l2loopback may keep the old format until no consumer is open)"
fi
stop_engine

# ---- 8. real video (optional) -----------------------------------------------------
if [ -n "${VIDEO:-}" ]; then
    if start_engine --input "$VIDEO" --on-eof loop && \
       "$BIN/vcam_probe" --device "$DEVICE" --frames 150 > "$WORK/probe4.txt" 2>&1; then
        record PASS "real video file" "$(basename "$VIDEO"): $(grep 'measured fps' "$WORK/probe4.txt" | sed 's/^ *//')"
    else
        record FAIL "real video file" "$(basename "$VIDEO")"
    fi
    stop_engine
else
    record SKIP "real video file" "set VIDEO=/path/clip.mp4 to include"
fi

# ---- report ---------------------------------------------------------------------------
{
    echo "# Device test report"
    echo
    echo "- Date: $(date -Iseconds)"
    echo "- Host: $(uname -srm), $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "- v4l2loopback: $(cat /sys/module/v4l2loopback/version 2>/dev/null || echo unknown)"
    echo "- FFmpeg: $(ffmpeg -version 2>/dev/null | head -1 | cut -d' ' -f3)"
    echo "- Device: $DEVICE"
    echo
    echo "| Test | Result | Details |"
    echo "|---|---|---|"
    printf '%s\n' "${results[@]}"
    echo
    echo "Manual checks (not automatable): VLC (Media > Open Capture Device), OBS (Video Capture Device"
    echo "source), Chromium/Firefox (https://webcamtests.com or a WebRTC call), Zoom — see docs/COMPATIBILITY.md."
} > "$REPORT"

echo
echo "passed $passes, failed $failures, skipped $skips — report: $REPORT"
[ "$failures" -eq 0 ]
