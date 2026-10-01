# Building, Testing and Debugging

Three ways to build:

1. **The complete software** with `make` (a wrapper around CMake).
2. **One module at a time** with `make module-<name>` / `make test-<name>`, or plain CMake targets.
3. **By hand with `g++`**, without any build system, to debug a single module or file.

---

## 1. Dependencies (any Linux distribution)

```bash
make install-deps        # = bash tools/install_deps.sh  — detects apt, dnf, pacman or zypper
make deps                # print the command for this system, install nothing
bash tools/install_deps.sh --no-camera   # without v4l2loopback (containers, CI)
bash tools/install_deps.sh --no-tests    # without GoogleTest / ffmpeg CLI / OpenCV
```

| Needed | Minimum | Debian / Ubuntu | Fedora | Arch | openSUSE |
|---|---|---|---|---|---|
| C++20 compiler | GCC 10 / Clang 12 | `build-essential` | `gcc-c++` | `base-devel` | `gcc-c++` |
| CMake, pkg-config | 3.20 | `cmake pkg-config` | `cmake pkgconf-pkg-config` | `cmake pkgconf` | `cmake pkg-config` |
| FFmpeg libraries | 4.4 | `libavformat-dev libavcodec-dev libavutil-dev libswscale-dev` | `pkgconfig(libavcodec)` … (ffmpeg-free, or RPM Fusion `ffmpeg-devel`) | `ffmpeg` | `pkgconfig(libavcodec)` … (Packman for H.264) |
| camera driver (run time) | — | `v4l2loopback-dkms v4l2loopback-utils` | `v4l2loopback` (RPM Fusion) | `v4l2loopback-dkms linux-headers` | `v4l2loopback-kmp-default` |
| tests (optional) | — | `libgtest-dev ffmpeg python3` | `gtest-devel ffmpeg-free python3` | `gtest python` | `googletest-devel ffmpeg python3` |

Supported: Linux on any 64-bit CPU (x86_64, arm64, riscv64). The CMake configure step checks the
operating system, compiler, 128-bit integer support, kernel headers and FFmpeg version, and stops with
a message that names the missing piece (`cmake/PlatformChecks.cmake`). Verified builds:
Ubuntu 22.04 (GCC 11, FFmpeg 4.4) and Ubuntu 24.04 (GCC 13 and Clang 18, FFmpeg 6.1); the CI workflow
(`.github/workflows/ci.yml`) also builds and tests on Debian 12, Fedora, Arch and openSUSE.

### Installing and packaging

```bash
make install             # /usr/local/bin: virtual-camera, vcam_probe, vcam-setup-loopback (sudo)
make uninstall           # remove exactly the files 'make install' copied (build/install_manifest.txt)
make package             # build/virtual-camera-engine-<ver>-Linux-<cpu>.tar.gz, plus .deb / .rpm
                         # when dpkg-deb / rpmbuild exist (cmake/Packaging.cmake)
make docker-build        # Docker image with the program (Dockerfile)
make docker-run FILE=/abs/path/video.mp4
```

A `.deb`/`.rpm` depends on the FFmpeg version of the distribution it was built on; build it on the
same distribution release you install it on.

## 2. The complete software

```bash
make                    # Release build into ./build ; binary: build/bin/virtual-camera
make test               # all unit + integration tests (no camera device needed)
make run FILE=~/Videos/clip.mp4 ARGS="--fps 30 --on-eof loop"
make run-pattern        # test pattern to /dev/video10
make debug test-debug   # AddressSanitizer + UBSan build and tests (./build-debug)
make tsan test-tsan     # ThreadSanitizer build and tests (./build-tsan)
make install            # install into /usr/local (sudo); make uninstall removes it
make help               # every target
```

With plain CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

| CMake option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `Debug`, `Release`, `RelWithDebInfo` |
| `VCAM_BUILD_TESTS` | `ON` | GoogleTest suites and integration tests |
| `VCAM_BUILD_TOOLS` | `ON` | the debugging tools in `tools/` |
| `VCAM_WARNINGS_AS_ERRORS` | `OFF` | add `-Werror` |
| `VCAM_ENABLE_ASAN` / `_UBSAN` / `_TSAN` | `OFF` | sanitizers |

