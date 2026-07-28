#!/usr/bin/env python3
"""Parse the structure of a *decoded* .e file (post unpack_e).

Layout was derived from sub_820FF9C8 / sub_820FF6C0 in default.xex and
validated against all 678 decoded .e files.

    +0x00 u32be  0x00000181            magic (loader accepts 0x180 | 0x181)
    +0x04 u32be  id/hash
    +0x08 u32be  ?
    +0x0C u32be  total size            == len(file)
    +0x10 u32be  image_size - 0x18     image = file[0x18 : 0x18+this]
    +0x14 u32be  reloc_off             reloc tables at image_end + this
    +0x18        image (bytecode)      copied+relocated at load time

    image_end            = 0x18 + hdr[0x10]
    bulk                 = file[image_end : reloc_base]   (text/resources,
                                                          NOT loaded by the
                                                          loader; streamed)
    reloc_base           = image_end + hdr[0x14]
      listA: u32 count, u32 offsets[count]   -> patched with image base
      listB: u32 count, u32 offsets[count]   -> patched, second class
    block2 = rest of file: four chained {u32 count, entries...} fixup tables
             consumed by sub_820FF748 / sub_820FF838 / sub_820FF910 x2.
             First one is {u32 symbol_id, u32 patch_offset} pairs, sorted
             by symbol_id.

usage: python scripts/e_layout.py <decoded.e> [...]
"""
import struct
import sys


def parse(data):
    magic, ident, unk8, total, imgsz, relofs = struct.unpack(">6I", data[:24])
    image_end = 0x18 + imgsz
    reloc_base = image_end + relofs

    def table(off):
        (n,) = struct.unpack_from(">I", data, off)
        return struct.unpack_from(">%dI" % n, data, off + 4), off + 4 + 4 * n

    a, off = table(reloc_base)
    b, off = table(off)
    return {
        "magic": magic,
        "ident": ident,
        "unk8": unk8,
        "total": total,
        "size_ok": total == len(data),
        "image": (0x18, image_end),
        "bulk": (image_end, reloc_base),
        "reloc_a": a,
        "reloc_b": b,
        "block2": (off, len(data)),
    }


def main(argv):
    for path in argv:
        d = open(path, "rb").read()
        i = parse(d)
        print(
            "%s\n  magic=0x%x id=0x%08x size=0x%x (%s)\n"
            "  image=0x%x..0x%x  bulk=0x%x..0x%x (0x%x)\n"
            "  relocA=%d relocB=%d  block2=0x%x..0x%x"
            % (
                path, i["magic"], i["ident"], i["total"],
                "ok" if i["size_ok"] else "MISMATCH",
                i["image"][0], i["image"][1],
                i["bulk"][0], i["bulk"][1], i["bulk"][1] - i["bulk"][0],
                len(i["reloc_a"]), len(i["reloc_b"]),
                i["block2"][0], i["block2"][1],
            )
        )


if __name__ == "__main__":
    main(sys.argv[1:])
