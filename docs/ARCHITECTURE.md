# Virtual Camera Engine — Architecture

Status: **v1.1 — all phases implemented (see §15 Phase 1 results, §16 implementation notes). Since v1.1 the
default buffer is the low-memory stream mode (§16.4).**

### Revision history

| Version | Date | Change |
|---|---|---|
| v1.1 | 2026-10-01 | **Default buffer mode changed from `ram` to `stream`** (decode-ahead window, fixed small RAM). Deviation A2 revised, A9 added; design in §16.4. `ram`/`disk` unchanged and still available. |
| v1.0 | 2026-10-01 | Phases 2–10 implemented. Deviations from this proposal are listed and justified in §16. |
| v0.3 | 2026-10-01 | Phase 1 implemented; results and findings in §15. |
| v0.2 | 2026-10-01 | **Output resolution = input resolution (no scaling).** Decision D2 replaced by this rule; `--width/--height` removed from the CLI design; `convert` now does pixel-format and colour conversion only. Remaining decisions D1, D3–D6 proceed with the proposed defaults (can still be changed). |
| v0.1 | 2026-10-01 | Initial proposal |

Scope: general-purpose Linux virtual camera. Independent of any research workload.

This document records the design of the engine and the decisions behind it. Sections 1–14 are the
original design (written before implementation, kept for the reasoning they contain); §15 and §16 record
what was built, measured and where the implementation differs from the original design.

| § | Topic |
|---|---|
| 1 | Architecture overview and module responsibilities |
| 2 | Dependencies and alternatives |
| 3 | Linux virtual-camera mechanism (v4l2loopback) |
| 4 | Thread model |
| 5 | Buffer architecture |
| 6 | Timing architecture |
| 7 | Resampling (frame-rate conversion, pause, seek, loop) |
| 8–9 | State machine, error handling |
| 10 | Directory structure and build |
| 11–14 | Development phases, risks, initial plan and decisions |
| 15–16 | Implementation results, deviations, stream mode |

---

## 1. Proposed Architecture

### 1.1 Pipeline

```text
                 ┌──────────────────────────── control plane ─────────────────────────────┐
                 │  CLI / config  →  Controller (state machine)  ←  stdin cmds / signals    │
                 └──────┬───────────────────────┬───────────────────────────┬─────────────┘
                        │ open/seek             │ start/pause/resume/stop   │ stats
                        v                       v                           v
  ┌──────────────┐   ┌───────────────┐   ┌─────────────┐   ┌───────────┐   ┌───────────────┐   ┌──────────────┐
  │ FrameSource  │──>│FormatConverter│──>│ FrameBuffer │──>│ Playhead/ │──>│   Scheduler   │──>│VirtualCamera │──> /dev/videoN
  │ (decode)     │   │ (normalize)   │   │ + Timeline  │   │ Resampler │   │ (pacer loop)  │   │ (V4L2 backend)│
  └──────────────┘   └───────────────┘   └─────────────┘   └───────────┘   └──────┬────────┘   └──────────────┘
     loader thread ───────────────────────────────┘          pure logic           │ pacer thread
                                                                                  v
                                                                          ┌───────────────┐
                                                                          │    Metrics    │──> terminal / CSV / JSON
                                                                          └───────────────┘
```

### 1.2 Responsibilities (the separation rule from §42 of your brief)

| Module | Owns | Must NOT know about |
|---|---|---|
| `core` | `Frame`, `FrameFormat`, `PixelFormat`, `Rational`, time types, `Status`, logging | everything else |
| `source` | *how frames are obtained*: open, probe metadata, decode in order, timestamps | output device, output timing, buffer type |
| `convert` | pixel-format and colour-matrix conversion at the *same* resolution (libswscale wrapper); never scales | where frames come from or go to |
| `buffer` | *where decoded frames live*: storage, random access, statistics | codecs, devices, timing |
| `resample` | *which source frame* belongs to output slot `n` (pure math over timestamps) | pixels, clocks, devices |
| `timing` | *when* output happens: absolute-deadline loop, clocks | file formats, V4L2 |
| `output` | *how* frames reach Linux (`/dev/videoN`) | whether the source was MP4, PNG or Python |
| `metrics` | timing/error statistics, histograms, reports | everything except `core` |
| `control` | state machine, commands, wiring modules together (`Engine`) | codec or device internals |
| `config` | defaults → config file → CLI precedence, validation | runtime behaviour |

The rule is **enforced by the build**, not just by convention: each module is its own CMake library with its
own include directory, and a module can only include headers of modules it links. If `source` ever tries
to `#include <vcam/output/...>`, compilation fails.

### 1.3 Two time domains (your §30)

The engine keeps two clocks strictly apart:

| Domain | Symbol | Unit / representation | Who uses it |
|---|---|---|---|
| Logical source time | `t_src` | integer ticks of the source's rational time base (e.g. 1/15360 s) | Source, Timeline, Resampler |
| Output wall-clock time | `t_out` | `int64` nanoseconds of `CLOCK_MONOTONIC` | Scheduler, Metrics |

They are linked only by the **Playhead** (§7), which maps output slot index `n` → source time.
Nothing in the system ever assumes "the file says 30 FPS, therefore frames arrived every 33.333 ms".

### 1.4 Core data types (sketch — names may be refined in Phase 1)

```cpp
namespace vcam {

// Exact rational number, used for frame rates (30000/1001) and time bases (1/15360).
struct Rational { int64_t num; int64_t den; };

enum class PixelFormat { YUYV, UYVY, I420, NV12, RGB24, BGR24, GRAY8 };
enum class ColorSpace  { BT601, BT709 };
enum class ColorRange  { Limited, Full };

struct PlaneLayout { size_t offset; size_t stride_bytes; size_t rows; };

// Everything needed to interpret a block of memory as an image.
struct FrameFormat {
    int width = 0, height = 0;
    PixelFormat pixel_format;
    ColorSpace  color_space  = ColorSpace::BT601;
    ColorRange  color_range  = ColorRange::Limited;
    int         plane_count  = 1;
    std::array<PlaneLayout, 3> planes{};
    size_t      size_bytes   = 0;     // total bytes of one frame
};

// Timing information of one source frame (stored in the Timeline, no pixels).
struct FrameTiming {
    uint64_t source_index;   // 0, 1, 2 ... in decode/presentation order
    int64_t  pts_ticks;      // presentation time in source time-base ticks (first frame normalised to 0)
    int64_t  duration_ticks; // display duration, 0 if unknown
};

// Non-owning views: cheap to pass around, never copy pixels.
struct FrameView        { const FrameFormat* format; std::span<const uint8_t> bytes; FrameTiming timing; };
struct MutableFrameView { const FrameFormat* format; std::span<uint8_t>       bytes; };

} // namespace vcam
```

Deliberate change from your sketch: **timestamps are integers, not `double`**. A `double` is fine for
hours of video, but integer ticks + rational time base make comparisons *exact*, so a 30 → 30 FPS
mapping is guaranteed to be `n → n` with no occasional off-by-one from rounding, and tests can assert
exact equality.

### 1.5 Source abstraction

Two kinds of sources behave fundamentally differently, so the interface exposes capabilities instead of
pretending they are the same:

| | Finite / seekable (file, image sequence) | Live (script, network, generator) |
|---|---|---|
| Frame count known | usually | no |
| Can preload fully | yes | no |
| Can seek | yes | no |
| Who decides frame order | file timestamps | arrival order |

