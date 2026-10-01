#!/usr/bin/env bash
# =============================================================================
# gen_test_media.sh — generate the test videos used by the test suite
#
# Usage:
#   bash tools/gen_test_media.sh <output-directory>
#
# Uses the ffmpeg command-line tool and its built-in test sources (lavfi), so
# no video files need to be committed to the repository. Generation is skipped
# when the files already exist (a stamp file records the script version).
#
# Files produced (all short, a few hundred KB in total):
#   cfr30.mp4        320x240, 30 fps,     90 frames  (H.264 if available)
#   cfr15.mp4        320x240, 15 fps,     45 frames
#   cfr24.mp4        320x240, 24 fps,     72 frames
#   cfr60.mp4        320x240, 60 fps,    180 frames
#   ntsc2997.mp4     320x240, 30000/1001, 90 frames
#   vfr.mkv          320x240, 30 frames at 30 fps then 30 frames at 15 fps (60 frames)
#   hd709.mp4        1280x720, 30 fps, 15 frames, tagged BT.709
#   odd.mkv          321x241 (odd size), 10 frames, lossless FFV1 yuv444p
#   cfr30.ts         same content as cfr30.mp4 in MPEG-TS (robust to truncation)
#   truncated.ts     cfr30.ts cut at 50 %
#   corrupt.ts       cfr30.ts with 8 KiB overwritten in the middle
#   audio_only.m4a   1 s of sine-wave audio, no video
#   not_a_video.txt  plain text
#   images/          12 PNG frames 001.png .. 012.png (320x240)
#   images_mixed/    3 PNGs of which one has a different size
# =============================================================================
set -euo pipefail

OUT_DIR="${1:?usage: gen_test_media.sh <output-directory>}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"   # absolute, so it stays valid after the cd below
VERSION="6"   # bump when the set of files changes, to force regeneration
STAMP="$OUT_DIR/.generated-v$VERSION"

if [ -f "$STAMP" ]; then
    echo "test media up to date in $OUT_DIR"
    exit 0
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ERROR: ffmpeg command not found (install it: bash tools/install_deps.sh)" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

# Prefer H.264 (what real .mp4 files contain); fall back to MPEG-4 Part 2,
# which every FFmpeg build has.
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' libx264 '; then
    VIDEO_CODEC=(-c:v libx264 -preset veryfast -pix_fmt yuv420p -g 30)
else
    VIDEO_CODEC=(-c:v mpeg4 -q:v 4 -pix_fmt yuv420p -g 30 -bf 2)   # -bf 2: B-frames, so decode order != display order
fi

# Older FFmpeg (< 5.1) uses -vsync instead of -fps_mode.
if ffmpeg -hide_banner -h full 2>/dev/null | grep -q -- '-fps_mode'; then
    PASSTHROUGH=(-fps_mode passthrough)
else
    PASSTHROUGH=(-vsync passthrough)
fi

# Quiet, overwrite, no audio unless asked.
FF=(ffmpeg -hide_banner -loglevel error -y)

# make_cfr <file> <rate> <frames> [extra args...]
make_cfr() {
    local file="$1" rate="$2" frames="$3"
    shift 3
    "${FF[@]}" -f lavfi -i "testsrc2=size=320x240:rate=$rate" -frames:v "$frames" \
        "${VIDEO_CODEC[@]}" "$@" "$file"
}

echo "generating test media in $OUT_DIR ..."

make_cfr cfr30.mp4 30 90
make_cfr cfr15.mp4 15 45
make_cfr cfr24.mp4 24 72
make_cfr cfr60.mp4 60 180
make_cfr ntsc2997.mp4 30000/1001 90

# Variable frame rate: setpts rewrites timestamps. Frames 0..29 are 1/30 s
# apart, frames 30..59 are 1/15 s apart. passthrough keeps them unchanged.
"${FF[@]}" -f lavfi -i "testsrc2=size=320x240:rate=30" -frames:v 60 \
    -vf "setpts='if(lt(N,30),N/30,1+(N-30)/15)/TB'" "${PASSTHROUGH[@]}" \
    "${VIDEO_CODEC[@]}" vfr.mkv

# HD with explicit BT.709 colour tags.
"${FF[@]}" -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 15 \
    "${VIDEO_CODEC[@]}" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv hd709.mp4

# Odd frame size: needs a format without chroma subsampling (yuv444p) and a
# codec that accepts odd sizes (FFV1, lossless). testsrc2 only makes even
# sizes, so the picture is scaled to 321x241.
"${FF[@]}" -f lavfi -i "testsrc2=size=320x240:rate=25" -frames:v 10 \
    -vf "scale=321:241,format=yuv444p" -c:v ffv1 odd.mkv

# MPEG-TS for damage tests (TS survives truncation, unlike MP4 whose index may
# be at the end). Encoded directly, not stream-copied from the MP4: a stream
# copy of MPEG-4 Part 2 would lose the in-band header that holds the frame size.
make_cfr cfr30.ts 30 90 -f mpegts
size=$(stat -c %s cfr30.ts)
head -c $((size / 2)) cfr30.ts > truncated.ts
cp cfr30.ts corrupt.ts
# Overwrite 8 KiB in the middle with 0xFF bytes (deterministic damage).
head -c 8192 /dev/zero | tr '\0' '\377' | dd of=corrupt.ts bs=1 seek=$((size / 2)) conv=notrunc status=none

"${FF[@]}" -f lavfi -i "sine=frequency=440:duration=1" -c:a aac audio_only.m4a
echo "this is not a video" > not_a_video.txt

# Image sequences: the %03d pattern numbers the files 001.png, 002.png, ...
mkdir -p images images_mixed
"${FF[@]}" -f lavfi -i "testsrc2=size=320x240:rate=30" -frames:v 12 images/%03d.png
"${FF[@]}" -f lavfi -i "testsrc2=size=320x240:rate=30" -frames:v 2 images_mixed/%03d.png
"${FF[@]}" -f lavfi -i "testsrc2=size=160x120:rate=30" -frames:v 1 images_mixed/003.png

rm -f .generated-v*
touch "$STAMP"
echo "done: $(find . -type f | wc -l) files"
