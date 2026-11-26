"""The folded reader must leave the background visible above its right edge."""

import pathlib
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory, "history.ppm")
    subprocess.run([sys.argv[1], "--headless", "--demo-screen", "history", "--output", path],
                   check=True)
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    assert (magic, dimensions, maximum) == (b"P6", b"320 170", b"255")


def pixel(x, y):
    start = (320 * y + x) * 3
    return pixels[start:start + 3]


# Sample both the last sheet column and its shadow on the native 320x170 layout.
background = pixel(284, 24)
for y in range(24, 35):
    assert pixel(280, y) == pixel(281, y) == background, f"Exposed edge at y={y}"
assert pixel(280, 35) != background and pixel(281, 35) != background