```cpp
struct SourceInfo {
    int width, height;                     // native resolution
    std::string codec_name, pixel_format_name;
    Rational time_base;                    // unit of pts_ticks
    std::optional<Rational> nominal_fps;   // avg frame rate if known
    std::optional<uint64_t> frame_count;   // if known up front
    std::optional<int64_t>  duration_ticks;
    bool is_live = false, is_seekable = false;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual Status            open() = 0;                                  // validate + probe metadata
    virtual const SourceInfo& info() const = 0;
    virtual Status            set_output_format(const FrameFormat& fmt) = 0; // same w×h as the source
    // Decode the next frame, converting it straight into `dst` (memory owned by the buffer).
    // Returns Frame, EndOfStream, or Error. No pixel copy beyond the unavoidable conversion.
    virtual ReadResult        read_next(MutableFrameView dst, FrameTiming& timing_out) = 0;
    virtual Status            seek(int64_t pts_ticks);                     // default: Unsupported
    virtual SourceStats       stats() const = 0;
    virtual void              close() = 0;
};
```

The source decodes into its own native format; internally it uses a `FormatConverter` (from the separate
`convert` module, linked privately) that writes the normalised result **directly into the buffer slot**.
The rule "output size = source size" lives in one function, `make_output_format(SourceInfo, PixelFormat)`. That gives "decode → buffer → output" with one user-space write per
frame (the conversion, which is needed anyway) and no extra copies.

---

## 2. Dependencies

### 2.1 Minimum required set (Version 1)

| Need | Choice | Ubuntu/Debian package |
|---|---|---|
| Compiler | GCC ≥ 11 or Clang ≥ 14, C++20 | `build-essential` |
| Build | CMake ≥ 3.20, plus a convenience `Makefile` | `cmake`, `make`, `pkg-config` |
| Demux/decode | FFmpeg libraries (C API) | `libavformat-dev libavcodec-dev libavutil-dev` |
| Pixel-format / colour convert | libswscale (part of FFmpeg) | `libswscale-dev` |
| Virtual camera | V4L2 kernel headers + **v4l2loopback** module | `linux-libc-dev` (headers), `v4l2loopback-dkms`, `v4l2loopback-utils` |
| Inspection | v4l-utils | `v4l-utils` |

### 2.2 Optional

| Need | Choice | When |
|---|---|---|
| Unit tests | GoogleTest | `-DVCAM_BUILD_TESTS=ON` (default ON in Debug) |
| YAML config | yaml-cpp | Phase 7; if absent, config files are disabled, CLI still works |
| Consumer tests | Python 3 + OpenCV, `ffmpeg`/`ffplay` CLI | Phase 5+ validation only, never linked |

Written in-house (small, readable, no dependency): logger, CLI parser (`getopt_long`), JSON/CSV writers,
percentile histogram, SPSC ring buffer.

### 2.3 Why these, and the alternatives considered

**Decoding: FFmpeg libav\* (chosen)**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **FFmpeg libav\*** | Exact per-frame `pts` in rational time base; handles VFR, B-frame reordering, every container listed in your brief; libswscale included; stable send/receive API since 4.x | C API is verbose; minor API changes between 4.4 / 6.x / 7.x | **Chosen** — timing correctness needs exact timestamps |
| OpenCV `VideoCapture` | Short code | Timestamps approximate (`CAP_PROP_POS_MSEC`), forces BGR, VFR not exposed, huge dependency | Rejected for core; used only as a **test consumer** |
| GStreamer | Powerful, has `v4l2sink` | Pipeline owns its own clock — conflicts with our requirement that *our* scheduler owns timing; large API surface | Rejected for V1; could become an optional source later |

Image sequences (Phase 7) reuse FFmpeg's PNG/JPEG decoders, so no extra image library is needed.

**Output: direct V4L2 ioctls + `write()` (chosen)** rather than piping into `ffmpeg -f v4l2` or GStreamer's
`v4l2sink`. The V4L2 producer side is ~150 lines of code and gives us precise knowledge of *when* a frame
was handed to the kernel — which is the "actual timestamp" we must measure.

---

## 3. Linux Virtual-Camera Mechanism

### 3.1 Options

| Mechanism | Visible to OpenCV/FFmpeg/v4l2-ctl | Visible to Chrome/Firefox/Zoom/OBS | Maintenance | Verdict |
|---|---|---|---|---|
| **v4l2loopback** (kernel module, userspace producer) | Yes — real `/dev/videoN` | Yes (with `exclusive_caps=1`) | Widely used (OBS virtual cam uses it), packaged in Debian/Ubuntu via DKMS | **Chosen for V1** |
| akvcam | Yes | Yes | Smaller user base, config-file driven | Alternative if v4l2loopback is unavailable |
| PipeWire camera node | No (not a V4L2 device) | Increasingly (portal-based apps) | Modern, no kernel module | **Future second backend**, not a replacement |
| CUSE (char device in userspace) | Partially — `mmap` streaming does not work properly | Unreliable | Hard | Rejected |
| Custom kernel driver | Yes | Yes | High cost, GPL kernel code, Secure Boot signing | Rejected — no technical reason yet |
| `vivid` test driver | Yes | Yes | Generates its own patterns; cannot be fed arbitrary frames | Rejected (useful only for consumer smoke tests) |

### 3.2 How v4l2loopback is used

One-time setup (root), provided as `tools/setup_loopback.sh`:

```bash
sudo modprobe v4l2loopback devices=1 video_nr=10 \
     card_label="Virtual Camera Engine" exclusive_caps=1 max_buffers=2
```

| Parameter | Why |
|---|---|
| `video_nr=10` | stable path `/dev/video10` |
| `card_label=...` | the camera name apps display (this is how `--device-name` works; see note below) |
| `exclusive_caps=1` | device advertises *only* CAPTURE once a producer is streaming. **Chrome/WebRTC hide devices that advertise both OUTPUT and CAPTURE**, so this is required for browsers |
| `max_buffers=2` | keeps latency low; consumers always see the newest frames |

Producer (our `V4L2LoopbackCamera`) sequence:

```text
open("/dev/video10", O_WRONLY | O_NONBLOCK)
VIDIOC_QUERYCAP            → verify driver is "v4l2 loopback" and supports VIDEO_OUTPUT
VIDIOC_S_FMT  (OUTPUT)     → width, height, pixelformat, bytesperline, sizeimage, colorspace
VIDIOC_S_PARM (OUTPUT)     → timeperframe = 1 / output_fps (what consumers read via G_PARM)
loop: write(fd, frame, sizeimage)   ← called by the scheduler at each deadline
close(fd)
```

Note on `--device-name`: the visible name is fixed when the loopback device is created. V1 documents the
`modprobe` route. Newer v4l2loopback (0.13+) supports creating devices at runtime through
`/dev/v4l2loopback` (`v4l2loopback-ctl add -n "Name" -x 1 /dev/video10`); an optional `--create-device`
path using that control interface is planned for Phase 5, but it needs extra privileges, so the engine
itself will **not** require root by default.

### 3.3 Consequences we must design around

* **One format per device.** A loopback device exposes exactly the format the producer set. A consumer
  asking for a different resolution cannot renegotiate; it gets the producer's format or an error. This is
  documented behaviour, not a bug. (Multiple resolutions = multiple devices, a later feature.)
* **Producer pace = camera pace.** Consumers block in `DQBUF`/`read` until we write; our scheduler therefore
  *is* the camera's frame clock.
* **Slow consumers never block us.** v4l2loopback does not make the writer wait for readers; a slow reader
  simply misses frames. We additionally open with `O_NONBLOCK` and count any `EAGAIN` as output backpressure.
