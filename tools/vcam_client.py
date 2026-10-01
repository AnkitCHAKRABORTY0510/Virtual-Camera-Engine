"""
vcam_client.py — push frames from Python into the Virtual Camera Engine.

The engine must run with push input:

    ./build/bin/virtual-camera --source-type push --socket /tmp/vcam.sock --fps 30

Then, from Python (standard library only; NumPy arrays are accepted too):

    from vcam_client import VirtualCameraClient

    with VirtualCameraClient(width=1280, height=720, pixel_format="rgb24") as camera:
        while True:
            frame = generate_frame()      # bytes, bytearray, memoryview or NumPy (720, 1280, 3) uint8
            camera.push(frame)

Your script only PRODUCES frames. The engine decides WHEN the camera shows
them (at its own --fps): if you push faster, some frames are skipped; if you
push slower, the last frame is repeated. Either way the camera keeps a steady
rate. Protocol details: docs/SCRIPT_INPUT.md.
"""

import os
import socket
import struct
import time

PROTOCOL_VERSION = 1

# Protocol codes, in the order defined by the engine (push_source.hpp).
PIXEL_FORMATS = {
    "yuyv": 0,
    "uyvy": 1,
    "i420": 2,
    "nv12": 3,
    "rgb24": 4,
    "bgr24": 5,
    "gray8": 6,
}


def frame_size(width, height, pixel_format):
    """Bytes in one tightly packed frame (same rules as the engine)."""
    if pixel_format in ("yuyv", "uyvy"):
        return width * height * 2
    if pixel_format in ("i420", "nv12"):
        return width * height * 3 // 2
    if pixel_format in ("rgb24", "bgr24"):
        return width * height * 3
    if pixel_format == "gray8":
        return width * height
    raise ValueError("unknown pixel format %r" % pixel_format)


class VirtualCameraError(RuntimeError):
    """The engine rejected the connection or the connection broke."""


class VirtualCameraClient:
    """One producer connection to the engine's Unix socket."""

    def __init__(self, width, height, pixel_format="rgb24", socket_path="/tmp/vcam.sock", connect_timeout=10.0):
        if pixel_format not in PIXEL_FORMATS:
            raise ValueError("pixel_format must be one of %s" % ", ".join(PIXEL_FORMATS))
        self.width = width
        self.height = height
        self.pixel_format = pixel_format
        self.frame_bytes = frame_size(width, height, pixel_format)
        self.frames_sent = 0
        self._socket = self._connect(socket_path, connect_timeout)
        self._handshake()

    # -- connection ----------------------------------------------------------------

    @staticmethod
    def _connect(path, timeout):
        """Connects, retrying until the engine has created the socket."""
        deadline = time.monotonic() + timeout
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                return sock
            except (FileNotFoundError, ConnectionRefusedError):
                sock.close()
                if time.monotonic() > deadline:
                    raise VirtualCameraError(
                        "no engine listening on %s (start: virtual-camera --source-type push --socket %s)"
                        % (path, path))
                time.sleep(0.1)

    def _handshake(self):
        # HELLO: "VCAM", u16 version, u16 pixel format, u32 width, u32 height (little-endian)
        hello = b"VCAM" + struct.pack("<HHII", PROTOCOL_VERSION, PIXEL_FORMATS[self.pixel_format],
                                      self.width, self.height)
        self._socket.sendall(hello)
        reply = self._receive_exactly(8)
        code, number = reply[:2], struct.unpack("<I", reply[4:8])[0]
        if code == b"OK":
            if number != self.frame_bytes:
                raise VirtualCameraError("engine expects %d bytes per frame, client computed %d"
                                         % (number, self.frame_bytes))
            return
        message = self._receive_exactly(number).decode("utf-8", "replace") if number else "rejected"
        self._socket.close()
        raise VirtualCameraError("engine rejected the connection: " + message)

    def _receive_exactly(self, count):
        data = b""
        while len(data) < count:
            chunk = self._socket.recv(count - len(data))
            if not chunk:
                raise VirtualCameraError("connection closed by the engine")
            data += chunk
        return data

    # -- frames -----------------------------------------------------------------------

    def push(self, frame):
        """Sends one frame. Accepts any bytes-like object or a contiguous NumPy uint8 array."""
        view = memoryview(frame).cast("B")  # flat byte view, no copy
        if view.nbytes != self.frame_bytes:
            raise ValueError("frame has %d bytes, expected %d (%dx%d %s)"
                             % (view.nbytes, self.frame_bytes, self.width, self.height, self.pixel_format))
        try:
            self._socket.sendall(b"FRAM" + struct.pack("<I", self.frame_bytes))
            self._socket.sendall(view)
        except (BrokenPipeError, ConnectionResetError) as error:
            raise VirtualCameraError("engine closed the connection") from error
        self.frames_sent += 1

    def close(self):
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# -- frame-number barcode (same layout as the engine's frame_barcode.hpp) ------------------

BARCODE_BITS = 32


def _barcode_bits(number):
    value = number & 0xFFFFFF
    checksum = (value & 0xFF) ^ ((value >> 8) & 0xFF) ^ ((value >> 16) & 0xFF) ^ 0xA5
    code = (value << 8) | checksum
    return [(code >> (BARCODE_BITS - 1 - bit)) & 1 for bit in range(BARCODE_BITS)]


def draw_barcode_rgb(frame, width, height, number):
    """Draws the frame number into an RGB24 bytearray (or NumPy (h, w, 3) array) in place."""
    block = width // BARCODE_BITS
    band = max(4, height // 16)
    bits = _barcode_bits(number)
    try:
        import numpy  # noqa: F401  (fast path)
        if hasattr(frame, "shape"):
            for bit, value in enumerate(bits):
                frame[:band, bit * block:(bit + 1) * block, :] = 255 if value else 0
            return
    except ImportError:
        pass
    stride = width * 3
    for y in range(band):
        row = y * stride
        for bit, value in enumerate(bits):
            start = row + bit * block * 3
            frame[start:start + block * 3] = (b"\xff" if value else b"\x00") * (block * 3)


def read_barcode_luma(luma_at, width, height):
    """Reads a frame number; luma_at(x, y) returns 0..255. None if the checksum fails."""
    block = width // BARCODE_BITS
    band = max(4, height // 16)
    code = 0
    for bit in range(BARCODE_BITS):
        code = (code << 1) | (1 if luma_at(bit * block + block // 2, band // 2) >= 128 else 0)
    value = code >> 8
    checksum = (value & 0xFF) ^ ((value >> 8) & 0xFF) ^ ((value >> 16) & 0xFF) ^ 0xA5
    return value if (code & 0xFF) == checksum else None


if __name__ == "__main__":
    print(__doc__)
    print("Example producer: python3 %s" % os.path.join(os.path.dirname(__file__), "examples", "push_frames.py"))
