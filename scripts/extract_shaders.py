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

The patch table matters: SetVertexShader (0x822660D8) walks it at bind time to
write texture fetch constants into the device and to apply register overrides.
A shader is microcode *plus* that baked state, so both halves are written out.

Usage:
    python scripts/extract_shaders.py [--xex assets/default.xex]
        [--out out/shaders] [--start 0x8238EC50] [--list]
"""
import argparse
import json
import os
import sys

# The shared container parsing lives in the KryptonRecomp submodule.
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "KryptonRecomp"))
from shader_extract import (FIRST_INDEX, MAX_INDEX,  # noqa: E402
                            XexImage, parse_blob, write_shader_files)

# The 'NVSH' tag that starts the chunked inventory sub_82129260 walks.
BLOB_START = 0x8238EC50


def manifest_entry(s):
    return {
        "name": s.name,
        "kind": s.kind,
        "table_index": s.index,
        "shader_model": s.model,
        "container_magic": f"0x{s.magic:08X}",
        "header_offset_in_object": s.object_header_offset,
        "chunk_addr": f"0x{s.addr:08X}",
        "container_addr": f"0x{s.container_addr:08X}",
        "chunk_size": s.chunk_size,
        "header_size": s.header_size,
        "microcode_size": s.microcode_size,
        "patch_table_offset": s.patch_table_offset,
        "patch_table_addr": (
            f"0x{s.container_addr + s.patch_table_offset:08X}"
            if s.patch_table_offset else None
        ),
        "patch_table": s.patch_table,
    }


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

    write_shader_files(shaders, args.out)

    manifest = {
        "source": os.path.basename(args.xex),
        "image_base": f"0x{img.base:08X}",
        "blob_start": f"0x{args.start:08X}",
        "blob_end": f"0x{end:08X}",
        "counts": counts,
        "vertex_table": "0x824BBEE8",
        "pixel_table": "0x824BC2E8",
        "first_table_index": FIRST_INDEX,
        "shaders": [manifest_entry(s) for s in shaders],
    }
    manifest_path = os.path.join(args.out, "shaders.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"wrote {len(shaders) * 3} files plus {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