* **Consumer connect/disconnect is invisible to the producer** (there is no notification API). The engine
  is unaffected by design; consumer-side behaviour is verified with our own probe tool (§6.6).
* With `exclusive_caps=1`, browsers list the camera only **after the first frame is written** — another
  reason the device is opened at READY and streaming starts immediately after.

### 3.4 Pixel format choice

| Format | Bytes/pixel | Interoperability | Use |
|---|---|---|---|
| **YUYV** (YUY2, 4:2:2 packed) | 2 | The format practically every webcam app accepts (browsers, Zoom, OBS, OpenCV, FFmpeg) | **Default** |
| I420 / YU12 (4:2:0 planar) | 1.5 | Browsers, FFmpeg, OpenCV, OBS; some native apps refuse it | Option — 25 % less memory |
| NV12 | 1.5 | Good with GStreamer/FFmpeg, uneven elsewhere | Option |
| RGB24 / BGR24 | 3 | OpenCV/FFmpeg only; many webcam apps reject RGB | Option for testing / CV use |

The format is a runtime option (`--pixel-format`), not hard-coded. Constraints enforced at config time:
YUYV/UYVY need even width; I420/NV12 need even width and height.

**Resolution rule (v0.2):** the virtual camera always exposes the **source's own resolution**. There is no
scaling stage and no `--width/--height` option. If the source has an odd width/height that the chosen pixel
format cannot represent (e.g. 321×241 with YUYV), the engine refuses in LOADING with a clear message and
suggests a format that can (RGB24, BGR24, GRAY8). If a stream changes resolution mid-file, decoding stops with
an explicit error rather than silently scaling.

Colour metadata declared to consumers: BT.601, limited range (the conventional webcam signalling).
Sources tagged BT.709 (most HD files) are converted with the correct matrix by libswscale
(`sws_setColorspaceDetails`), so colours are not washed out or shifted. Full details go in `docs/FORMATS.md`
(channel order, bit depth, stride, alignment, memory layout per format).

---

## 4. Thread Model

Your brief suggested five threads and asked me to check which are really needed. Analysis:

| Proposed thread | Needs its own execution context? | Reason |
|---|---|---|
| Source / decoder | **Yes** | Decoding is CPU-heavy and has unbounded per-frame latency; it must never delay a deadline |
| Buffer manager | No | The buffer is a passive data structure; the loader writes, the pacer reads. A thread would only add hand-off latency |
| Timing / scheduler | **Yes** | The only timing-critical context; must do nothing but wait, select, publish, record |
| Virtual camera output | No (merged into pacer) | `write()` of one frame is a bounded memcpy into the kernel (~0.1–1 ms). A separate thread would add a hand-off whose latency *is* jitter, and would blur what "actual publish time" means |
| Control / CLI | **Yes** (the main thread) | Handles signals, stdin commands, progress/stats printing and file I/O |

### Final V1 model: three threads

```text
main thread (control)                loader thread                    pacer thread
─────────────────────                ─────────────                    ────────────
parse CLI / config                   open source                      wait until READY
start loader                         decode → convert → buffer slot   T0 = now + margin
poll() on:                           publish frame count (atomic)     loop:
  signalfd   (Ctrl+C, SIGTERM)       until EOF or stop                  sleep_until(T0 + n·ΔT)  [absolute]
  stdin      (pause/resume/seek/…)   then exits (full-preload mode)     apply pending command
  timerfd    (1 Hz stats refresh)                                       select frame (Playhead)
  eventfd    (state changes)                                            write() to V4L2
print progress / stats                                                  record metric sample (lock-free)
drain metric ring → CSV/JSON                                            n++
```

Communication (no locks on the hot path):

| From → To | Mechanism |
|---|---|
| loader → pacer | buffer's `frames_committed` `std::atomic<uint64_t>` (release/acquire); slots never move once written |
| control → pacer | small command mailbox, checked once per slot |
| pacer → control | single-producer/single-consumer ring of metric samples + atomic counters |
| any → control | state changes signalled through an `eventfd` |

Rules for the pacer thread: no heap allocation, no locks, no logging, no file I/O inside the loop.
Signals are blocked in all threads before they start and handled only through `signalfd` in the main loop,
which makes Ctrl+C handling deterministic. Threads are `std::jthread` with `std::stop_token` for cooperative,
hang-free shutdown.

Later phases may add one thread each where justified: a disk-writer for the disk-backed buffer, and an IPC
receiver for script input.

---

## 5. Buffer Architecture

### 5.1 Why full RAM preload cannot be the only mode

Frames are stored **already converted to the output pixel format** at the source resolution (convert once at load; looping
never re-converts). Raw size per frame:

| Resolution | YUYV | I420 |
|---|---|---|
| 640×480 | 0.59 MiB | 0.44 MiB |
| 1280×720 | 1.76 MiB | 1.32 MiB |
| 1920×1080 | 3.96 MiB | 2.97 MiB |
| 3840×2160 | 15.8 MiB | 11.9 MiB |

Total for a clip at 30 FPS:

| Clip | 720p YUYV | 1080p YUYV | 720p I420 |
|---|---|---|---|
| 1 min (1 800 frames) | 3.1 GiB | 7.0 GiB | 2.3 GiB |
| 15 min (27 000 frames) | **46 GiB** | **104 GiB** | 35 GiB |

So the 15-minute example from your brief does not fit in RAM on a normal machine. Because the output
resolution equals the source resolution (v0.2), the only memory lever inside one mode is the pixel format
(I420 needs 25 % less than YUYV); the real fix for long clips is the disk/hybrid buffer.

### 5.2 Pluggable buffers

```text
FrameBuffer (interface)
 ├── RamBuffer          V1   full preload into RAM
 ├── DiskBackedBuffer   later  full preload into a memory-mapped spill file (page cache does the caching)
 └── HybridBuffer       later  READY after N seconds of pre-roll; loader keeps decoding ahead of the playhead
                               into a bounded RAM ring (optionally backed by disk)
```

| Mode | READY condition | Memory | Suits |
|---|---|---|---|
| `ram` | every frame decoded + converted | N × frame size | short clips, exact repeatable playback |
| `disk` | every frame written to spill file | page cache + N × frame size on disk (720p30: ~55 MB/s read) | long clips on SSD |
| `hybrid` | pre-roll window filled, decode speed measured ≥ required rate | ring size × frame size | very long clips, live sources |

> **As built (v1.1):** the `hybrid` idea is implemented as `StreamBuffer` (`--buffer-mode stream`) and is
> the **default**; see §16.4.

Before loading, the engine **estimates the required memory** (frame size × expected frame count) and
compares it to a budget (default: 50 % of `MemAvailable`, configurable `--memory-limit`). If it does not fit,
it fails in LOADING with a clear message and suggestions, instead of being killed by the OOM killer
halfway through.

### 5.3 Interface

```cpp
class FrameBuffer {
public:
    virtual ~FrameBuffer() = default;
    virtual Status configure(const FrameFormat& fmt, std::optional<uint64_t> expected_frames) = 0;

    // ---- producer side (loader thread only) ----
    virtual Result<MutableFrameView> begin_write() = 0;          // slot to decode/convert into
    virtual Status commit_write(const FrameTiming& timing) = 0;  // publishes the slot
    virtual void   mark_source_complete() = 0;

    // ---- consumer side (pacer thread) ----
    virtual std::optional<FrameView> get(uint64_t source_index) const = 0;  // O(1) random access
    virtual const Timeline& timeline() const = 0;                           // timestamps only, no pixels

    virtual BufferStats stats() const = 0;   // total/loaded frames, memory, disk, capacity, state
    virtual void clear() = 0;                // only while the pacer is stopped
};
```

