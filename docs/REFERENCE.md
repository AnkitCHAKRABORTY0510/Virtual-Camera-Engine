# Virtual Camera Engine — Full Reference

Everything the program can do, in detail. New users: start with the [User Guide](USER_GUIDE.md);
project overview: [README](../README.md).

A general-purpose **Linux virtual camera**. Give it a video file, a folder of images, a built-in test
pattern, or frames pushed by your own script, and every Linux application — OpenCV, FFmpeg, VLC, OBS,
Chrome, Firefox, Zoom — sees an ordinary webcam such as `/dev/video10`.

It behaves like a real camera **and measures that it does**: frames are released on an exact,
drift-free clock, and every run ends with a timing report (lateness, jitter, drift, dropped and
repeated frames).

```text
input ──► decode ──► buffer ──► READY ──► absolute-time scheduler ──► /dev/video10 ──► apps
                     (default: ~1 s read-ahead window, ~20–130 MB RAM for any video length)
```

Design: [docs/ARCHITECTURE.md](ARCHITECTURE.md) · Build and debugging: [docs/BUILD.md](BUILD.md) ·
Timing results: [docs/TIMING.md](TIMING.md) · Apps: [docs/COMPATIBILITY.md](COMPATIBILITY.md) ·
Script input: [docs/SCRIPT_INPUT.md](SCRIPT_INPUT.md) · Pixel formats: [docs/FORMATS.md](FORMATS.md)

---

## 1. Install dependencies

```bash
make install-deps        # = bash tools/install_deps.sh  (apt, dnf, pacman or zypper; asks for sudo)
make deps                # only print the command for this distribution
```

On Ubuntu/Debian this is:

```bash
sudo apt install -y build-essential cmake pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libgtest-dev ffmpeg v4l-utils v4l2loopback-dkms v4l2loopback-utils python3-opencv
```

