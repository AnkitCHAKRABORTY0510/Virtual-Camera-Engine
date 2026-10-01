#!/usr/bin/env bash
# =============================================================================
# compare_with_ffprobe.sh — Phase 1 acceptance test
#
# For each test video, compares the engine's decoded frames (via probe_source)
# with FFmpeg's own reference tool ffprobe:
#   * the number of frames must be identical
#   * every frame's timestamp (best_effort_timestamp, shifted so the first
#     frame is 0) must be identical, tick for tick
#
# Usage: compare_with_ffprobe.sh <path/to/probe_source> <media-directory>
# =============================================================================
set -euo pipefail

PROBE="${1:?usage: compare_with_ffprobe.sh <probe_source> <media-dir>}"
MEDIA="${2:?usage: compare_with_ffprobe.sh <probe_source> <media-dir>}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FILES=(cfr30.mp4 cfr15.mp4 cfr24.mp4 cfr60.mp4 ntsc2997.mp4 vfr.mkv hd709.mp4 odd.mkv cfr30.ts)
failures=0

for file in "${FILES[@]}"; do
    path="$MEDIA/$file"

    # Engine: column 2 of the CSV is pts_ticks (already normalised to start at 0).
    # rgb24 is used because it accepts every frame size (odd.mkv is 321x241).
    "$PROBE" --frames 0 --pixel-format rgb24 --csv "$WORK/ours.csv" "$path" > /dev/null
    tail -n +2 "$WORK/ours.csv" | cut -d, -f2 > "$WORK/ours.txt"

    # Reference: ffprobe's best_effort_timestamp per frame, shifted to start at 0.
    ffprobe -v error -select_streams v:0 -show_entries frame=best_effort_timestamp -of csv=p=0 "$path" \
        | awk 'NR == 1 { first = $1 } { print $1 - first }' > "$WORK/reference.txt"

    ours_count=$(wc -l < "$WORK/ours.txt")
    reference_count=$(wc -l < "$WORK/reference.txt")

    if [ "$ours_count" -ne "$reference_count" ]; then
        echo "FAIL $file: frame count $ours_count, ffprobe says $reference_count"
        failures=$((failures + 1))
    elif ! diff -q "$WORK/ours.txt" "$WORK/reference.txt" > /dev/null; then
        echo "FAIL $file: timestamps differ from ffprobe (first differences below)"
        diff "$WORK/ours.txt" "$WORK/reference.txt" | head -6
        failures=$((failures + 1))
    else
        echo "ok   $file: $ours_count frames, timestamps identical to ffprobe"
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "$failures file(s) differ from ffprobe"
    exit 1
fi
echo "all ${#FILES[@]} files match ffprobe"
