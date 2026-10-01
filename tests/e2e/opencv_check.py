#!/usr/bin/env python3
"""
opencv_check.py — open the virtual camera with OpenCV like any webcam application.

    python3 tests/e2e/opencv_check.py /dev/video10 --frames 150

Reports resolution, advertised FPS, the FPS actually received and, if the
engine runs the test pattern, whether frame numbers arrive in order.
Exit code 0 = camera usable from OpenCV.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from vcam_client import read_barcode_luma  # noqa: E402

try:
    import cv2
except ImportError:
    print("SKIP: OpenCV for Python is not installed (sudo apt install python3-opencv)")
    sys.exit(0)

parser = argparse.ArgumentParser()
parser.add_argument("device", nargs="?", default="/dev/video10")
parser.add_argument("--frames", type=int, default=150)
args = parser.parse_args()

capture = cv2.VideoCapture(args.device, cv2.CAP_V4L2)
if not capture.isOpened():
    print("FAIL: OpenCV cannot open %s" % args.device)
    sys.exit(1)

width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
fps = capture.get(cv2.CAP_PROP_FPS)
fourcc = int(capture.get(cv2.CAP_PROP_FOURCC))
print("OpenCV %s: %dx%d, %.3f fps advertised, fourcc %s" % (
    cv2.__version__, width, height, fps, "".join(chr((fourcc >> (8 * i)) & 0xFF) for i in range(4))))

times, numbers = [], []
for _ in range(args.frames):
    ok, frame = capture.read()  # BGR image
    if not ok:
        print("FAIL: read() returned no frame")
        sys.exit(1)
    times.append(time.monotonic())
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    numbers.append(read_barcode_luma(lambda x, y: int(gray[y, x]), gray.shape[1], gray.shape[0]))
capture.release()

measured = (len(times) - 1) / (times[-1] - times[0])
readable = [n for n in numbers if n is not None]
print("received %d frames, measured %.3f fps" % (len(times), measured))
if readable:
    backwards = sum(1 for a, b in zip(readable, readable[1:]) if b < a)
    gaps = sum(1 for a, b in zip(readable, readable[1:]) if b > a + 1)
    print("barcodes: %d/%d readable, %d gaps, %d backward jumps (loop restarts)" % (
        len(readable), len(numbers), gaps, backwards))
print("PASS: OpenCV can use the virtual camera")