Pass options through make: `make CMAKE_FLAGS="-DVCAM_WARNINGS_AS_ERRORS=ON"`.

---

## 3. Module by module

Every directory in `modules/` is a static library with its own include directory and its own tests. A
module can only include headers of modules it links, so the separation rules are checked by the compiler.

| Module | Library | Tests | Contains | Needs |
|---|---|---|---|---|
| `core` | `vcam_core` | `test_core` | Rational, Status/Result, PixelFormat, Frame types, logging, time | — |
| `convert` | `vcam_convert` | `test_convert` | FormatConverter (libswscale) | core, FFmpeg |
| `source` | `vcam_source` | `test_source` | VideoFile/ImageSequence/Pattern/Push sources, barcode | core, convert, FFmpeg |
| `buffer` | `vcam_buffer` | `test_buffer` | stream (decode-ahead window, default), RAM, disk-backed and live buffers, Timeline, memory budget | core |
| `timing` | `vcam_timing` | `test_timing` | clocks, absolute-deadline Scheduler, real-time thread setup, decoder thread priority | core |
| `metrics` | `vcam_metrics` | `test_metrics` | histogram, TimingAnalyzer, SPSC ring, reports | core |
| `resample` | `vcam_resample` | `test_resample` | SourceClock (exact slot → source time, pause/seek), Playhead: FPS mapping, EOF policies | core, buffer |
| `output` | `vcam_output` | `test_output` | V4L2 loopback, null and raw-file cameras | core |
| `config` | `vcam_config` | `test_config` | settings, YAML-subset file, command line | core, source, resample |
| `control` | `vcam_control` | `test_control` | state machine, commands, Engine, ControlLoop | all |

```bash
make module-timing      # build only libvcam_timing.a (+ core)
make test-timing        # build + run only the timing tests
make test-control       # engine end-to-end tests (fast mode, raw-file output)
```

With CMake / CTest directly:

```bash
cmake --build build --target vcam_resample test_resample
ctest --test-dir build -L resample --output-on-failure     # -L: tests with this label (module name)
ctest --test-dir build -R "Playhead.CaseD"                  # -R: tests whose name matches
ctest --test-dir build -L integration                       # the cross-module tests
```

Run one test binary directly (handy in a debugger):

```bash
VCAM_TEST_MEDIA_DIR=build/test_media ./build/bin/test_source --gtest_filter='*Push*'
gdb --args ./build-debug/bin/test_control --gtest_filter='*LoopAndHoldWithFrameLimit/stream'
```

The engine tests run in both buffer modes; the mode is part of the test name:
`BufferModes/EngineModeTest.<Test>/stream` and `…/ram`. Stream-only tests are `EngineTest.StreamMode*`;
the buffer itself is tested by `test_buffer --gtest_filter='StreamBuffer*'`, the clock arithmetic by
`test_resample --gtest_filter='SourceClock*'`, decoder seeking by `test_source --gtest_filter='*Seek*'`.

Test videos are generated, not stored: `make media` (CTest also generates them automatically).

---

## 4. Debugging tools (each exercises one module)

| Tool | Module(s) | Example | Shows |
|---|---|---|---|
| `probe_source` | source | `make probe-source FILE=clip.mp4 ARGS="--frames all --csv t.csv"` | metadata, per-frame timestamps, CFR/VFR, decode speed, RAM needed |
| `bench_scheduler` | timing, metrics | `make bench ARGS="--fps 60 --seconds 30"` | lateness, jitter, drift on this machine, no device |
| `resample_table` | resample | `make resample-table ARGS="--source-fps 24 --output-fps 30"` | which source frame each output slot shows |
| `vcam_probe` | output (consumer side) | `make probe-device ARGS="--frames 300 --barcode"` | what an application receives from /dev/videoN |
| `virtual-camera --output null` / `--output file` | everything except V4L2 | `virtual-camera --input clip.mp4 --output null` | full engine without a device |
| same, comparing buffer modes | buffer, source | `virtual-camera --input clip.mp4 --output null --duration 20 --buffer-mode ram` (vs default `stream`) | RAM (`/usr/bin/time -v`), underflow count, the `ahead N/M` window fill in the status line |
| `tools/vcam_client.py` | script input | `python3 tools/examples/push_frames.py` | push frames from Python |

