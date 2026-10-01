#!/usr/bin/env python3
"""
push_frames.py — example frame producer for the engine's script input.

Generates a moving colour gradient (with the frame number as a barcode at the
top) and pushes it to the engine at its own rate.

    # terminal 1: the camera
    ./build/bin/virtual-camera --source-type push --socket /tmp/vcam.sock --fps 30
    # terminal 2: the producer
    python3 tools/examples/push_frames.py --width 1280 --height 720 --fps 30

Works with only the Python standard library; uses NumPy when available
(much faster for large frames).
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from vcam_client import VirtualCameraClient, draw_barcode_rgb  # noqa: E402

try:
    import numpy as np
except ImportError:  # pure-Python fallback
    np = None


def make_frame(width, height, number):
    """RGB24 frame: horizontal gradient that scrolls with the frame number."""
    shift = (number * 4) % width
    if np is not None:
        x = (np.arange(width, dtype=np.uint16) + shift) % width
        red = (x * 255 // max(1, width - 1)).astype(np.uint8)
        frame = np.empty((height, width, 3), dtype=np.uint8)
        frame[:, :, 0] = red
        frame[:, :, 1] = 255 - red
        frame[:, :, 2] = (number * 3) % 256
        draw_barcode_rgb(frame, width, height, number)
        return frame
    row = bytearray()
    for x in range(width):
        red = ((x + shift) % width) * 255 // max(1, width - 1)
        row += bytes((red, 255 - red, (number * 3) % 256))
    frame = bytearray(row * height)
    draw_barcode_rgb(frame, width, height, number)
    return frame


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--socket", default="/tmp/vcam.sock")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--fps", type=float, default=30.0, help="producer rate (the camera rate is the engine's --fps)")
    parser.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = until Ctrl+C)")
    parser.add_argument("--frames", type=int, default=0, help="stop after this many frames (0 = unlimited)")
    args = parser.parse_args()

    with VirtualCameraClient(args.width, args.height, "rgb24", args.socket) as camera:
        print("connected: %dx%d rgb24, pushing at %.1f fps (Ctrl+C to stop)" % (args.width, args.height, args.fps))
        start = time.monotonic()
        number = 0
        try:
            while True:
                if args.frames and number >= args.frames:
                    break
                if args.seconds and time.monotonic() - start >= args.seconds:
                    break
                camera.push(make_frame(args.width, args.height, number))
                number += 1
                # Absolute pacing (no drift): frame n is due at start + n / fps.
                delay = start + number / args.fps - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
        except KeyboardInterrupt:
            pass
        elapsed = time.monotonic() - start
        print("pushed %d frames in %.2f s (%.2f fps)" % (number, elapsed, number / elapsed if elapsed else 0))


if __name__ == "__main__":
    main()
