# Timing Model and Measured Accuracy

The engine does not just claim to behave like a camera — it measures it. This page defines the
quantities, explains how the clock works, and reports the measurements taken during development.

## 1. Two clocks

| | Symbol | Unit | Used by |
|---|---|---|---|
| Logical source time | `t_src` | integer ticks of the file's time base | source, timeline, playhead |
| Output (wall-clock) time | `t_out` | nanoseconds of `CLOCK_MONOTONIC` | scheduler, metrics |

They are linked only through the playhead: output slot `n` shows the source frame on screen at

```text
t_src(n) = s_a + (n − n_a) · 1/FPS_out          (anchor (n_a, s_a) moves on pause, seek, loop)
```

and slot `n` is due at

```text
T_n = T0 + n · 1/FPS_out
```

computed from scratch for every `n` with exact integer arithmetic (128-bit), and slept to with
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`. Oversleeping one frame therefore never delays the
next deadline: **error cannot accumulate**. "The video says 30 FPS" and "the host delivered a frame every
33.333 ms" are kept apart — the first is `t_src`, the second is what this page measures.

## 2. What is measured (per output frame)

| Quantity | Definition |
|---|---|
| target `T_n` | when slot `n` is due |
| wake `W_n` | when the timing thread returned from its sleep |
| actual `A_n` | when `write()` to the device returned (the frame is visible to applications) |
| lateness `E_n` | `A_n − T_n` (includes the frame copy into the kernel) |
| wake latency | `W_n − T_n` (the operating system's share) |
| jitter `J_n` | `(A_n − A_{n−1}) − k·ΔT`, `k` = slots between the two frames (normally 1) |
| measured FPS | `(frames − 1) / (A_last − A_first)` |
| drift | least-squares slope of `E_n` over time, in ppm (1 ppm = 1 µs per second). 0 = no accumulation. Meaningful for runs ≥ 30 s |
| late frame | `E_n` > threshold (default 2 ms, `--late-ms`) |
| dropped (overrun) | slot skipped because the thread woke after the *next* slot was already due; the engine never emits a burst of old frames |
| repeated / skipped source frames | expected consequences of FPS conversion, pause, hold, live producers — counted separately |

Percentiles come from a fixed-memory histogram (1 µs resolution below 1 ms). Mean, standard deviation,
min and max are exact.

## 3. How to measure on your machine

```bash
make bench ARGS="--fps 30 --seconds 600"                       # scheduler only, no device
./build/bin/virtual-camera --input clip.mp4 --report-json r.json --timing-csv frames.csv
./build/bin/vcam_probe --device /dev/video10 --frames 600      # what an application receives
make device-test                                               # all consumer-side checks
```

Options that tighten timing:

| Option | Effect | Cost |
|---|---|---|
| (always on) timer slack 1 ns | removes the kernel's default 50 µs wake-up batching | none |
| `--rt-priority 10` | `SCHED_FIFO`: the timing thread pre-empts normal threads | needs `CAP_SYS_NICE` or `ulimit -r` |
| `--spin-us 200` | sleeps until 200 µs before the deadline, then busy-waits | ~0.6 % CPU per 100 µs at 30 fps |
| `--lock-memory` | `mlockall()`: buffered frames are never swapped out | needs `ulimit -l` |

## 4. Results (development machine)

**Machine:** cloud VM, 2 vCPU Intel Xeon @ 2.1 GHz, Linux 6.18 (`PREEMPT_DYNAMIC`, not a real-time
kernel), Ubuntu 24.04, GCC 13, clock source TSC. Shared, virtualised hardware: the hypervisor can pause
the VM, which is the main source of the rare multi-millisecond outliers below. Each frame copies 1.8 MB
(a 720p YUYV frame, same work as `write()` to a v4l2loopback device), included in lateness.

### 4.1 Fast simulation mode (simulated clock)

One hour at 29.97 fps (`30000/1001`): **107 892 frames, lateness 0 ns for every frame, drift 0 ppm,
frame 30 000 exactly 1001.000000000 s after frame 0.** This proves the deadline arithmetic itself is
exact; everything below is the operating system's contribution.

### 4.2 Frame-rate sweep (real time, default options, 30 s each)

| Requested | Measured FPS | Lateness mean | P95 | P99 | max | Jitter P99 | Late (>2 ms) | Dropped |
|---|---|---|---|---|---|---|---|---|
| 24 | 24.0000 | 0.448 ms | 0.604 | 1.300 | 2.568 | 1.100 ms | 3 / 720 | 0 |
| 25 | 25.0000 | 0.441 ms | 0.636 | 1.200 | 2.844 | 1.600 ms | 3 / 750 | 0 |
| 30 | 29.9999 | 0.448 ms | 0.601 | 1.900 | 3.296 | 1.800 ms | 7 / 900 | 0 |
| 60 | 59.9333 | 0.433 ms | 0.588 | 1.400 | 15.576 | 2.000 ms | 11 / 1800 | 2 |
| 120 | 120.0002 | 0.276 ms | 0.398 | 0.730 | 3.536 | 1.100 ms | 12 / 3600 | 0 |

### 4.3 Long run: drift (real time, 30 fps, 10 minutes, default options)

| Quantity | Value |
|---|---|
| frames | 18 000 in 600.067 s |
| **drift** | **+0.110 ppm** (≈ 0.07 ms over 10 minutes — no accumulation) |
| lateness | mean 0.450, median 0.404, P95 0.597, P99 1.300, max 28.2 ms |
| wake latency | mean 0.148, P99 0.617 ms |
| jitter | mean 0.139, P95 0.334, P99 1.700 ms |
| late frames (> 2 ms) | 97 (0.54 %), spread evenly over the run (VM background noise) |
| dropped slots | 3 (two outliers > 33 ms) — measured FPS 29.995 because of them |

### 4.4 Effect of the timing options (60 fps, 60 s each)

| Mode | Lateness mean | P99 | max | Wake mean | Jitter P99 | Late | Dropped | Measured FPS |
|---|---|---|---|---|---|---|---|---|
| default | 0.411 ms | 1.400 | 14.570 | 0.133 | 1.900 | 21 | 1 | 59.9833 |
| `--rt-priority 50` | 0.377 ms | 0.933 | **3.919** | 0.120 | **0.986** | 10 | **0** | **60.0000** |
| `--rt-priority 50 --spin-us 300` | **0.265 ms** | 1.100 | 11.750 | **0.019** | 1.400 | 16 | 1 | 59.9833 |

Real-time priority removes the large outliers caused by competing threads; spinning removes the
~0.1 ms wake-up latency but cannot help against the hypervisor pausing the whole VM.

### 4.5 Complete engine (real time, 1080p clip, 20 s, output = raw file to /dev/null)

Measured 29.9999 fps; lateness mean 0.130, P95 0.185, P99 0.432, max 1.352 ms; 0 dropped. (Lower
lateness than §4.2 because writing to `/dev/null` copies nothing.)

### 4.6 Resources

| Scenario | Result |
|---|---|
| Loading a 1080p clip (300 frames, MPEG-4) to YUYV in RAM | ≈ 100 frames/s on 2 vCPU (decode ≈ 480 fps, conversion is the bottleneck); RSS 1.28 GB = 300 × 4.1 MB + ~50 MB |
| Same, `--buffer-mode disk` | 3.9 s; RSS also counts the mapped file pages, but those are page cache the kernel can evict |
| Streaming 720p @ 30 fps (pattern, 20 s) | 5 % of one CPU in total, including generating the 300 frames |
| Streaming 1080p @ 30 fps (20 s incl. 3 s loading) | 15 % CPU on average |
| Start-up to READY | LOADING (open file, probe stream, choose format, memory check) 4 ms; BUFFERING 300 × 1080p frames 2.9 s (≈ 103 frames/s) |

### 4.7 Stream mode (default) vs ram mode

H.264 clips, real time, output `null`, 2-vCPU cloud VM, default options. *CPU* = user + system time
of the whole process divided by run time (includes all loading/decoding).

| Clip / mode | Peak RSS | CPU | Fps | Lateness P99 / max | Dropped | Underflow |
|---|---|---|---|---|---|---|
| 800×410, 2 min, **stream**, 30 s | **65 MB** | 5 % | 30.000 | 0.39–0.51 / 1.9–4.4 ms | 0 | 0 |
| 800×410, 2 min, ram, 30 s | 2.35 GB | 20 % (mostly the preload) | 30.000 | 0.61–2.0 / 2.0–30 ms | 0–2 | 0 |
| 1280×720, **stream**, 10 s | **111 MB** | 22 % | 30.000 | — | 0 | 0 |
| 1920×1080, 1 min, **stream**, 20 s | **207 MB** | 41 % | 30.000 | 0.38 / 21.7 ms | 0 | 0 |
| 1920×1080, 1 min, ram | refused: needs 7.0 GiB > 3.7 GiB budget | | | | | |
| 800×410, **stream**, both vCPUs busy with 2 × `yes` | 65 MB | — | 30.000 | 3.2 / 6.7 ms | 0 | 0 |

* RAM in stream mode = read-ahead window (32 frames × frame size) + decoder + ~15 MB, **independent of
  the video's length** (a 15-minute 800×410 clip uses the same 65 MB; in ram mode it needs ~16.5 GiB).
* CPU in stream mode is the cost of decoding in real time, spread over the run instead of paid up front.
* The last row shows the effect of running the decoder at `nice` 10: with the CPU saturated by other
  programs, the frame clock still kept 30.000 fps with no dropped slot and no underflow.
* Rare single-frame outliers (tens of ms) appear in both modes on this VM (hypervisor); use
  `--rt-priority` on a real machine (§4.4).

## 5. Interpretation and recommendations

* **No drift.** Over 10 minutes the frame clock stayed locked to the monotonic clock within 0.11 ppm;
  in simulation it is exact. Delivered frame rates match the requested rate to 4–5 significant digits
  whenever no slot is dropped.
* **Typical lateness ≈ 0.1–0.5 ms**, most of it the frame copy; P99 ≈ 1–2 ms on a shared VM.
* **Outliers** (rare multi-millisecond delays) come from the platform, not from the design; they are
  reported, never hidden, and never cause bursts (late slots are skipped).
* On a physical machine, use `--rt-priority 10` for the cleanest timing; add `--lock-memory` for long
  buffered clips (`ram` mode; in the default stream mode memory is small anyway).
* **Stream mode (default)** costs no timing accuracy (§4.7) and keeps RAM small; if *Underflow repeats*
  grows on a slow machine, the CPU cannot decode in real time: raise `--decoder-threads`, or preload
  with `--buffer-mode ram` / `disk`.

## 6. Still to measure on the target machine

* Consumer-side timing through a real v4l2loopback device (`vcam_probe`, `opencv_check.py`) —
  `make device-test` collects it.
* The cost of `write()` into v4l2loopback for 1080p (a 4 MB copy into the kernel; expected ≈ 0.5–1 ms).
