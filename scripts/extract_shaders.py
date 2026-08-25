#!/usr/bin/env python3
"""Extract the game's whole shader inventory from default.xex, offline.

Every shader the game can bind lives in one static chunked blob compiled into
the executable, starting at the 'NVSH' tag near 0x8238EC50. sub_82129260 walks
it once at init, creates each shader, and stores the results into the 256 entry
tables dword_824BBEE8 (vertex) and dword_824BC2E8 (pixel) starting at index 1.
Nothing is ever created lazily, so this parse sees everything the game has.

Chunk layout:
    +0   'NVSH' or 'NPSH'
    +4   chunk size, advances to the next chunk
    +16  shader container, magic 0x102A1101

Container layout, as read by the creators sub_82266598 / sub_82266488:
    +0   magic
    +4   header size, which is also the offset to the microcode
    +8   microcode size
    +20  offset to the patch table

The patch table matters: SetVertexShader (0x822660D8) walks it at bind time to
write texture fetch constants into the device and to apply register overrides.
A shader is microcode *plus* that baked state, so both halves are written out.

Usage:
    python scripts/extract_shaders.py [--xex assets/default.xex]
                                      [--out out/shaders] [--list]
"""
import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_image import XexImage  # noqa: E402

BLOB_START = 0x8238EC50

TAG_VERTEX = b"NVSH"
TAG_PIXEL = b"NPSH"
# The container magic's low bit is a flag, not part of the magic. Both creators
# allocate `(magic & 1) ? 872 : 40` bytes plus the header, and in this blob the
# bit tracks the tag: NVSH containers set it, NPSH containers do not.
CONTAINER_MAGIC = 0x102A1100
CONTAINER_MAGIC_MASK = ~1 & 0xFFFFFFFF
CONTAINER_OFFSET = 16

# Where each creator copies the container header inside the shader object:
# sub_82266598 (vertex) writes it at +872, sub_82266488 (pixel) at +40. The
# patch table field sits 20 bytes into the header, which is why SetVertexShader
# reads it at object+892.
HEADER_OFFSET_IN_OBJECT = {1: 872, 0: 40}
PATCH_FIELD_IN_HEADER = 20

# The table index the game stores into starts at 1; entry 0 stays null.
FIRST_INDEX = 1
MAX_INDEX = 0xFF

SHADER_MODEL_RE = re.compile(rb"[vp]s_\d_\d")


# Texture fetch constants live at device+1024, six dwords (24 bytes) per stage.
FETCH_CONSTANT_BASE = 1024
FETCH_CONSTANT_STRIDE = 24


