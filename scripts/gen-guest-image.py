#!/usr/bin/env python3
"""Write the guest image the executable embeds (src/core/guest_image.h).

The image is built from src/guest_data alone: the XEX header
(xex_header.txt) and every byte of data (text, strings, tables, menu
layouts), with no retail file. Code sections are empty because codegen
already compiled them. Zero runs are elided by the XEX "basic" compression,
so only data that is read at runtime takes space.

    python scripts/gen-guest-image.py out/guest-image.xex
"""
import struct
import sys

from guest_data import (drop_unread, overlay_blank_png, overlay_layouts, overlay_pal50,
                        overlay_strings, overlay_tables, overlay_text)
from xex_header import PAGE_SIZE, build_header, import_words, read_header

# Shorter zero runs cost more as a block record than they save.
MIN_ZERO_RUN = 64


def blocks(image):
    """(data_size, zero_size) runs covering the whole image."""
    out = []
    data_start = 0
    pos = 0
    n = len(image)
    while pos < n:
        if image[pos]:
            pos += 1
            continue
        end = pos
        while end < n and not image[end]:
            end += 1
        if end - pos >= MIN_ZERO_RUN or end == n:
            out.append((pos - data_start, end - pos))
            data_start = end
        pos = end
    if data_start < n:
        out.append((n - data_start, 0))
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    dst = sys.argv[1]
    spec = read_header()
    image = bytearray(PAGE_SIZE * sum(c for _, c in spec["pages"]))
    base = spec["load_address"][0]
    overlay_text(image, base)
    overlay_strings(image, base)
    overlay_pal50(image, base)
    overlay_layouts(image, base)
    overlay_tables(image, base)
    overlay_blank_png(image, base)
    words = import_words(spec)
    for addr, word in words.items():
        struct.pack_into(">I", image, addr - base, word)
    drop_unread(image, base)

    runs = blocks(bytes(image))
    body = b"".join(image[off:off + data] for off, data in _offsets(runs))
    header = build_header(spec, runs)
    with open(dst, "wb") as f:
        f.write(header)
        f.write(body)
    print(f"{dst}: {len(header) + len(body)} bytes, {len(runs)} blocks, "
          f"{len(words)} import words")
    return 0


def _offsets(runs):
    off = 0
    for data, zero in runs:
        yield off, data
        off += data + zero


if __name__ == "__main__":
    sys.exit(main())
