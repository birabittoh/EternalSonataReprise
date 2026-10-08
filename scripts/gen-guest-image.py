#!/usr/bin/env python3
"""Write the guest image the executable embeds (src/core/guest_image.h).

The retail default.xex goes in decrypted, with .text and .pdata zeroed:
codegen already compiled the code, and the guest never reads either section
(checked by page protecting them). The import thunk words in .text stay,
since the SDK reads them at load to pair imports. The BTX text comes from
src/guest_data/text, and data nothing reads (the shader blob) is dropped; see
guest_data.py. Zero runs are elided by the XEX "basic"
compression, so only data that is read at runtime takes space.

    python scripts/gen-guest-image.py assets/default.xex out/guest-image.xex
"""
import struct
import sys

from guest_data import drop_unread, overlay_text
from xex_image import XexImage, XEX_FILE_FORMAT_INFO

XEX_IMPORT_LIBRARIES = 0x000103FF

ZEROED_SECTIONS = (".text", ".pdata")

# Shorter zero runs cost more as a block record than they save.
MIN_ZERO_RUN = 64


def opt_headers(raw):
    count, = struct.unpack_from(">I", raw, 20)
    for i in range(count):
        key, value = struct.unpack_from(">II", raw, 24 + 8 * i)
        yield 24 + 8 * i, key, value


def import_record_addresses(raw):
    for _, key, off in opt_headers(raw):
        if key != XEX_IMPORT_LIBRARIES:
            continue
        total, strings_size = struct.unpack_from(">II", raw, off)
        lib = strings_size + 12
        while lib < total:
            size, = struct.unpack_from(">I", raw, off + lib)
            if not size:
                break
            count, = struct.unpack_from(">H", raw, off + lib + 0x26)
            yield from struct.unpack_from(f">{count}I", raw, off + lib + 0x28)
            lib += size


def pe_sections(image):
    lfanew, = struct.unpack_from("<I", image, 0x3C)
    count, opt_size = struct.unpack_from("<H12xH", image, lfanew + 6)
    table = lfanew + 0x18 + opt_size
    for i in range(count):
        name, vsize, rva = struct.unpack_from("<8sII", image, table + 40 * i)
        yield name.rstrip(b"\0").decode(), rva, vsize


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
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1:]
    raw = open(src, "rb").read()
    xex = XexImage.load(src)
    image = bytearray(xex.data)

    keep = {}
    for addr in import_record_addresses(raw):
        if addr and xex.contains(addr, 4):
            keep[addr] = xex.read(addr, 4)
    for name, rva, size in pe_sections(image):
        if name in ZEROED_SECTIONS:
            image[rva:rva + size] = bytes(size)
    for addr, word in keep.items():
        off = addr - xex.base
        image[off:off + 4] = word
    overlay_text(image, xex.base)
    drop_unread(image, xex.base)

    runs = blocks(bytes(image))
    body = b"".join(
        image[off:off + data]
        for off, data in _offsets(runs)
    )

    # The new file format info goes after the old header, so every other
    # header keeps its offset.
    header = bytearray(raw[:struct.unpack_from(">I", raw, 8)[0]])
    header += bytes(-len(header) % 16)
    fmt = struct.pack(">IHH", 8 + 8 * len(runs), 0, 1)
    fmt += b"".join(struct.pack(">II", d, z) for d, z in runs)
    fmt_off = len(header)
    header += fmt
    header += bytes(-len(header) % 0x1000)
    for slot, key, _ in opt_headers(raw):
        if key == XEX_FILE_FORMAT_INFO:
            struct.pack_into(">I", header, slot + 4, fmt_off)
    struct.pack_into(">I", header, 8, len(header))

    with open(dst, "wb") as f:
        f.write(header)
        f.write(body)
    print(f"{dst}: {len(header) + len(body)} bytes, {len(runs)} blocks, "
          f"{len(keep)} import words kept")
    return 0


def _offsets(runs):
    off = 0
    for data, zero in runs:
        yield off, data
        off += data + zero


if __name__ == "__main__":
    sys.exit(main())