def decode_patch_table(container, offset):
    """Decode the bind-time patch table, as walked by SetVertexShader
    (0x822660D8) and SetPixelShader (0x82265E20).

    Header, relative to the table base (which is container relative):
        +0   value applied to the device dirty mask
        +4   clear mask, ANDed as ~mask into the neighbouring mask dword
        +8   not read by either bind path
        +12  flag; if non-zero the binder sets bit 0x01000000 in the device+32 mask
        +16  byte length of the record stream that follows
        +20  record stream

    The stream is three consecutive sections, each a list of records starting
    with u16 dst_offset, u16 count, and each terminated by a count of zero:

      1. skipped by *both* bind paths; fixed 8 byte records
      2. copy: `count` dwords written straight to device+1024+dst_offset
      3. merge: `count/2` (and_mask, or_value) pairs read-modify-written into
         device+1024+dst_offset

    Sections 2 and 3 are the texture fetch constants, so dst_offset/24 is the
    texture stage. This is the "baked state" half of a shader.
    """
    out = {
        "mask_value": struct.unpack_from(">I", container, offset)[0],
        "mask_clear": struct.unpack_from(">I", container, offset + 4)[0],
        "unread_8": struct.unpack_from(">I", container, offset + 8)[0],
        "flag_12": struct.unpack_from(">I", container, offset + 12)[0],
    }
    stream_size, = struct.unpack_from(">I", container, offset + 16)
    out["stream_size"] = stream_size

    pos = offset + 20
    end = pos + stream_size
    if end > len(container):
        raise ValueError("patch table stream runs past the container")

    def read_record():
        nonlocal pos
        dst, count = struct.unpack_from(">HH", container, pos)
        pos += 4
        return dst, count

    # Section 1: skipped, fixed 8 byte records.
    skipped = []
    while pos < end:
        dst, count = read_record()
        if not count:
            break
        skipped.append({
            "dst_offset": dst,
            "count": count,
            "value": struct.unpack_from(">I", container, pos)[0],
        })
        pos += 4
    out["section1_skipped"] = skipped

    # Section 2: raw dword copies into the fetch constant array.
    copies = []
    while pos < end:
        dst, count = read_record()
        if not count:
            break
        words = list(struct.unpack_from(f">{count}I", container, pos))
        pos += 4 * count
        copies.append({
            "dst_offset": dst,
            "stage": dst // FETCH_CONSTANT_STRIDE,
            "dwords": [f"0x{w:08X}" for w in words],
        })
    out["fetch_constant_copies"] = copies

    # Section 3: masked read-modify-write into the same array.
    merges = []
    while pos < end:
        dst, count = read_record()
        if not count:
            break
        pairs = []
        for _ in range(count // 2):
            and_mask, or_value = struct.unpack_from(">II", container, pos)
            pos += 8
            pairs.append({"and": f"0x{and_mask:08X}", "or": f"0x{or_value:08X}"})
        merges.append({
            "dst_offset": dst,
            "stage": dst // FETCH_CONSTANT_STRIDE,
            "pairs": pairs,
        })
    out["fetch_constant_merges"] = merges
    out["stream_consumed"] = pos - (offset + 20)
    return out


class Shader:
    def __init__(self, kind, index, addr, chunk_size, container):
        self.kind = kind
        self.index = index
        self.addr = addr
        self.chunk_size = chunk_size
        self.container = container

        magic, header_size, microcode_size = struct.unpack_from(">III", container)
        self.magic = magic
        self.header_size = header_size
        self.microcode_size = microcode_size
        self.patch_table_offset, = struct.unpack_from(
            ">I", container, PATCH_FIELD_IN_HEADER)
        self.object_header_offset = HEADER_OFFSET_IN_OBJECT[magic & 1]
        self.header = container[:header_size]
        self.microcode = container[header_size:header_size + microcode_size]

        model = SHADER_MODEL_RE.search(self.header)
        self.model = model.group(0).decode() if model else None

        self.patch_table = None
        if self.patch_table_offset:
            self.patch_table = decode_patch_table(container, self.patch_table_offset)

    @property
    def name(self):
        return f"{self.kind}_{self.index:03d}"

    @property
    def container_addr(self):
        return self.addr + CONTAINER_OFFSET

    def manifest_entry(self):
        return {
            "name": self.name,
            "kind": self.kind,
            "table_index": self.index,
            "shader_model": self.model,
            "container_magic": f"0x{self.magic:08X}",
            "header_offset_in_object": self.object_header_offset,
            "chunk_addr": f"0x{self.addr:08X}",
            "container_addr": f"0x{self.container_addr:08X}",
            "chunk_size": self.chunk_size,
            "header_size": self.header_size,
            "microcode_size": self.microcode_size,
            "patch_table_offset": self.patch_table_offset,
            "patch_table_addr": (
                f"0x{self.container_addr + self.patch_table_offset:08X}"
                if self.patch_table_offset else None
            ),
            "patch_table": self.patch_table,
        }


def parse_blob(img, start=BLOB_START):
    """Walk the chunk list exactly as sub_82129260 does, and stop where it does."""
    shaders = []
    counters = {"vs": FIRST_INDEX, "ps": FIRST_INDEX}
    addr = start
    while True:
        if not img.contains(addr, 8):
            break
        tag = img.read(addr, 4)
        if tag == TAG_VERTEX:
            kind = "vs"
        elif tag == TAG_PIXEL:
            kind = "ps"
        else:
            break  # the loop's own termination condition

        chunk_size = img.u32(addr + 4)
        if chunk_size <= CONTAINER_OFFSET or not img.contains(addr, chunk_size):
            raise ValueError(f"bad chunk size {chunk_size} at 0x{addr:08X}")

        container = img.read(addr + CONTAINER_OFFSET, chunk_size - CONTAINER_OFFSET)
        magic, = struct.unpack_from(">I", container)
        if magic & CONTAINER_MAGIC_MASK != CONTAINER_MAGIC:
            raise ValueError(
                f"bad container magic 0x{magic:08X} at 0x{addr + CONTAINER_OFFSET:08X}"
            )

        index = counters[kind]
        if index > MAX_INDEX:
            raise ValueError(f"more than {MAX_INDEX} {kind} shaders; table overflow")
        counters[kind] += 1

        shaders.append(Shader(kind, index, addr, chunk_size, container))
        addr += chunk_size
    return shaders, addr


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xex", default="assets/default.xex")
    ap.add_argument("--out", default="out/shaders")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=BLOB_START,
                    help="blob start address (default 0x8238EC50)")
    ap.add_argument("--list", action="store_true",
                    help="only print the inventory, write nothing")
    args = ap.parse_args()

    img = XexImage.load(args.xex)
    shaders, end = parse_blob(img, args.start)

    counts = {"vs": 0, "ps": 0}
    for s in shaders:
        counts[s.kind] += 1
    print(f"blob 0x{args.start:08X}..0x{end:08X} "
          f"({end - args.start} bytes): {counts['vs']} vertex, "
          f"{counts['ps']} pixel, {len(shaders)} total")

    if args.list:
        for s in shaders:
            patch = (f"patch +0x{s.patch_table_offset:X}"
                     if s.patch_table_offset else "no patch table")
            print(f"  {s.name}  0x{s.addr:08X}  {s.model or '?':6}  "
                  f"hdr {s.header_size:5}  code {s.microcode_size:6}  {patch}")
        return 0

    os.makedirs(args.out, exist_ok=True)
    for s in shaders:
        base = os.path.join(args.out, s.name)
        # Full container first: it is the self-contained artifact, holding the
        # microcode and the baked state together.
        with open(base + ".bin", "wb") as f:
            f.write(s.container)
        with open(base + ".ucode", "wb") as f:
            f.write(s.microcode)
        with open(base + ".header", "wb") as f:
            f.write(s.header)

    manifest = {
        "source": os.path.basename(args.xex),
        "image_base": f"0x{img.base:08X}",
        "blob_start": f"0x{args.start:08X}",
        "blob_end": f"0x{end:08X}",
        "counts": counts,
        "vertex_table": "0x824BBEE8",
        "pixel_table": "0x824BC2E8",
        "first_table_index": FIRST_INDEX,
        "shaders": [s.manifest_entry() for s in shaders],
    }
    manifest_path = os.path.join(args.out, "shaders.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"wrote {len(shaders) * 3} files plus {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
