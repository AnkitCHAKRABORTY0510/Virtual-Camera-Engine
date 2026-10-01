# Pixel Formats and Colour

How the engine lays out frames in memory, and how colours are handled.
Source of truth in code: `modules/core/src/frame.cpp` (`make_frame_format`) and
`modules/convert/src/format_converter.cpp`.

## Resolution rule

The output frame always has **the same width and height as the source** (architecture v0.2).
Nothing is scaled. If the chosen pixel format cannot represent the source size (for example an odd
width with YUYV), the engine refuses with an error that suggests a format that can.

## Common properties

| Property | Value |
|---|---|
| Bit depth | 8 bits per sample for every format |
| Row stride | tightly packed: `stride = visible bytes per row` (no row padding) |
| Plane order | planes stored back to back in one block, in the order listed below |
| Alignment | engine-owned frame memory (`OwnedFrame`) starts on a 64-byte boundary |
| Byte order | as listed; byte 0 is the top-left pixel |

## Layouts (width W, height H)

| Format | CLI name | Planes (offset → size) | Bytes/frame | Even size needed | V4L2 fourcc |
|---|---|---|---|---|---|
| YUYV 4:2:2 | `yuyv` (alias `yuy2`) | 0 → `2W × H`, bytes `Y0 U Y1 V` | `2WH` | width | `YUYV` |
| UYVY 4:2:2 | `uyvy` | 0 → `2W × H`, bytes `U Y0 V Y1` | `2WH` | width | `UYVY` |
| I420 4:2:0 | `i420` (aliases `yu12`, `yuv420p`) | Y: 0 → `W × H`; U: `WH` → `W/2 × H/2`; V: `WH + WH/4` → `W/2 × H/2` | `1.5WH` | width and height | `YU12` |
| NV12 4:2:0 | `nv12` | Y: 0 → `W × H`; UV interleaved: `WH` → `W × H/2` | `1.5WH` | width and height | `NV12` |
| RGB24 | `rgb24` (alias `rgb`) | 0 → `3W × H`, bytes `R G B` | `3WH` | — | `RGB3` |
| BGR24 | `bgr24` (alias `bgr`) | 0 → `3W × H`, bytes `B G R` (OpenCV's order) | `3WH` | — | `BGR3` |
| GRAY8 | `gray8` (alias `gray`) | 0 → `W × H`, luma only | `WH` | — | `GREY` |

Example sizes for 1280×720: YUYV 1 843 200 B, I420 1 382 400 B, RGB24 2 764 800 B.

## Colour

| Output | Matrix | Range |
|---|---|---|
| YUV formats (YUYV, UYVY, I420, NV12) | BT.601 | limited (Y 16–235, UV 16–240) |
| RGB24, BGR24, GRAY8 | — | full (0–255) |

BT.601 limited range is the conventional webcam signalling that consumers (browsers, OpenCV, FFmpeg)
assume for YUV camera frames.

How the source's colours are read:

| Source metadata | Interpreted as |
|---|---|
| `colorspace = bt709` | BT.709 |
| `bt470bg`, `smpte170m`, `fcc` | BT.601 |
| unspecified / other | BT.709 if height ≥ 720, else BT.601 (common convention; logged at DEBUG as "matrix guessed") |
| `color_range = pc/jpeg`, or `yuvj*` pixel formats | full range |
| otherwise | limited range |

Conversion paths:

* **Copy**: source pixel format, matrix and range already equal the output → planes are copied
  row by row (fastest, bit-exact).
* **libswscale**: everything else (pixel format and/or matrix/range differ). Same width and height,
  so only chroma resampling and colour maths happen. Accuracy is verified to ±2 levels in
  `test_format_converter.cpp` (e.g. BT.709 red `Y63/Cb102/Cr240` → BT.601 `Y81/Cb90/Cr240`).

## Measured conversion speed (Phase 1, 2-core cloud VM, 1080p source, single thread)

| Output | Frames/s (decode + convert) |
|---|---|
| YUYV (BT.709 → BT.601) | ~110–120 |
| I420 (BT.709 → BT.601) | ~170 |
| decode only (FFmpeg) | ~480 |

Conversion, not decoding, is the bottleneck. It does not affect real-time output (frames are
converted once while buffering), but it sets the loading time. Parallel conversion in the loader was
proposed but not implemented in v1.0 (docs/ARCHITECTURE.md §16, A3).

## V4L2 signalling

`VIDIOC_S_FMT` declares, for YUV formats, `colorspace = SMPTE170M`, `ycbcr_enc = 601`,
`quantization = LIM_RANGE`; for RGB/grey `colorspace = SRGB`, full range. `bytesperline` equals the
first plane's stride and `sizeimage` the full frame size, so planar formats are contiguous as above.