Only FFmpeg's libraries and the v4l2loopback kernel module are needed at run time; GoogleTest, `ffmpeg` and
OpenCV are for the tests. Supported systems: [README → Supported systems](../README.md#supported-systems).

## 2. Build

```bash
make            # Release build into ./build  (binary: build/bin/virtual-camera)
make test       # 173 unit + integration tests, no camera device needed
```

Building single modules, running single tests and compiling files by hand with `g++`:
[docs/BUILD.md](BUILD.md).

## 3. Create the virtual camera device (once per boot)

```bash
sudo tools/setup_loopback.sh                       # creates /dev/video10 named "Virtual Camera Engine"
sudo tools/setup_loopback.sh --number 11 --name "Lab Camera"   # other number / name
sudo tools/setup_loopback.sh --remove              # remove it
```

The script loads `v4l2loopback` with `exclusive_caps=1`, which browsers need. **Persistent setup**
(load at every boot):

```bash
echo v4l2loopback | sudo tee /etc/modules-load.d/v4l2loopback.conf
echo 'options v4l2loopback devices=1 video_nr=10 card_label="Virtual Camera Engine" exclusive_caps=1 max_buffers=2' \
    | sudo tee /etc/modprobe.d/v4l2loopback.conf
```

## 4. Stream a video

```bash
./build/bin/virtual-camera --input ~/Videos/test.mp4
```

Example (1-minute 720p clip):

```text
Virtual Camera Engine
---------------------
Input       : ~/Videos/test.mp4
Output      : /dev/video10
[LOADING]   opening source and checking the output device...
            source : h264 1280x720 yuv420p, 30 fps, ~1800 frames, 1:00.000
            output : 1280x720 yuyv bt601/limited (1843200 bytes/frame) @ 30 fps -> /dev/video10 ('Virtual Camera Engine')
[BUFFERING] filling the read-ahead window (1.0 s); the rest is decoded while streaming
[READY]     read-ahead window: 32 frames, 56.3 MiB RAM in total (any video length)
[STREAMING] Virtual camera is live
            device : /dev/video10 ('Virtual Camera Engine', v4l2 loopback)
            format : yuyv 1280x720 @ 30 fps
STREAMING | 0:12.367 / 1:00.000 | sent 371 | 30.00 fps | late 0 | p99 0.31 ms | dropped 0 | loops 0 | ahead 31/32
```

Now select **"Virtual Camera Engine"** in any application. Stop with **Ctrl+C** (clean shutdown and a final
timing report) or by typing `stop`.

While it runs, type commands followed by Enter:

| Command | Effect |
|---|---|
| `pause` | freeze the picture; the camera keeps delivering the held frame at its frame rate |
| `resume` | continue exactly where it paused |
| `seek 120` | jump to 120 s in the source |
| `stats` | print the timing report so far |
| `stop` | shut down cleanly |

Other inputs:

```bash
./build/bin/virtual-camera --input frames/ --source-fps 25        # image sequence (natural sort: 2.png < 10.png)
./build/bin/virtual-camera --input pattern                         # test pattern with frame-number barcode
./build/bin/virtual-camera --source-type push --socket /tmp/vcam.sock   # frames from a script (docs/SCRIPT_INPUT.md)
```

## 5. Frame rate

```bash
--fps 30            # 30 frames per second
--fps 29.97         # = 30000/1001 exactly (also 23.976, 59.94, 119.88)
--fps 30000/1001    # explicit fraction
```

Without `--fps` the camera uses the source's own rate. The source rate and the camera rate are independent:

| Source → camera | What happens |
|---|---|
| 30 → 30 | every frame shown once, in order |
| 30 → 15 | every second source frame shown |
| 15 → 30 | each source frame shown twice (no invented in-between frames) |
| 24 → 30 | 1 frame in 4 shown twice (inherent judder) |
| variable rate | the real per-frame timestamps decide |

Each slot shows the source frame that would be on screen at that moment (`--selection hold`, default)
or the closest one (`--selection nearest`). `make resample-table ARGS="--source-fps 24 --output-fps 30"`
prints the exact mapping.

## 6. Resolution

The camera always has **the source's resolution**; nothing is scaled. Choose the pixel format with
`--pixel-format`:

| Format | Use |
|---|---|
| `yuyv` (default) | works with practically every webcam application, browser and video-call app |
| `i420`, `nv12` | 25 % less memory; fine for FFmpeg, OpenCV, browsers |
| `rgb24`, `bgr24`, `gray8` | OpenCV-style consumers; also required for odd frame sizes (e.g. 321×241) |

## 7. End of the input

| `--on-eof` | After the last frame |
|---|---|
| `loop` (default) | continues from frame 0; the camera's frame clock is not interrupted |
| `hold` | keeps showing the last frame; the camera stays live |
| `stop` | stops streaming and exits |

## 8. Memory and long videos

By default (`--buffer-mode stream`) the engine keeps only a **short window of decoded frames ahead of
the camera** (1 s by default) and decodes the rest while streaming. RAM use is fixed and small,
whatever the length of the video:

| Video | Stream (default) | `--buffer-mode ram` (whole video decoded first) |
|---|---|---|
| 800×410, 30 fps, 15 min | ~65 MB process, 20 MiB window | ~16.5 GiB |
| 1280×720, 30 fps, 1 min | ~110 MB process, 56 MiB window | ~3.1 GiB |
| 1920×1080, 30 fps, 15 min | ~210 MB process, 127 MiB window | ~104 GiB (use `disk`) |

Timing is not affected: the scheduler never waits for the decoder. The decoder runs on 2 threads at a
lower CPU priority (`nice` 10), so on a busy machine the frame clock always gets the CPU first; if the
decoder ever falls behind, the camera repeats the previous frame (counted as *Underflow repeats*) and
keeps its frame rate. Measured on a 2-core cloud VM: 30.000 fps with zero underflows, even with both
cores fully loaded by other programs (docs/TIMING.md §4.7).

```bash
--buffer-mode stream          # default: decode while streaming, low fixed RAM
--read-ahead 2                # seconds decoded ahead (default 1; more = more cushion, more RAM)
--decoder-threads 4           # default: 2 in stream mode (auto = all cores when preloading)
--buffer-mode ram             # decode everything first; budget = half of the free RAM (--memory-limit 8G)
--buffer-mode disk            # decode everything first into a temporary file in --spill-dir (default /var/tmp)
--pixel-format i420           # 25 % smaller than YUYV (any mode)
```

When to use `ram`/`disk`: very slow machines that cannot decode the video in real time (the status
line's `ahead` count keeps dropping and *Underflow repeats* grows), or videos whose decoding is
much more expensive than playing (e.g. 4K HEVC on an old CPU). Seeking in stream mode takes a few
milliseconds while the decoder jumps (the current picture is held meanwhile).

## 9. Inspect the virtual camera

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video10 --all            # format, resolution, frames per second
./build/bin/vcam_probe --device /dev/video10 --frames 300 --barcode   # what an application receives
```

## 10. Test with OpenCV

```python
import cv2
camera = cv2.VideoCapture("/dev/video10", cv2.CAP_V4L2)
print(camera.get(cv2.CAP_PROP_FRAME_WIDTH), camera.get(cv2.CAP_PROP_FRAME_HEIGHT), camera.get(cv2.CAP_PROP_FPS))
ok, frame = camera.read()        # a normal BGR image
```

or run `python3 tests/e2e/opencv_check.py /dev/video10`. To test every consumer at once:
`make device-test` (see [docs/COMPATIBILITY.md](COMPATIBILITY.md)).

## 11. Measure timing accuracy

Every run prints a report at the end (also `stats` while running):

```text
Timing report (real-time mode)            (10-minute run on the development VM, docs/TIMING.md)
  Requested FPS       : 30.000 (30)
  Published           : 18000 frames in 600.067 s (18000 slots)
  Lateness (A - T)    : mean 0.450 | median 0.404 | P95 0.597 | P99 1.300 | max 28.225 ms
  Jitter |I - dT|     : mean 0.139 | median 0.062 | P95 0.334 | P99 1.700 | max 27.841 ms
  Drift               : +0.110 ppm (slope of lateness; ~0 = no accumulating drift)
  Dropped (overrun)   : 3
```

* `--report-json report.json` — the report as JSON; `--timing-csv frames.csv` — one row per frame;
  `--stats-json stats.jsonl` — periodic statistics.
* `make bench ARGS="--fps 60 --seconds 60"` — scheduler accuracy on this machine without any device.
* `--rt-priority 10`, `--spin-us 200`, `--lock-memory` — tighter timing (see docs/TIMING.md).
* `--fast` — fast simulation mode: same frame selection, simulated clock, as fast as possible
  (needs `--on-eof stop`, `--duration` or `--max-frames`).

Definitions and measured results: [docs/TIMING.md](TIMING.md).

## 12. Configuration file

All options can live in a YAML file; command-line options override it:

```bash
./build/bin/virtual-camera --config config/examples/video_loop.yaml --input ~/Videos/clip.mp4
./build/bin/virtual-camera --help            # every option and every config key
```

Examples in [config/examples/](../config/examples/).

## Exit codes

`0` ok · `1` invalid options/config · `2` input problem · `3` camera device problem · `4` runtime error
(e.g. not enough memory).

## Troubleshooting

| Message | Fix |
|---|---|
| `camera device '/dev/video10' does not exist` | `sudo tools/setup_loopback.sh` |
| `no permission to open` | `sudo usermod -aG video $USER`, then log out and in |
| `is a real video device ... not a v4l2loopback device` | your webcam has that number; use `--device` with the loopback device |
| `does not accept output right now` | another program is already streaming into the device |
| `needs about … but only … is allowed` | use the default `--buffer-mode stream`, or `disk`, `--pixel-format i420`, `--memory-limit` |
| `Underflow repeats` keeps growing | the CPU cannot decode in real time: `--decoder-threads 4`, `--buffer-mode ram` or `disk` |
| `pixel format yuyv cannot represent 321x241` | use `--pixel-format rgb24` for odd frame sizes |
| browser does not show the camera | device must be created with `exclusive_caps=1` (the setup script does) |

## Project layout

```text
apps/virtual-camera/   the application (thin main)
modules/               core, convert, source, buffer, timing, metrics, resample, output, config, control
tools/                 probe_source, bench_scheduler, resample_table, vcam_probe, vcam_client.py, scripts
tests/integration/     app CLI, ffprobe comparison, Python push input
tests/e2e/             real-device and application tests
config/examples/       configuration files
docs/                  architecture, build, timing, formats, script input, compatibility
```

## License

MIT — see [LICENSE](../LICENSE).