### 5.4 `RamBuffer` internals (V1)

* Frames have a fixed size, so storage is a list of **slabs** (e.g. 64 frames per slab). Frame `i` lives at
  `slab[i / 64] + (i % 64) × frame_size` → O(1) lookup, no per-frame allocation, no fragmentation.
* The slab directory is reserved up front, so it never reallocates while the pacer reads it.
* Slots are immutable after `commit_write`; the pacer reads them without locks.
* `Timeline` is a separate `std::vector<FrameTiming>` — the Resampler works on it alone, which makes the
  resampling logic testable with zero pixel data.
* Optional `--lock-memory` (`mlockall`) prevents the preloaded frames from being swapped out, which would
  otherwise cause page-fault stalls inside the pacer.

`DiskBackedBuffer` keeps the same slab layout in a file mapped with `mmap`, so the interface and the
indexing math are identical — only the allocator changes.

---

## 6. Timing Architecture

### 6.1 Absolute deadlines

```text
T_n = T0 + n · ΔT,     ΔT = 1 / FPS_out   (FPS_out is a Rational, e.g. 30000/1001)
```

Each deadline is computed **from scratch** from `n`, never by adding intervals, so error cannot accumulate.
Integer arithmetic with a 128-bit intermediate:

```text
T_n[ns] = T0[ns] + (n · 1 000 000 000 · fps.den) / fps.num
```

(the 128-bit intermediate avoids overflow for runs of years and avoids floating-point accumulation).

### 6.2 Clock abstraction (makes fast mode and tests trivial)

```cpp
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t now_ns() const = 0;
    virtual void    sleep_until_ns(int64_t target_ns) = 0;
};
```

| Implementation | `sleep_until` | Used for |
|---|---|---|
| `MonotonicClock` | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, …)` | real-time mode |
| `SimulatedClock` | sets `now = target` instantly | fast simulation mode (§31 of your brief) |
| `ScriptedClock` | injects programmed latency | unit tests for overrun / late-frame handling |

Real-time and fast mode run **the exact same scheduler loop and frame-selection code**; only the clock
object differs.

### 6.3 Scheduler loop

```text
n = 0
T0 = clock.now() + start_margin            (small margin so frame 0 is not already late)
while not stop_requested:
    target = T0 + slot_time(n)
    clock.sleep_until(target)
    wake = clock.now()

    // Overrun policy: never burst to "catch up".
    n_now = slot_index_at(wake)            // which slot are we really in?
    if n_now > n:
        record_dropped_slots(n .. n_now-1) // counted as scheduler overruns
        n = n_now

    apply_pending_command()                // pause/resume/seek/stop
    frame = buffer.get(playhead.select(n)) // Resampler + Playhead (pure functions)
    result = camera.publish(frame)
    publish = clock.now()
    metrics.record(n, target, wake, publish, selection, result)
    n += 1
```

Wake-up precision without a real-time kernel:

* `prctl(PR_SET_TIMERSLACK, 1)` on the pacer thread — the default 50 µs timer slack is otherwise added to
  every sleep.
* Optional `--rt-priority N` → `SCHED_FIFO` (needs `CAP_SYS_NICE` or an rtprio limit; gracefully falls back).
* Optional `--spin-us N` → sleep until `target − N µs`, then busy-wait. Off by default (burns CPU).

### 6.4 What is measured (terminology)

For each output frame `n`:

| Quantity | Definition |
|---|---|
| target time `T_n` | `T0 + n·ΔT` |
| wake time `W_n` | when the pacer returned from sleep |
| actual (publish) time `A_n` | when `write()` returned — the frame is in the kernel and visible to consumers |
| timing error `E_n` | `A_n − T_n` (always ≥ 0 in practice; called *lateness*) |
| wake latency | `W_n − T_n` (OS scheduling component of the error) |
| inter-frame interval `I_n` | `A_n − A_{n−1}` |
| jitter `J_n` | `I_n − ΔT` (deviation of each interval from nominal) |

Reported: mean, median, P95, P99, max of `E_n`; mean |J|, std-dev, P95/P99/max |J|; measured FPS
`= (N−1) / (A_{N−1} − A_0)`; **drift** = least-squares slope of `E_n` versus time (in ppm — a non-zero slope
means error is accumulating); counts of late frames (`E_n >` threshold, default 2 ms, configurable),
dropped output slots, duplicated frames, overruns.

### 6.5 Statistics without slowing the hot path

* The pacer pushes a 48-byte sample into a pre-allocated SPSC ring; the main thread drains it.
* Percentiles use a fixed **log-linear histogram** (1 µs resolution below 1 ms, coarser above): O(1) per
  sample, bounded memory, accurate for runs of days. Exact per-frame CSV is optional (`--timing-csv`).
* Duplicates and drops are split into **expected** (caused by FPS conversion, e.g. 30 → 15 drops half the
  source frames by design) and **unexpected** (buffer underflow, scheduler overrun). Mixing them would make
  the report misleading.

### 6.6 Producer-side vs consumer-side truth

`A_n` is measured on the producer side. What a consumer *receives* also depends on the consumer's own
scheduling. A separate tool `vcam-probe` (Phase 5) opens `/dev/videoN` as a normal camera, records the V4L2
buffer timestamps and receipt times, and decodes a frame-index **barcode** drawn by a built-in test-pattern
source, so ordering, drops and duplicates are verified end-to-end through the real kernel path.

---

## 7. Resampling Strategy

### 7.1 Playhead: one mechanism for pause, loop and seek

The Playhead holds an **anchor** `(n_a, s_a)`: "output slot `n_a` shows source time `s_a`". At 1× speed:

```text
t_src(n) = s_a + (n − n_a) · ΔT_out
```

| Event at slot n | New anchor | Effect |
|---|---|---|
| start | `(0, 0)` | |
| pause | (playhead frozen at `s_p`; output keeps running) | camera keeps emitting the held frame |
| resume | `(n, s_p)` | continues exactly where it paused, output cadence never broke |
| loop (t_src ≥ duration D) | `(n, t_src(n) − D)` | keeps the fractional phase, so the loop seam has correct timing |
| seek to `s` | `(n, s)` | after the buffer confirms the target is available |

The output clock (`T_n`) is **never** re-anchored in default mode — so pause/resume/loop/seek never cause a
timing discontinuity in what consumers see.

### 7.2 Frame selection policy

Default **HOLD (floor)**: show the last source frame whose `pts ≤ t_src(n)` — the frame that "was on screen" at
that moment, exactly what a player or a camera sensor would show. Optional **NEAREST** (round). No
interpolation is ever invented (your §14 Case C).

Comparisons are exact. With the anchor `s_a` stored in source ticks, "frame with `pts` is due at slot `n`" is

```text
(pts − s_a) · tb.num · fps_out.num  ≤  (n − n_a) · fps_out.den · tb.den
```

evaluated in 128-bit integers — no division, no rounding. Lookup cost: a forward-moving cursor gives **amortised O(1)** per slot; after seek/loop a binary search
(`std::upper_bound`) re-positions it in O(log N).

### 7.3 Cases

| Case | Source → Output | Selected source indices for n = 0,1,2,… | Expected counters |
|---|---|---|---|
| A | 30 → 30 | 0,1,2,3,4,… | none |
| B | 30 → 15 | 0,2,4,6,… | 1 source frame skipped per output frame |
| C | 15 → 30 | 0,0,1,1,2,2,… | 1 duplicate per source frame (hold) |
| D | 24 → 30 | 0,0,1,2,3,4,4,5,6,7,… | 1 duplicate every 5 outputs (inherent judder, reported) |
| E | VFR | driven by real per-frame `pts` | duplicates/skips reported as they occur |

Because selection always uses the per-frame timestamps stored in the Timeline, **VFR works by construction**
— there is no hidden CFR assumption. The source probe still reports whether input is VFR (by examining pts
deltas), and files with missing timestamps get synthesised ones from the nominal rate with a warning.

### 7.4 EOF policies

| Policy | Behaviour after the last frame's display duration ends | State |
|---|---|---|
| `stop` | stop writing, close device; consumers see the stream end / time out | EOF → STOPPED |
| `hold` | keep emitting the last frame at the output rate forever (camera stays "live", image frozen) | EOF (output continues) |
| `loop` | wrap to frame 0 via re-anchoring, cadence unchanged, loop count reported | stays STREAMING |

If `--fps` is omitted, the output rate defaults to the source's nominal rate (e.g. `30000/1001`); for VFR
sources without a nominal rate, 30 FPS with a warning. `--fps` accepts `30`, `29.97` and `30000/1001`.

---

## 8. State Machine

```text
INIT ─► LOADING ─► BUFFERING ─► READY ─► STREAMING ◄──► PAUSED
          │            │          │          │
          └────────────┴──────────┴──► ERROR │
                                             ├──► EOF ──(stop)──► STOPPED
