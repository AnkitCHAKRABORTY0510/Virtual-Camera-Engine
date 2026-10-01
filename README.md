<div align="center">

# 🎥 Virtual Camera Engine

**Turn a video file, a folder of images, a test pattern or your own script into a real Linux webcam.**

Google Meet, Zoom, Teams, OBS, VLC, Chrome, Firefox, FFmpeg and OpenCV see an ordinary camera —
with frame timing that is exact, drift-free and measured.

[![CI](../../actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-Linux-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)
![License](https://img.shields.io/badge/license-MIT-green)

[Quick start](#quick-start) ·
[User guide](docs/USER_GUIDE.md) ·
[Full reference](docs/REFERENCE.md) ·
[Architecture](docs/ARCHITECTURE.md) ·
[Contributing](CONTRIBUTING.md)

</div>

---

## Contents

- [What it does](#what-it-does)
- [Features](#features)
- [Supported systems](#supported-systems)
- [Quick start](#quick-start)
- [Using it in Google Meet, Zoom, OBS…](#using-it-in-google-meet-zoom-obs)
- [Everyday commands](#everyday-commands)
- [Performance](#performance)
- [Other ways to install](#other-ways-to-install)
- [Documentation](#documentation)
- [Project structure](#project-structure)
- [Development and tests](#development-and-tests)
- [Contributing](#contributing)
- [Author](#author)
- [License](#license)

---

## What it does

```text
 video.mp4 ─┐                                                                  ┌─► Google Meet / Zoom
 images/  ──┼─► decode ─► small read-ahead ─► exact frame clock ─► /dev/video10 ─┼─► OBS / VLC / Chrome
 pattern  ──┤             buffer (≈1 s)       (absolute deadlines)               └─► FFmpeg / OpenCV
 script   ──┘
```

You run one command; a camera named **"Virtual Camera Engine"** appears in every application and plays
your video at a steady frame rate — looping, pausing or seeking on command. At the end you get a
timing report that proves the frame rate was kept.

Typical uses: feeding recorded datasets (for example traffic-camera clips) into a vision pipeline as if
they were live cameras, repeatable demos and tests of video-call software, testing OpenCV / FFmpeg
applications without a physical camera.

## Features

- **Any input** — video files (everything FFmpeg decodes: MP4, MKV, TS, AVI, MOV; H.264, H.265, VP9, AV1…),
  image folders, a built-in test pattern with frame-number barcodes, or frames pushed from Python.
- **Same resolution in and out** — nothing is scaled; pixel format chosen for compatibility (YUYV by default).
- **Exact timing** — absolute-deadline scheduler, integer time arithmetic, no drift over hours;
  every frame's lateness is measured and reported.
- **Any frame rate** — 30 → 15, 15 → 30, 24 → 30, 29.97 (exactly 30000/1001), variable-rate videos.
- **Low memory by default** — only ~1 s of decoded frames is kept; a 15-minute video uses the same
  ~65 MB as a 1-minute one.
- **Live control** — `pause`, `resume`, `seek 90`, `stats`, `stop` while it runs; loop / hold / stop at the end.
- **Scriptable** — command-line options, YAML config files, JSON/CSV timing output, push frames from your own code.
- **Clear errors** — checks the device, memory and input before streaming and tells you how to fix problems.
- **Modular C++20** — ten small libraries, each buildable and testable on its own; 173 automated tests.

## Supported systems

The camera is created by **v4l2loopback**, a Linux kernel driver, so the engine runs on **Linux**.

| Distribution | Versions | Status |
|---|---|---|
| Ubuntu / Linux Mint / Pop!_OS | 22.04 or newer | ✅ built and tested (CI) |
| Debian / Raspberry Pi OS **64-bit** | 12 (bookworm) or newer | ✅ built and tested (CI) |
| Fedora | 39 or newer | ✅ built and tested (CI) — camera driver from RPM Fusion |
| Arch / Manjaro / EndeavourOS | rolling | ✅ built and tested (CI) |
| openSUSE Tumbleweed | rolling | ⚠️ experimental (CI, allowed to fail) |
| Other Linux | — | works if you have the [requirements](#requirements) |
| macOS, Windows | — | ❌ no v4l2loopback (use a Linux virtual machine) |

CPU: any **64-bit** processor — x86_64 (Intel/AMD), arm64/aarch64 (Raspberry Pi 4/5, Jetson, ARM servers),
riscv64. 32-bit systems are not supported.

### Requirements

| What | Minimum | Installed by `make install-deps` |
|---|---|---|
| C++ compiler | GCC 10 or Clang 12 | ✅ |
| CMake, make, pkg-config | CMake 3.20 | ✅ |
| FFmpeg libraries (libavformat, libavcodec, libavutil, libswscale) | 4.4 | ✅ |
| v4l2loopback kernel module | any recent | ✅ (Fedora: RPM Fusion) |
| GoogleTest, `ffmpeg` command, Python 3 | — | ✅ only needed for tests |

`cmake` checks all of this first and prints exactly what is missing and how to install it.

---

## Quick start

```bash
# 1. Get the code
git clone <this-repository-url> virtual-camera-engine
cd virtual-camera-engine

# 2. Install dependencies — detects apt, dnf, pacman or zypper (asks for your password)
make install-deps

# 3. Build
make

# 4. Create the camera /dev/video10 (once after every reboot)
make setup-loopback

# 5. Play a video as the camera
make run FILE="$HOME/Videos/my_video.mp4"
```

When the terminal shows **`[STREAMING] Virtual camera is live`**, the camera is ready:

```text
[LOADING]   opening source and checking the output device...
            source : h264 1280x720 yuv420p, 30 fps, ~1800 frames, 1:00.000
            output : 1280x720 yuyv bt601/limited (1843200 bytes/frame) @ 30 fps -> /dev/video10 ('Virtual Camera Engine')
[BUFFERING] filling the read-ahead window (1.0 s); the rest is decoded while streaming
[READY]     read-ahead window: 32 frames, 56.3 MiB RAM in total (any video length)
[STREAMING] Virtual camera is live
STREAMING | 0:12.367 / 1:00.000 | sent 371 | 30.00 fps | late 0 | p99 0.31 ms | dropped 0 | loops 0 | ahead 31/32
```

Step-by-step instructions for beginners: **[User Guide](docs/USER_GUIDE.md)**.

## Using it in Google Meet, Zoom, OBS…

1. Start the camera first (`make run FILE=...`). If the app was already open, reload it.
2. In the app's camera settings choose **Virtual Camera Engine**
   (Google Meet: **⋮ → Settings → Video → Camera**).
3. Keep the terminal open — the camera exists while the program runs. Stop with **Ctrl + C** or `stop`.

Tested consumers and their quirks: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Everyday commands

| Goal | Command |
|---|---|
| play a video, looping forever | `make run FILE=video.mp4` |
| play once, then stop | `make run FILE=video.mp4 ARGS="--on-eof stop"` |
| keep the last frame at the end | `make run FILE=video.mp4 ARGS="--on-eof hold"` |
| force 30 fps (or 29.97, 60, 30000/1001…) | `make run FILE=video.mp4 ARGS="--fps 30"` |
| play a folder of images at 25 fps | `make run FILE=frames/ ARGS="--source-fps 25"` |
| test pattern (no video needed) | `make run-pattern` |
| frames from your own Python script | `make run-push` + [docs/SCRIPT_INPUT.md](docs/SCRIPT_INPUT.md) |
| another camera number | `make run FILE=video.mp4 DEVICE=/dev/video11` |
| use a config file | `./build/bin/virtual-camera --config config/examples/video_loop.yaml --input video.mp4` |
| every option | `./build/bin/virtual-camera --help` |

While it runs, type `pause`, `resume`, `seek 120`, `stats` or `stop` and press Enter.

## Performance

Measured on a 2-core cloud VM (default settings, H.264 input, 30 fps):

| Video | Memory | CPU | Frame rate | Dropped frames |
|---|---|---|---|---|
| 800×410 | 65 MB | 5 % | 30.000 fps | 0 |
| 1280×720 | 111 MB | 22 % | 30.000 fps | 0 |
| 1920×1080 | 207 MB | 41 % | 30.000 fps | 0 |
| 800×410 with both CPU cores 100 % busy | 65 MB | — | 30.000 fps | 0 |

Over a 10-minute run the frame clock drifted by **0.11 ppm**; typical frame lateness is 0.1–0.5 ms.
Details and methodology: [docs/TIMING.md](docs/TIMING.md).

---

## Other ways to install

### System-wide install

```bash
make install        # installs virtual-camera, vcam_probe, vcam-setup-loopback into /usr/local
make uninstall      # removes them again
```

### Packages (.deb / .rpm / .tar.gz)

```bash
make package        # builds packages for THIS distribution into build/
sudo apt install ./build/virtual-camera-engine_1.1.0_amd64.deb      # Debian / Ubuntu
sudo dnf install ./build/virtual-camera-engine-1.1.0-1.x86_64.rpm   # Fedora / openSUSE (needs rpmbuild)
```

A package lists its FFmpeg libraries as dependencies, so the package manager installs them.
Build the package on the same distribution (and version) you install it on.

### Docker (no compiler or FFmpeg on the host)

```bash
sudo tools/setup_loopback.sh                         # the camera driver must be loaded on the HOST
make docker-build
make docker-run FILE=/absolute/path/to/video.mp4     # = docker run --device /dev/video10 ...
```

Containers share the host's kernel, so the v4l2loopback module is always loaded on the host.

---

## Documentation

| Document | For |
|---|---|
| [User Guide](docs/USER_GUIDE.md) | first-time users: install, run, use in Meet, troubleshoot |
| [Full Reference](docs/REFERENCE.md) | every option, end-of-file policies, frame-rate rules, timing report, error messages |
| [Build & Debug](docs/BUILD.md) | building single modules, running single tests, compiling by hand, sanitizers |
| [Architecture](docs/ARCHITECTURE.md) | design: threads, buffers, timing, state machine, decisions |
| [Timing](docs/TIMING.md) | how timing is measured, results |
| [Formats](docs/FORMATS.md) | pixel formats and colour handling |
| [Script Input](docs/SCRIPT_INPUT.md) | pushing frames from Python or any language |
| [Compatibility](docs/COMPATIBILITY.md) | browsers, video-call apps, OBS, OpenCV, FFmpeg |
| [Changelog](CHANGELOG.md) | what changed in each version |

## Project structure

```text
virtual-camera-engine/
├── apps/virtual-camera/     the program (a thin main)
├── modules/                 ten independent C++ libraries
│   ├── core/                rational numbers, status codes, pixel formats, frames, logging
│   ├── convert/             pixel-format conversion (libswscale, no scaling)
│   ├── source/              video files, image folders, test pattern, script input
│   ├── buffer/              read-ahead stream buffer, RAM / disk / live buffers
│   ├── timing/              clocks and the absolute-deadline scheduler
│   ├── metrics/             timing statistics and reports
│   ├── resample/            frame-rate conversion, pause / seek / loop
│   ├── output/              v4l2loopback camera, null and file outputs
│   ├── config/              options, config files, command line
│   └── control/             state machine, engine, terminal control loop
├── tools/                   setup and debugging tools, dependency installer
├── tests/                   integration and real-device tests
├── config/examples/         ready-made configuration files
├── docs/                    documentation
├── cmake/                   build helpers, platform checks, packaging
├── Makefile                 short commands (make help)
└── Dockerfile               container build
```

Each module has `include/`, `src/` and `tests/`, and can only use the modules it links — the
compiler enforces the architecture.

## Development and tests

```bash
make test               # all 173 unit + integration tests (no camera device needed)
make test-timing        # one module's tests (any module: core, source, buffer, ...)
make module-source      # build one module only
make test-debug         # AddressSanitizer + UndefinedBehaviorSanitizer
make test-tsan          # ThreadSanitizer (data races)
make device-test        # tests against a real /dev/video10 (after make setup-loopback)
make help               # every target
```

Building and debugging module by module, including plain `g++` commands: [docs/BUILD.md](docs/BUILD.md).

## Contributing

Contributions are welcome — bug reports, documentation, new input sources, tested distributions.
Please read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md).
Security issues: see [SECURITY.md](SECURITY.md).

## Author

**Ankit Chakraborty** — design, implementation and maintenance. See [AUTHORS.md](AUTHORS.md).

## License

[MIT](LICENSE) © 2026 Ankit Chakraborty
