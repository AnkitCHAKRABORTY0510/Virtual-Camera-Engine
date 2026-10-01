# Virtual Camera Engine — User Guide

This program turns a **video file into a webcam**. Google Meet, Zoom, OBS, VLC, OpenCV and any other
app will see a camera called **"Virtual Camera Engine"** that plays your video.

You only need 4 steps. Steps 1 and 2 are done **once**; step 3 once **after every reboot**.

```text
 1. Install     2. Build      3. Create camera     4. Play a video  ──►  pick the camera in Meet
   (once)         (once)      (after each reboot)     (every time)
```

---

## Step 1 — Install the required software (once)

Open a terminal, get the code and run the installer:

```bash
git clone <repository-url> virtual-camera-engine     # or use the folder you already have
cd virtual-camera-engine
make install-deps
```

It asks for your password and installs FFmpeg, the virtual-camera kernel module and the build tools.
It works on Ubuntu, Debian, Linux Mint, Fedora, Arch, Manjaro and openSUSE (any 64-bit computer).
On Fedora it also prints two extra commands for the camera module — run them too.

## Step 2 — Build the program (once, and after every code update)

```bash
make
```

When it finishes you have the program at `build/bin/virtual-camera`.
If something is missing, `make` stops and says exactly which package to install.

## Step 3 — Create the virtual camera (after every reboot)

```bash
make setup-loopback
```

This creates the camera `/dev/video10` named "Virtual Camera Engine". It asks for your password.

> **Tip:** to skip this step after every reboot, see "Make the camera permanent" at the end.

## Step 4 — Play your video as the camera

```bash
make run FILE="$HOME/Videos/my_video.mp4"
```

For example, a clip from a dataset folder:

```bash
make run FILE="$HOME/Datasets/traffic/clip_02.mp4"
```

You will see something like this:

```text
[LOADING]   opening source and checking the output device...
            source : h264 800x410 yuv420p, 30 fps, ~27000 frames, 15:00.000
[BUFFERING] filling the read-ahead window (1.0 s); the rest is decoded while streaming
[READY]     read-ahead window: 32 frames, 20.0 MiB RAM in total (any video length)
[STREAMING] Virtual camera is live
STREAMING | 0:12.367 / 15:00.000 | sent 371 | 30.00 fps | late 0 | p99 0.31 ms | dropped 0 | loops 0 | ahead 31/32
```

When you see **`[STREAMING] Virtual camera is live`**, the camera is ready.
Leave this terminal open: the camera works only while the program runs.

---

## Using it in Google Meet (or Zoom, Teams…)

1. **Start the program first** (Step 4), then open Meet.
   If Meet was already open, reload the page.
2. In Meet, click **⋮ → Settings → Video**.
3. Under **Camera**, choose **Virtual Camera Engine**.
4. Your video now plays as your camera. It loops forever by default.

---

## Controlling the video while it plays

Type a word in the terminal where the program runs and press **Enter**:

| Type | What happens |
|---|---|
| `pause` | freezes the picture (Meet keeps showing the frozen frame) |
| `resume` | continues from where it paused |
| `seek 90` | jumps to 90 seconds into the video |
| `stats` | shows how smoothly the camera is running |
| `stop` | closes the camera cleanly |
| `help` | lists these commands |

You can also stop with **Ctrl + C**. A short timing report is printed at the end.

---

## Useful options

Put extra options in `ARGS="..."`:

| I want to… | Command |
|---|---|
| play the video **once** and stop | `make run FILE=video.mp4 ARGS="--on-eof stop"` |
| keep the **last frame** on screen at the end | `make run FILE=video.mp4 ARGS="--on-eof hold"` |
| force **30 frames per second** | `make run FILE=video.mp4 ARGS="--fps 30"` |
| start **2 minutes** into the video | `make run FILE=video.mp4` then type `seek 120` |
| stop automatically after **10 minutes** | `make run FILE=video.mp4 ARGS="--duration 600"` |
| hide the live status line | `make run FILE=video.mp4 ARGS="--quiet"` |
| use a **different camera number** | `make run FILE=video.mp4 DEVICE=/dev/video11` |
| show a **test pattern** (no video needed) | `make run-pattern` |
| play a **folder of images** at 25 fps | `make run FILE=~/Pictures/frames ARGS="--source-fps 25"` |

Combine options with spaces, e.g. `ARGS="--fps 30 --on-eof stop"`.
All options: `./build/bin/virtual-camera --help`.

---

## How much memory and CPU does it use?

Very little, for videos of any length. Only about 1 second of video is kept ready in memory; the
rest is read from the file while it plays.

| Video size | Memory used | CPU used |
|---|---|---|
| 800×410 (e.g. traffic-camera datasets) | about 65 MB | about 5 % of one core |
| 1280×720 (HD) | about 110 MB | about 20 % |
| 1920×1080 (Full HD) | about 210 MB | about 40 % |

**Slow or old computer?** If the status line shows `ahead` dropping close to `0` again and again,
the computer cannot read the video fast enough. Try one of these:

```bash
make run FILE=video.mp4 ARGS="--decoder-threads 4"     # use more CPU cores for reading the video
make run FILE=video.mp4 ARGS="--buffer-mode ram"       # load the whole video into memory first (short videos)
make run FILE=video.mp4 ARGS="--buffer-mode disk"      # load the whole video into a temporary file first (long videos)
```

---

## Something went wrong?

| You see | What to do |
|---|---|
| `camera device '/dev/video10' does not exist` | run Step 3: `make setup-loopback` |
| `no permission to open` | run `sudo usermod -aG video $USER`, then log out and log in again |
| `cannot open '…': No such file or directory` | check the video path; put it in quotes if it has spaces |
| `does not accept output right now` | another copy of the program is already running; stop it first |
| The camera is not listed in Meet | start the program **before** opening Meet, or reload the Meet page |
| Meet shows the camera but the picture is black | make sure the terminal shows `[STREAMING]`; then reselect the camera in Meet |
| `make: *** No targets` / `No rule to make target` | you are not in the project folder: `cd` into it first |
| `FFmpeg development libraries … were not found` | run `make install-deps` again |
| `make run` says `usage: make run FILE=...` | you forgot `FILE="..."` |

Still stuck? Run with more detail and read the last lines:
`make run FILE=video.mp4 ARGS="--verbose"`

---

## Make the camera permanent (optional)

So that you never need Step 3 again, run these two commands once:

```bash
echo v4l2loopback | sudo tee /etc/modules-load.d/v4l2loopback.conf
echo 'options v4l2loopback devices=1 video_nr=10 card_label="Virtual Camera Engine" exclusive_caps=1 max_buffers=2' \
    | sudo tee /etc/modprobe.d/v4l2loopback.conf
```

After the next reboot the camera exists automatically.

To remove the camera: `make remove-loopback`.

---

## Quick reference card

```bash
cd virtual-camera-engine                     # the project folder
make setup-loopback                          # after each reboot
make run FILE="$HOME/Videos/my_video.mp4"    # start the camera
#   pause | resume | seek 90 | stats | stop  # type while it runs
# In Meet: ⋮ → Settings → Video → Camera → "Virtual Camera Engine"
```

For developers: building single modules, tests and debugging are in [BUILD.md](BUILD.md); the design is
in [ARCHITECTURE.md](ARCHITECTURE.md); timing measurements are in [TIMING.md](TIMING.md).