any state ──(Ctrl+C / stop)──► STOPPING ──► STOPPED
```

| State | Meaning | Device |
|---|---|---|
| INIT | config parsed and validated | not touched |
| LOADING | source opened, metadata probed, output device *validated* (exists, is loopback, not busy), memory estimated | validated then closed |
| BUFFERING | loader decoding into the buffer; progress printed (`%`, frames, memory) | closed |
| READY | buffer reached its preload condition; device opened and format set | open, format set |
| STREAMING | pacer emitting frames on deadlines | streaming |
| PAUSED | output continues with the held frame (default) or stops writing (`--pause-output stop`); source position preserved | streaming / idle |
| EOF | source exhausted; behaviour defined by EOF policy | depends on policy |
| STOPPING / STOPPED | threads joined, device closed, decoder released | closed |
| ERROR | fatal error; message + exit code | closed |

Transitions are validated in one place (`StateMachine::transition`), so illegal jumps are impossible.
The device is validated in LOADING so a misconfigured `/dev/videoN` fails in a second — not after minutes of
decoding. `--start manual` holds in READY until a `start` command (default: start immediately).

---

## 9. Error Handling

| Failure | Behaviour |
|---|---|
| missing / unreadable file | LOADING → ERROR, message names the path and `errno` |
| unsupported container / codec / no video stream | ERROR naming the codec; lists detected streams |
| corrupted packet / frame | skipped, `decode_errors` counted, WARN (rate-limited); ERROR if errors exceed a threshold (default 1 %) |
| device missing / not a loopback / busy (`EBUSY`) / no permission (`EACCES`) | ERROR with the exact remedy (`modprobe` line, `video` group) |
| device removed while streaming (`ENODEV`, `EIO`) | ERROR, clean stop |
| `write()` returns `EAGAIN` | counted as output backpressure, frame skipped, streaming continues |
| buffer would exceed memory budget | refused in LOADING with suggestions (`--buffer-mode`, I420 pixel format) |
| buffer underflow (hybrid mode) | hold last frame, counted as *unexpected* duplicate |
| scheduler overrun | skip missed slots (never burst), counted, rate-limited WARN |
| Ctrl+C | clean shutdown: stop pacer → stop loader → close device → free decoder; second Ctrl+C forces exit |

Exit codes: `0` ok, `1` usage/config error, `2` input error, `3` device error, `4` runtime error.

---

## 10. Directory Structure and Build

### 10.1 Layout

I propose a per-module layout instead of the flat `include/` + `src/` from the brief. Reason: it lets the build
**enforce** the module boundaries (§1.2), and it gives you exactly the "compile and test module by module"
workflow from the project instructions.

```text
virtual-camera-engine/
├── CMakeLists.txt                 # top level: options, finds deps, adds modules
├── Makefile                       # convenience wrapper: make, make test, make run, make module-<x> …
├── README.md
├── LICENSE
├── cmake/
│   ├── CompilerWarnings.cmake     # -Wall -Wextra -Wpedantic -Wconversion …
│   └── Sanitizers.cmake           # ASan/UBSan/TSan toggles
├── modules/
│   ├── core/                      # Frame, FrameFormat, PixelFormat, Rational, Status, logging
│   │   ├── CMakeLists.txt
│   │   ├── include/vcam/core/*.hpp
│   │   ├── src/*.cpp
│   │   └── tests/*.cpp
│   ├── convert/                   # FormatConverter (libswscale)
│   ├── source/                    # FrameSource, VideoFileSource, (later) ImageSequenceSource, PatternSource, PushSource
│   ├── buffer/                    # FrameBuffer, RamBuffer, Timeline, (later) DiskBackedBuffer, HybridBuffer
│   ├── resample/                  # Playhead, Resampler
│   ├── timing/                    # Clock implementations, Scheduler
│   ├── output/                    # VirtualCamera, V4L2LoopbackCamera, NullCamera, RawFileCamera
│   ├── metrics/                   # TimingMetrics, Histogram, report writers (text/CSV/JSON)
│   ├── control/                   # StateMachine, Controller, Engine (wires modules)
│   └── config/                    # Config struct, CLI parser, YAML loader, validation
├── apps/
│   └── virtual-camera/main.cpp    # thin: parse config, build Engine, run
├── tools/
│   ├── env_check.sh               # Phase 0 environment inspection
│   ├── setup_loopback.sh          # load v4l2loopback with the right options
│   ├── gen_test_media.sh          # generate test videos with ffmpeg (CFR, VFR, 15/24/30/60 fps, corrupt)
│   ├── probe_source.cpp           # Phase 1 debug tool: decode a file, print metadata + per-frame timestamps
│   ├── bench_scheduler.cpp        # Phase 3: run scheduler with NullCamera, print timing report
│   ├── resample_table.cpp         # Phase 4: print n → source-index mapping for given rates
│   ├── v4l2_pattern.cpp           # Phase 5: push a test pattern to /dev/videoN without decoding anything
│   └── vcam_probe.cpp             # Phase 5: consumer-side timing / ordering verification
├── tests/
│   ├── integration/               # cross-module tests (source+buffer, engine with NullCamera)
│   └── e2e/                       # Python/OpenCV and ffmpeg consumer tests against /dev/videoN
├── config/examples/*.yaml
└── docs/
    ├── ARCHITECTURE.md            # this document
    ├── BUILD.md                   # full + module-by-module build/run instructions
    ├── TIMING.md                  # timing model + validation results
    ├── FORMATS.md                 # pixel formats, colour, stride, alignment
    └── COMPATIBILITY.md           # per-application results
```

Note: your project folder name contains spaces (`Virtual Camera Engine`). CMake handles this; the Makefile
will quote all paths so it works too.

### 10.2 Build targets (planned)

Full build:

```bash
make                 # = cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
make debug           # Debug + ASan/UBSan
make test            # all unit + integration tests (ctest)
make run ARGS="--input video.mp4 --fps 30"
```

Module by module (each module is its own library + its own test executable + debug tool):

```bash
make module-core       && make test-core
make module-source     && make test-source
make probe-source FILE=~/Videos/test.mp4         # Phase 1 debug tool
make bench-scheduler FPS=60 FRAMES=3000          # Phase 3, no V4L2 needed
# or directly with CMake:
cmake --build build --target vcam_source probe_source test_source
ctest --test-dir build -R source --output-on-failure
```

`docs/BUILD.md` will also show the raw `g++ … $(pkg-config --cflags --libs libavformat …)` command for
compiling a single tool by hand, for when you want to debug outside CMake.

Compiler settings: C++20, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, `-Werror` optional in CI,
Debug/Release/RelWithDebInfo, sanitizer toggles, tests optional (`-DVCAM_BUILD_TESTS=ON|OFF`).

---

## 11. Development Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **0** Environment | `tools/env_check.sh` output, v4l2loopback loaded, `ffmpeg → /dev/video10 → ffplay/OpenCV` smoke test passes | you send me the script output; known OS/FFmpeg/v4l2loopback versions |
| **1** Frame source | `core`, `convert`, `source` (`VideoFileSource`), `probe_source`, tests | frame count & timestamps match `ffprobe -show_frames`; invalid inputs rejected; ASan clean |
| **2** Buffer | `RamBuffer`, `Timeline`, memory estimate/budget, stats | random + sequential access tests; concurrent loader/reader test under TSan |
| **3** Timing | `Clock`s, `Scheduler`, `TimingMetrics`, `bench_scheduler` | simulated clock: E_n = 0 exactly; real clock at 24/25/30/60/120 FPS: drift slope ≈ 0, measured FPS within 0.01 % over 10 min |
| **4** Resampling | `Playhead`, `Resampler`, `resample_table` | exact index tables for 30→30, 30→15, 15→30, 24→30, VFR fixture |
| **5** Virtual camera | `V4L2LoopbackCamera`, `NullCamera`, `RawFileCamera`, `v4l2_pattern`, `vcam_probe` | `v4l2-ctl` shows correct format/FPS; ffplay and OpenCV open it; disconnect/reconnect OK |
| **6** Integration | `Engine`, CLI, state machine, progress output — **first complete virtual camera (MP4 → /dev/video10)** | the user experience in §41 of your brief works end to end |
| **7** Controls | pause/resume/stop, loop/hold/stop EOF, seek, YAML config, image-sequence source | control tests; seek accuracy tests |
| **8** Script input | `PushSource` + C API + pure-Python client (shared memory + Unix socket) | Python generator drives the camera; engine keeps owning timing |
| **9** Performance | disk/hybrid buffers, `--rt-priority`, long-run tests | CPU/RAM/startup/jitter/drift report in `docs/TIMING.md` |
| **10** Compatibility | OpenCV, FFmpeg, VLC, OBS, Chromium, Firefox, others | `docs/COMPATIBILITY.md` |

### 11.1 Script input design (Phase 8, architecture fixed now)

```text
Python / C++ producer ──(Unix socket: handshake, passes memfd via SCM_RIGHTS)──► PushSource
        │                                                                            │
        └──── writes frames into shared-memory slots, bumps a sequence number ───────┘
                                                                                     ▼
                                                                         HybridBuffer (latest-N ring)
                                                                                     ▼
                                                                         Scheduler (engine-owned timing)
```

* The producer never controls timing: it only *offers* frames. At each output slot the engine takes the
  **newest available** frame (like a real sensor). Producer too slow → last frame held (counted). Producer
  too fast → older frames dropped (counted).
* Shared memory means frames are not copied through the socket. The Python client needs only `mmap` and
  `socket` from the standard library (NumPy arrays can be written straight into the slot).
* Because `PushSource` is just another `FrameSource` with `is_live = true`, no other module changes.

---

## 12. Risks and Design Decisions

### 12.1 Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | v4l2loopback not installed / **Secure Boot** refuses the unsigned DKMS module / kernel update breaks DKMS | no device at all | Phase 0 checks `mokutil --sb-state`; document MOK enrolment; engine fails with an exact remedy |
| R2 | Browsers don't list the camera | WebRTC unusable | `exclusive_caps=1`; YUYV default; verified in Phase 10 |
| R3 | Consumers can't choose a different resolution on a loopback device | app-specific failures | documented; the camera always has the source's resolution (v0.2); multi-device later |
| R4 | Full preload of long/high-res clips exhausts RAM | OOM kill | memory estimate + budget check before loading; disk/hybrid buffers |
| R5 | Non-RT kernel wake-up latency (typically tens of µs, occasional ms spikes under load) | jitter | absolute deadlines (no drift), timer slack 1 ns, optional SCHED_FIFO/spin; **measured and reported, not hidden** |
| R6 | FFmpeg API differences (4.4 on Ubuntu 22.04, 6.1 on 24.04, 7.x upstream) | build breaks | use send/receive API only; version guards isolated in `source/ffmpeg_compat.hpp` |
| R7 | Colour mismatch (BT.709 vs BT.601, limited vs full range) | washed-out/shifted colours | explicit colour conversion; declared colorimetry; test with colour bars |
| R8 | Broken timestamps in files (missing pts, non-monotonic) | wrong pacing | `best_effort_timestamp`; detect & repair with warnings; tests with crafted files |
| R9 | Fast mode writing to a real device floods consumers | consumers drop frames | fast mode intended with `NullCamera`/`RawFileCamera`; warns if used with V4L2 |
| R10 | I cannot run commands on your machine from this session (only read/write files in the project folder) | Phase 0 and device tests need you | `env_check.sh` collects everything into one report you paste back; I build and unit-test the non-device modules in my own Linux environment |

### 12.2 Key design decisions (summary)

| Decision | Choice | Main reason |
|---|---|---|
| Virtual camera | v4l2loopback, direct V4L2 ioctls + `write()` | established, works for all listed consumers, no custom kernel code |
| Decoder | FFmpeg libav\* | exact timestamps, VFR, all containers |
| Default output format | YUYV, BT.601 limited | widest compatibility |
| Internal storage | frames pre-converted to output format | convert once; pacer only copies to the kernel |
| Timestamps | integer ticks + rational time base | exact, testable mapping |
| Threads | 3 (control, loader, pacer) | only contexts with a real reason to exist |
| Timing | absolute deadlines, `clock_nanosleep(TIMER_ABSTIME)`, injectable `Clock` | no drift; same code for real-time and fast mode |
| Overrun policy | skip missed slots, never burst | consumers never see bursts |
| Pause default | output keeps running with held frame | real cameras never stop sending frames |
| Selection policy | hold/floor, no interpolation | matches real display semantics; your §14 |
| Module boundaries | separate CMake libraries per module | rule enforced by the compiler |

---

## 13. Minimal Phase 1 Implementation Plan

Goal: **open a video file, probe it, decode every frame in order with exact timestamps, normalise it into a
caller-provided buffer, and shut down cleanly.** No buffer classes, scheduler or V4L2 yet.

### 13.1 Files

| File | Content |
|---|---|
| `modules/core/include/vcam/core/rational.hpp` | `Rational`, reduce, compare, 128-bit-safe `rescale(a, from, to)` |
| `modules/core/include/vcam/core/status.hpp` | `Status` (code + message) and small `Result<T>` (C++20 has no `std::expected`) |
| `modules/core/include/vcam/core/pixel_format.hpp` + `.cpp` | `PixelFormat` enum, name ↔ enum, V4L2 fourcc mapping, `make_frame_format(w, h, fmt, align)` computing planes/strides/size |
| `modules/core/include/vcam/core/frame.hpp` | `FrameFormat`, `FrameTiming`, `FrameView`, `MutableFrameView`, `OwnedFrame` (for tests/tools) |
| `modules/core/include/vcam/core/log.hpp` + `.cpp` | levelled logger (ERROR…TRACE), timestamps, thread names, rate-limited helper |
| `modules/convert/include/vcam/convert/format_converter.hpp` + `.cpp` | RAII wrapper over `SwsContext`: pixel-format + colour matrix/range conversion at identical resolution, plain copy fast path, writes into `MutableFrameView` |
| `modules/source/include/vcam/source/frame_source.hpp` | `FrameSource`, `SourceInfo`, `ReadResult` |
| `modules/source/include/vcam/source/video_file_source.hpp` + `.cpp` | FFmpeg demux + decode, pts normalisation, VFR detection, decoder drain at EOF, corrupt-packet handling, RAII cleanup |
| `modules/source/src/ffmpeg_compat.hpp` | FFmpeg version guards (private to the module) |
| `tools/probe_source.cpp` | prints `SourceInfo`, decodes all frames, per-frame `index / pts / Δt` (with `--frames`), summary (count, duration, VFR?, decode speed), optional `--dump out.yuv` to check visually with `ffplay -f rawvideo` |
| `tools/gen_test_media.sh` | generates fixtures with the `ffmpeg` CLI: 30/15/24/60 FPS CFR, a VFR file, odd resolution, BT.709 HD, truncated/corrupt file, audio-only file |
| `modules/core/tests/test_rational.cpp`, `test_pixel_format.cpp` | unit tests |
| `modules/source/tests/test_video_file_source.cpp` | open valid file, reject missing/invalid/audio-only, frame count, order, monotonic pts, VFR detection |
| `CMakeLists.txt`, `cmake/*.cmake`, `Makefile`, `docs/BUILD.md` | build, module targets, instructions |

### 13.2 Acceptance criteria for Phase 1

1. `probe_source` frame count equals `ffprobe -count_frames` on every fixture.
2. Per-frame timestamps equal `ffprobe -show_frames` `best_effort_timestamp` (after normalising the first to 0).
3. The VFR fixture is reported as VFR; CFR fixtures as CFR with the correct rational rate.
4. Missing file, non-video file, audio-only file → clear error, non-zero exit, no crash.
5. Truncated file → frames before the damage decoded, error counted, no crash.
6. Ctrl+C during decode → clean exit; ASan/LSan report no leaks; UBSan clean.
7. Every module builds and tests independently (`make module-source && make test-source`).

### 13.3 Phase 0 — what I need from you first

Run `tools/env_check.sh` (delivered with this proposal; read-only, it changes nothing) and send me the
report file it writes. It records OS, kernel, compiler, CMake, FFmpeg library versions, OpenCV, GStreamer,
existing `/dev/video*` devices, v4l2loopback status and version, Secure Boot state, `video` group membership
and real-time scheduling limits. It also prints the exact install and smoke-test commands (not run
automatically, because they need `sudo`).

---

## 14. Decisions Needing Your Approval

| # | Question | My default |
|---|---|---|
| D1 | Approve per-module layout (§10) instead of flat `include/` + `src/`? | yes |
| D2 | ~~Aspect-ratio handling~~ — **resolved v0.2:** output resolution always equals input resolution | no scaling |
| D3 | Pause behaviour: keep emitting the held frame (camera stays live) vs stop writing? | keep emitting |
| D4 | Test framework: GoogleTest from the system package? | yes |
| D5 | Project licence (MIT / Apache-2.0 / GPL)? FFmpeg is linked dynamically (LGPL-compatible either way) | MIT |
| D6 | Target Ubuntu release(s) — 22.04, 24.04, or both? | both |

---

## 15. Phase 1 Results (implemented)

Delivered: modules `core`, `convert`, `source`; tool `probe_source`; scripts `gen_test_media.sh`,
`compare_with_ffprobe.sh`; CMake + Makefile; docs `BUILD.md`, `FORMATS.md`.

| Acceptance criterion (§13.2) | Result |
|---|---|
| 1. Frame count equals ffprobe | ✅ 9/9 fixture files (`integration.probe_source_matches_ffprobe`) |
| 2. Per-frame timestamps equal ffprobe `best_effort_timestamp` | ✅ tick-for-tick on all 9 files, incl. B-frames, 29.97 FPS and VFR |
| 3. VFR detected, CFR reported with exact rational rate | ✅ (`VariableFrameRateIsDetected`, `DecodesAllFramesOfCfrFilesWithExactTimestamps`) |
| 4. Missing / non-video / audio-only → clean error | ✅ `NOT_FOUND`, `INVALID_DATA`, `UNSUPPORTED`; exit code 2 |
| 5. Truncated / corrupted file → no crash, damage counted | ✅ |
| 6. Ctrl+C clean; ASan/LSan/UBSan clean | ✅ exit 130 with partial report; 56/56 tests pass under sanitizers; valgrind clean |
| 7. Each module builds and tests alone | ✅ `make test-core`, `make test-convert`, `make test-source` |

Test count: 56 (core 21, convert 8, source 21, integration 5, media fixture 1). Built with
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wold-style-cast … -Werror`.

### Implementation notes (small deviations from the sketch, no structural change)

* `FrameSource::read_next(dst, timing)` — the source owns its `FormatConverter` (configured by
  `set_output_format`) instead of receiving one per call. The converter is still a separate module;
  `source` links it privately. §1.5 updated accordingly.
* `FrameView`/`MutableFrameView` hold the `FrameFormat` by value (≈100 bytes) rather than a pointer, so
  a view can never outlive its format description.
* Colour: libswscale applies the BT.709 → BT.601 matrix even when input and output share the same
  pixel format (verified by test), so no extra conversion stage is needed.

### Finding: conversion is the loading bottleneck

On the 2-core test VM at 1080p: decode alone ≈ 480 fps, decode + YUYV conversion ≈ 110–120 fps
(single-threaded libswscale). `SWS_ACCURATE_RND` was removed after measuring it 2–3× slower with no
visible accuracy benefit (±2 levels). This only affects BUFFERING time, not real-time output.
**Proposal for Phase 2:** the loader converts several frames in parallel (one `FormatConverter` per
worker thread, whole frames per worker, so there are no seams). This adds worker threads to the
loading stage only; the 3-thread streaming model of §4 is unchanged.

### Not yet verified on your machine

All tests ran on Ubuntu 24.04 / FFmpeg 6.1 / GCC 13 in the cloud workspace, whose FFmpeg has no
libx264 encoder, so fixtures there are MPEG-4 Part 2 (with B-frames). On a normal Ubuntu install,
`gen_test_media.sh` uses H.264 automatically. FFmpeg 4.4 (Ubuntu 22.04) compatibility is handled by
version guards but has not been compiled here.

---

## 16. v1.0 Implementation Notes (Phases 2–10)

Everything below was built and tested; this section records **where the implementation differs from the
proposal above, and why**, as required by the brief ("do not change the architecture silently").

### 16.1 Deviations

| # | Proposal | Implemented | Reason |
|---|---|---|---|
| A1 | Script input over **shared memory** + Unix socket handshake (§11.1) | Frames sent **over the Unix socket** itself (`PushSource`, protocol in `docs/SCRIPT_INPUT.md`) | Client code is ~100 lines of standard-library Python; a Unix socket carries several GB/s, far above 1080p60 (~250 MB/s). Interface (`FrameSource`) unchanged, so shared memory can be added later without touching other modules. |
| A2 | `HybridBuffer` (pre-roll + decode-ahead ring) for long files (§5.2) | **v1.0:** not implemented. **v1.1:** implemented as `StreamBuffer` (`--buffer-mode stream`): a fixed window of decoded frames ahead of the playhead, no disk. No decode-speed measurement before READY; instead a late decoder is detected per slot (*underflow*, previous frame repeated). `LiveFrameBuffer` (newest-frame triple buffer) still serves live input. | Requested: low RAM by default without hurting frame rate or CPU. Design in §16.4. |
| A3 | Parallel frame conversion in the loader (§15 proposal) | **Not implemented**; single converter. | Kept the thread model unchanged as approved; conversion speed only affects loading time (measured in `docs/TIMING.md`). |
| A4 | Tool `v4l2_pattern` (§10.1) | Replaced by the engine itself: `virtual-camera --input pattern` (`PatternSource`) | Avoids a second producer implementation; the pattern carries a frame-number barcode that `vcam_probe`, the tests and `opencv_check.py` decode. |
| A5 | `--device-name` sets the camera name | The name is set when the device is created (`tools/setup_loopback.sh --name`); `--device-name` only checks it and warns with the fix | v4l2loopback fixes `card_label` at device creation; the runtime control ioctl is version-specific and needs root. |
| A6 | Control loop listed under the main app | `ControlLoop` lives in the `control` module; `apps/virtual-camera/main.cpp` is ~40 lines | Testable and reusable; the app stays thin. |
| A7 | Config via yaml-cpp (optional) | Built-in parser for the YAML subset used by config files (sections + `key: value`) | No external dependency; one setter table (`apply_setting`) serves both the file and the command line. |
| A8 | `ReadResult` = Frame / EndOfStream / Error | Added **Again** (live sources: nothing yet) and `FrameSource::interrupt()` | Needed so the loader thread never blocks forever on a live producer and shutdown stays prompt. |
| A9 | Default buffer mode `ram` (§5.2, decision D-buffer) | **Default `stream`** (v1.1). `FrameSource::seek()` implemented for video files, image sequences and the pattern (needed for loop/seek without a full buffer). Playhead's slot→time arithmetic factored out into `SourceClock` (shared by `Playhead` and stream mode). The loader thread runs at `nice` 10 in stream mode. | RAM use independent of video length (e.g. 15-min 800×410: ~65 MB instead of ~16.5 GiB); measured timing unchanged (docs/TIMING.md §4.7). The thread model (loader / pacer / control) is unchanged: the loader simply keeps running while streaming. |

### 16.2 Module map (as built)

| Module | Main types | Depends on |
|---|---|---|
| `core` | `Rational`, `Status/Result`, `PixelFormat`, `FrameFormat`, `FrameView`, `OwnedFrame`, logging, monotonic time, text formatting | — |
| `convert` | `FormatConverter` (libswscale, no scaling) | core, FFmpeg (private) |
| `source` | `FrameSource`, `VideoFileSource`, `ImageSequenceSource`, `PatternSource`, `PushSource`, barcode, factory | core; convert + FFmpeg (private) |
| `buffer` | `FrameBuffer`, `StreamBuffer`, `RamBuffer`, `DiskBackedBuffer`, `LiveFrameBuffer`, `Timeline`, memory budget | core |
| `timing` | `Clock` (`MonotonicClock`, `SimulatedClock`), `Scheduler`, real-time thread setup, decoder thread priority | core |
| `metrics` | `LatencyHistogram`, `TimingAnalyzer`, `SpscRing`, text/JSON/CSV reports | core |
| `resample` | `SourceClock` (exact slot → source time, pause/seek anchoring), `Playhead` (selection and EOF policies over a Timeline) | core, buffer (Timeline) |
| `output` | `VirtualCamera`, `V4L2LoopbackCamera`, `NullCamera`, `RawFileCamera` | core, kernel V4L2 header |
| `config` | `Config`, `apply_setting`, config file, command line | core, source, resample (enums) |
| `control` | `StateMachine`, commands, `Engine`, `ControlLoop` | all of the above |

The separation rules of §1.2 hold and are enforced by CMake: e.g. `output` links only `core`, so it cannot
see where frames come from; `source` cannot see `/dev/videoX`.

### 16.3 What was verified where

| Area | Verified in the cloud build (no device) | Needs your machine (`make device-test`) |
|---|---|---|
| Decoding, timestamps, VFR, damaged files | ✅ vs ffprobe, 9 files | real H.264/HEVC clips |
| Buffers (stream, RAM, disk, live), budgets | ✅ incl. concurrent reader/writer; engine tests run in stream and ram mode | very large clips |
| Scheduler, metrics, drift | ✅ simulated (exact) + real clock (`docs/TIMING.md`) | your CPU/kernel numbers |
| Resampling, loop/hold/stop, pause/seek | ✅ exact index tables, engine end-to-end via barcodes | — |
| Script input | ✅ Python producer → engine → frames verified | — |
| V4L2 output | error paths only (no v4l2loopback in the container) | ✅ required |
| Applications (OpenCV, FFmpeg, VLC, OBS, browsers) | — | ✅ required (`docs/COMPATIBILITY.md`) |

### 16.4 Stream mode (default since v1.1)

```text
 loader thread (nice 10)                          pacer thread (timing-critical, unchanged scheduler)
 ───────────────────────                          ──────────────────────────────────────────────────
 loop:                                            each slot n:
   seek requested? -> source.seek(t),               ticks = SourceClock.ticks_at(n)
                      window.restart()              pick  = StreamBuffer.pick(ticks)
   window full?    -> sleep (cond. variable)          -> frame on screen at that time; frees older frames
   decode 1 frame  -> pts += offset, commit           -> underflow: next frame due but not decoded:
   end of video    -> loop: offset += length,            previous frame repeated, counted
                      source.seek(0), pass++        publish(frame)
                      stop/hold: mark complete
```

* **Window size** = `read_ahead` (default 1 s) × source fps + 2 frames, capped at 256 MiB (or
  `--memory-limit`), minimum 4. Memory is allocated once; nothing is allocated while streaming.
* **READY** when the window is full for the first time (or the whole input fitted), so streaming starts
  with a full cushion.
* **One timeline, no loop logic in the reader:** the loader adds the length of every completed pass to the
  timestamps (*unwrapped* time), so source time only increases; `SourceClock` just keeps counting.
  Each frame carries its *pass* number, so the pacer marks the loop at the exact slot the seam reaches the
  screen (the loader is up to 1 s ahead).
* **Seek:** the pacer sets the clock to the target and freezes it, then hands the target to the loader
  (mutex-protected value + request counter). The loader seeks the decoder, drops the window
  (`restart()`; the frame on screen stays valid) and reports completion. The pacer unfreezes the clock
  when the first new frame is available; until then the old picture is repeated (no black frames). Seeking
  to a frame is exact: `VideoFileSource::seek` decodes forward from the preceding keyframe and returns
  the frame on screen at the target.
* **Timing isolation:** the pacer never waits for the loader (in real-time mode). The loader and FFmpeg's
  decoder threads (2 by default, inherited priority) run at `nice` 10, so the pacer wins the CPU under
  load. Measured: docs/TIMING.md §4.7.
* **Fast mode** (simulated clock) is the exception: there the pacer waits for the decoder instead of
  reporting underflow, so fast runs stay deterministic and produce the same frames as `ram` mode
  (verified by the barcode tests, which run in both modes).
* **`--selection nearest`** is approximated by picking at *t + ½ nominal frame duration* (exact for
  constant frame rate; `ram`/`disk` give the exact variant for variable frame rate).
* **Trade-off vs `ram`/`disk`:** the machine must decode the video in real time (≈5 % of one core for
  800×410, ≈40 % for 1080p H.264 on the test VM). If it cannot, frames are repeated and counted as
  underflow; `ram`/`disk` remove that requirement at the cost of memory / loading time.
