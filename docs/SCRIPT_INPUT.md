# Script Input (pushing frames from another program)

Any program — a Python script, a C/C++ application, a simulator — can be the camera's picture source.
The program **produces** frames; the engine stays in charge of **when** the camera shows them.

```text
your program ──frames──► Unix socket ──► PushSource ──► LiveFrameBuffer ──► Scheduler (engine's --fps) ──► /dev/video10
```

* The camera runs at the engine's `--fps` (default 30), whatever rate you push at.
* Push **faster** than the camera → some frames are never shown (counted as "skipped source frames").
* Push **slower** → the last frame is shown again (counted as "repeated frames").
* Disconnect → the camera keeps showing the last frame; a producer can reconnect at any time.
* The **first** producer decides the camera resolution; later producers must use the same width/height
  (their pixel format may differ).

## Quick start (Python)

```bash
# terminal 1 — the camera
./build/bin/virtual-camera --source-type push --socket /tmp/vcam.sock --fps 30
# (no device yet? add --output null to try it without v4l2loopback)

# terminal 2 — the example producer
python3 tools/examples/push_frames.py --width 1280 --height 720 --fps 30
```

Your own script, using `tools/vcam_client.py` (standard library only; NumPy arrays accepted):

```python
import sys
sys.path.insert(0, "tools")
from vcam_client import VirtualCameraClient
import numpy as np

with VirtualCameraClient(width=640, height=480, pixel_format="rgb24", socket_path="/tmp/vcam.sock") as camera:
    t = 0
    while True:
        frame = np.zeros((480, 640, 3), dtype=np.uint8)   # height x width x 3, RGB order
        frame[:, :, 0] = t % 256
        camera.push(frame)                                 # returns once the bytes are sent
        t += 1
```

`push()` accepts `bytes`, `bytearray`, `memoryview` or a contiguous NumPy `uint8` array of exactly
`frame_bytes` bytes. OpenCV images are BGR: use `pixel_format="bgr24"`.

## Protocol (for other languages)

Unix domain stream socket. All integers little-endian.

| Step | Direction | Bytes | Content |
|---|---|---|---|
| HELLO | client → engine | 16 | `"VCAM"`, `u16 version = 1`, `u16 pixel_format`, `u32 width`, `u32 height` |
| reply (accepted) | engine → client | 8 | `"OK\0\0"`, `u32 frame_bytes` |
| reply (rejected) | engine → client | 8 + n | `"ER\0\0"`, `u32 n`, n bytes of UTF-8 message; then the engine closes the connection |
| FRAME (repeat) | client → engine | 8 + frame_bytes | `"FRAM"`, `u32 frame_bytes`, pixel data |

`pixel_format` codes and frame sizes (tightly packed, layouts in `docs/FORMATS.md`):

| Code | Format | frame_bytes |
|---|---|---|
| 0 | yuyv | W·H·2 |
| 1 | uyvy | W·H·2 |
| 2 | i420 | W·H·3/2 |
| 3 | nv12 | W·H·3/2 |
| 4 | rgb24 | W·H·3 |
| 5 | bgr24 | W·H·3 |
| 6 | gray8 | W·H |

Rules enforced by the engine:

* A frame header with the wrong magic or length is a protocol error: the connection is dropped.
* A producer that stops sending in the **middle** of a frame for more than 2 s is dropped.
* The socket file is created with permissions `0600` (only your user can push frames) and removed when
  the engine exits.

### Minimal C client

```c
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, "/tmp/vcam.sock");
    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0) return 1;

    uint32_t width = 640, height = 480;
    uint8_t hello[16] = {'V','C','A','M', 1,0, 4,0};       /* version 1, rgb24 */
    memcpy(hello + 8, &width, 4); memcpy(hello + 12, &height, 4);   /* x86-64 is little-endian */
    write(fd, hello, 16);
    uint8_t reply[8]; read(fd, reply, 8);
    if (reply[0] != 'O') return 2;

    static uint8_t pixels[640 * 480 * 3];
    uint8_t header[8] = {'F','R','A','M'};
    uint32_t size = sizeof pixels; memcpy(header + 4, &size, 4);
    for (int n = 0; ; ++n) {
        memset(pixels, n & 0xFF, sizeof pixels);
        write(fd, header, 8);
        write(fd, pixels, sizeof pixels);                    /* real code: loop on short writes */
        usleep(33000);
    }
}
```

## Timing and diagnostics

The engine's report shows the producer's effect separately from the camera's own timing:

* `Repeated frames` — slots where no new frame had arrived (producer slower than the camera);
* `Skipped src frames` — frames that arrived but were replaced before the next slot (producer faster);
* lateness/jitter/drift — the camera clock itself, unaffected by the producer.

Seeking is not available for live input (there is nothing to seek in); `pause` holds the current frame.

## Tested

`tests/integration/test_push.sh` (part of `make test`): a Python producer pushes 60 fps frames with
frame-number barcodes to a 30 fps engine for 2 s; the test checks that the camera delivered 60 frames,
all complete (no torn frames) and in increasing order.
