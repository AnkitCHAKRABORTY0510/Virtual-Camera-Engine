# Application Compatibility

The engine exposes an ordinary V4L2 capture device through v4l2loopback, so any Linux program that uses
webcams can open it. This page lists how each consumer is tested, what to expect, and known limitations.

> **Status:** the consumer tests below must run on a machine with the v4l2loopback module. The cloud
> build environment used during development has no kernel modules, so the results table is filled by
> `make device-test` on your machine (it writes `device_test_report.md`; paste the table here).

## Before testing

```bash
sudo tools/setup_loopback.sh                      # /dev/video10 "Virtual Camera Engine", exclusive_caps=1
./build/bin/virtual-camera --input pattern        # test pattern with frame numbers, 1280x720 @ 30 fps
```

## Automated: `make device-test`

`tests/e2e/run_device_tests.sh` runs the engine and checks it from the consumer side:

| # | Check | Tool | Pass criterion |
|---|---|---|---|
| 1 | device listed, format and FPS advertised | `v4l2-ctl --list-devices`, `--all` | `/dev/video10` listed; YUYV 1280×720; 30 fps |
| 2 | streaming, received FPS, jitter, frame order | `vcam_probe --barcode` | 300 frames, no missing/unreadable frames |
| 3 | consumer disconnect + reconnect | second `vcam_probe` | receives frames again |
| 4 | FFmpeg capture | `ffmpeg -f v4l2 -i /dev/video10` | 90 frames |
| 5 | OpenCV (Python) | `tests/e2e/opencv_check.py` | opens, reads 150 frames, FPS ≈ 30 |
| 6 | slow consumer | `ffmpeg … -vf fps=5` | engine reports 0 overruns |
| 7 | engine restart with another format | restart at 640×480 @ 15 fps | consumer sees the new format |
| 8 | real video (optional) | `VIDEO=clip.mp4 make device-test` | frames received |

## Manual checklist

| Application | How to open the camera | What to verify |
|---|---|---|
| **v4l2-ctl** | `v4l2-ctl -d /dev/video10 --all` | Driver "v4l2 loopback", card name, `Width/Height`, `Pixel Format 'YUYV'`, `Frames per second` |
| **ffplay** | `ffplay -f v4l2 /dev/video10` | moving box, barcode changes every frame, smooth motion |
| **FFmpeg** | `ffmpeg -f v4l2 -i /dev/video10 -t 10 out.mkv` | `ffprobe out.mkv` shows the expected size/rate |
| **OpenCV C++/Python** | `cv2.VideoCapture("/dev/video10", cv2.CAP_V4L2)` | `CAP_PROP_FRAME_WIDTH/HEIGHT/FPS` match; `read()` returns frames |
| **VLC** | Media → Open Capture Device → `/dev/video10` | picture, no stutter |
| **OBS Studio** | Sources → + → Video Capture Device (V4L2) → "Virtual Camera Engine" | resolution matches; no "device busy" (OBS's *own* virtual camera uses another loopback device) |
| **Chromium / Chrome** | https://webcamtests.com or `chrome://media-internals`, or a WebRTC call | camera listed by name; picture; reported resolution/FPS |
| **Firefox** | https://webcamtests.com | camera listed (with `media.webrtc.camera.allow-pipewire` = false Firefox uses V4L2 directly) |
| **GStreamer** | `gst-launch-1.0 v4l2src device=/dev/video10 ! videoconvert ! autovideosink` | picture |
| **Zoom / Teams / Meet** | Settings → Video → camera "Virtual Camera Engine" | picture (browsers and Zoom expect YUYV — keep the default `--pixel-format yuyv`) |

## Known limitations and their reasons

| Limitation | Why | What to do |
|---|---|---|
| Browsers do not list the camera | v4l2loopback without `exclusive_caps=1` advertises both capture and output; Chrome hides such devices | create the device with `tools/setup_loopback.sh` (sets `exclusive_caps=1`) |
| Camera appears only while the engine is streaming | with `exclusive_caps=1` the device advertises *capture* only while a producer writes | start the engine first, then the application (or restart the app's device list) |
| Application cannot choose another resolution/FPS | a loopback device exposes exactly the producer's format; the camera resolution is the source's (architecture v0.2) | pick FPS with `--fps`; use a source of the wanted size |
| New format not taken after restart | v4l2loopback keeps the previous format while any consumer still has the device open (`keep_format`) | close consumers before restarting with a different size/format, or reload the module (`tools/setup_loopback.sh --reload`) |
| Some native apps reject I420/NV12/RGB | many webcam apps only implement YUYV/MJPEG | use the default `yuyv` |
| Colours slightly off in an app | apps assume BT.601 limited range for YUV webcams; the engine converts HD BT.709 sources to that | report the app; RGB output (`--pixel-format rgb24`) avoids matrix ambiguity for OpenCV |
| Secure Boot refuses the module | DKMS-built modules must be signed | enrol the MOK key during `apt install v4l2loopback-dkms` and reboot |
| PipeWire-only apps (some Flatpaks) | they use the camera portal, not V4L2 directly | PipeWire's V4L2 bridge usually exposes v4l2loopback devices; a native PipeWire backend is a possible future output |

## Results

Fill in from `device_test_report.md` after `make device-test`:

| Application | Version | Result | Notes |
|---|---|---|---|
| v4l2-ctl | | not yet run | |
| vcam_probe (V4L2 mmap) | | not yet run | |
| FFmpeg | | not yet run | |
| OpenCV | | not yet run | |
| VLC | | not yet run | |
| OBS | | not yet run | |
| Chromium | | not yet run | |
| Firefox | | not yet run | |