---

## 5. Compiling by hand with g++

Run from the project root. `-g -O0` gives debug symbols without optimisation; use `-O2` for speed.
`pkg-config --cflags --libs <libs>` prints the compiler/linker flags of installed libraries.

Each module needs the source files of the modules it depends on (table in §3).

```bash
# core
g++ -std=c++20 -g -O0 -I modules/core/include \
    modules/core/src/*.cpp modules/core/tests/*.cpp -lgtest_main -lgtest -pthread -o /tmp/test_core

# buffer (core + buffer)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/buffer/include \
    modules/core/src/*.cpp modules/buffer/src/*.cpp modules/buffer/tests/*.cpp \
    -lgtest_main -lgtest -pthread -o /tmp/test_buffer

# timing (core + timing)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/timing/include \
    modules/core/src/*.cpp modules/timing/src/*.cpp modules/timing/tests/*.cpp \
    -lgtest_main -lgtest -pthread -o /tmp/test_timing

# metrics (core + metrics)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/metrics/include \
    modules/core/src/*.cpp modules/metrics/src/*.cpp modules/metrics/tests/*.cpp \
    -lgtest_main -lgtest -pthread -o /tmp/test_metrics

# resample (core + buffer + resample)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/buffer/include -I modules/resample/include \
    modules/core/src/*.cpp modules/buffer/src/*.cpp modules/resample/src/*.cpp modules/resample/tests/*.cpp \
    -lgtest_main -lgtest -pthread -o /tmp/test_resample

# output (core + output)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/output/include \
    modules/core/src/*.cpp modules/output/src/*.cpp modules/output/tests/*.cpp \
    -lgtest_main -lgtest -pthread -o /tmp/test_output

# convert (core + convert + FFmpeg)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/convert/include \
    modules/core/src/*.cpp modules/convert/src/*.cpp modules/convert/tests/*.cpp \
    $(pkg-config --cflags --libs libavutil libswscale) -lgtest_main -lgtest -pthread -o /tmp/test_convert

# the probe_source tool (core + convert + source + FFmpeg)
g++ -std=c++20 -g -O0 -I modules/core/include -I modules/convert/include -I modules/source/include \
    modules/core/src/*.cpp modules/convert/src/*.cpp modules/source/src/*.cpp tools/probe_source.cpp \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale) -pthread -o /tmp/probe_source

# the complete application (every module)
g++ -std=c++20 -O2 $(for m in modules/*/; do printf -- '-I %sinclude ' "$m"; done) \
    modules/*/src/*.cpp apps/virtual-camera/main.cpp \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale) -pthread -o /tmp/virtual-camera
```

Tests that need videos look for them in `$VCAM_TEST_MEDIA_DIR` (`bash tools/gen_test_media.sh /tmp/media`).

---

## 6. Finding problems

| Tool | Command | Finds |
|---|---|---|
| AddressSanitizer + UBSan | `make test-debug` | buffer overflows, use-after-free, leaks, undefined behaviour |
| ThreadSanitizer | `make test-tsan` | data races between the loader, pacer and control threads |
| Valgrind | `valgrind --leak-check=full ./build/bin/virtual-camera --input pattern --fast --max-frames 100 --output null` | leaks, uninitialised reads |
| gdb | `gdb --args ./build-debug/bin/virtual-camera --input clip.mp4 --output null` | step through code; threads are named `vcam-loader`, `vcam-pacer` |
| Logs | `virtual-camera … --verbose` (or `--log-level trace`) | state changes, conversion path, FFmpeg warnings, thread names |
| Timing CSV | `--timing-csv frames.csv` | one row per output frame for plots |

A note on folder names with spaces (e.g. `Virtual Camera Engine`): CMake and the Makefile handle them;
quote paths you type yourself.
