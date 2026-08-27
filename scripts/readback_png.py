"""Turn a readback buffer dump into a PNG so it can be looked at.

The native renderer's readback path writes logs/readback_<addr>_<w>x<h>_p<pitch>_f<frame>.bin
when it fills a small resolve destination: raw BGRA at the buffer's own row
pitch, which is the host image exactly as the GPU handed it back. Everything
needed to decode one is in the file name.

    python scripts/readback_png.py logs/readback_*.bin

Writes a .png next to each input and prints a one line summary.
"""

import re
import struct
import sys
import zlib
from pathlib import Path

NAME = re.compile(r"readback_([0-9A-Fa-f]+)_(\d+)x(\d+)_p(\d+)_f(\d+)\.bin$")


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + row for row in rows)
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def convert(path):
    match = NAME.search(path.name)
    if not match:
        print(f"{path.name}: not a readback dump name")
        return
    address, width, height, pitch, frame = match.groups()
    width, height, pitch = int(width), int(height), int(pitch)
    data = path.read_bytes()

    rows = []
    total = 0
    dark = 0
    for y in range(height):
        source = data[y * pitch : y * pitch + width * 4]
        if len(source) < width * 4:
            break
        # BGRA in the file, RGB in the PNG.
        row = bytearray(width * 3)
        for x in range(width):
            b, g, r = source[x * 4], source[x * 4 + 1], source[x * 4 + 2]
            row[x * 3 : x * 3 + 3] = bytes((r, g, b))
            luma = r + g + b
            total += luma
            dark += luma < 24
        rows.append(bytes(row))

    if not rows:
        print(f"{path.name}: no complete rows")
        return
    out = path.with_suffix(".png")
    write_png(out, width, len(rows), rows)
    count = width * len(rows)
    print(f"{out.name}: 0x{address} {width}x{len(rows)} frame {frame} | "
          f"mean {total // (count * 3)} | {dark * 100 // count}% near black")


def main():
    args = sys.argv[1:]
    if not args:
        args = sorted(str(p) for p in Path("logs").glob("readback_*.bin"))
    if not args:
        print("no readback dumps found in logs/")
        return
    for name in args:
        convert(Path(name))


if __name__ == "__main__":
    main()
