#!/usr/bin/env python3
"""Write the PS3's item and magic tables as C++ source
(src/engine/ps3_item_tables.generated.inc).

The PS3 keeps the 360's layouts, filled for twelve characters:

* item master records (100 bytes, 0x46D2E8): items 402..431 are new, mostly
  Crescendo's and Serenade's gear, and many shared ones are rebalanced. Their
  equip bits are 22 and 23; the 360 code tests 4 << c, so they move to 13
  and 14.
* magic records (12 bytes, 0x47A598): 119..132 belong to 11 and 12.
* magic display order (u16[11] per character, 0x468DA8), twelve rows.
* item icons (u16 image ids, 0x468CDC, read at icon - 1): the PS3 inserts
  its new ones at 48, where the 360's table runs into the camp portraits.
* the item and magic name and description BTX blocks. Their British English
  strings are empty for everything new, so those fall back to the US ones.

Without the EBOOT the file is written empty, and PS3 mode keeps the 360's.

usage:
    python scripts/ps3_item_tables.py [EBOOT.elf [output]]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from btx import LANGS  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "src", "engine", "ps3_item_tables.generated.inc")
EBOOT = os.path.join(ROOT, "assets", "EBOOT.elf")

MASTER_VA = 0x46D2E8
MASTER_RECORD = 100
MASTER_COUNT = 431
MAGIC_VA = 0x47A598
MAGIC_RECORD = 12
MAGIC_COUNT = 132
ORDER_VA = 0x468DA8
ORDER_ROW = 22
CHARACTERS = 12
ICONS_VA = 0x468CDC
ICONS_FIRST = 48
# PS3 block, 360 block it replaces, what it holds.
BLOCKS = [
    (0x759470, 0x82376400, "item names"),
    (0x719920, 0x8233A3A8, "item descriptions"),
    (0x6EABE0, 0x8230F640, "magic names"),
    (0x6D9588, 0x822FF598, "magic descriptions"),
]
PS3_EQUIP_BITS = {22: 13, 23: 14}


def read_btx(data, base):
    """{lang: [(id, bytes)]}, in file order."""
    first, _, count = struct.unpack_from(">III", data, base + 4)
    out = []
    q = base + first
    for _ in range(count):
        lang = data[q:q + 4].decode("ascii")
        table, following, _, entries = struct.unpack_from(">IIII", data, q + 4)
        strings = []
        for i in range(entries):
            sid, offset = struct.unpack_from(">II", data, q + table + 8 * i)
            start = q + offset
            strings.append((sid, data[start:data.index(0, start)]))
        out.append((lang, strings))
        q += following
    return out


def write_btx(langs):
    blocks = []
    for lang, strings in langs:
        table = 20
        text = 20 + 8 * len(strings)
        entries = b""
        body = b""
        for sid, s in strings:
            entries += struct.pack(">II", sid, text + len(body))
            body += s + b"\0"
        size = text + len(body)
        blocks.append(struct.pack(">4sIIII", lang.encode("ascii"), table, size, 0, len(strings))
                      + entries + body)
    payload = b"".join(blocks)
    return struct.pack(">4sIII", b"BTX ", 16, 16 + len(payload), len(langs)) + payload


def fill_gbr(langs):
    usa = dict(next(s for l, s in langs if l == "USA "))
    filled = 0
    out = []
    for lang, strings in langs:
        if lang == "GBR ":
            fixed = []
            for sid, s in strings:
                if not s and usa.get(sid):
                    s = usa[sid]
                    filled += 1
                fixed.append((sid, s))
            strings = fixed
        out.append((lang, strings))
    return out, filled


def array(name, data):
    lines = ["constexpr uint8_t %s[] = {" % name]
    for i in range(0, len(data), 32):
        lines.append(",".join(str(b) for b in data[i:i + 32]) + ",")
    lines.append("};")
    return lines


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else EBOOT
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    if not os.path.isfile(path):
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write("// No PS3 EBOOT.elf at build time: empty.\n"
                    "#define ETERNALSONATA_PS3_ITEM_TABLES 0\n")
        print("warning: %s not found, wrote an empty %s" % (path, out))
        return
    elf = open(path, "rb").read()
    phoff = struct.unpack(">Q", elf[0x20:0x28])[0]
    phes, phnum = struct.unpack(">HH", elf[0x36:0x3A])
    segments = []
    for i in range(phnum):
        o = phoff + i * phes
        kind, _, offset, va, _, filesz, _ = struct.unpack(">IIQQQQQ", elf[o:o + 48])
        if kind == 1:
            segments.append((va, offset, filesz))

    def file_offset(va):
        for start, offset, size in segments:
            if start <= va < start + size:
                return offset + va - start
        raise ValueError("unmapped address %#x" % va)

    master = bytearray(elf[file_offset(MASTER_VA):][:MASTER_RECORD * MASTER_COUNT])
    for k in range(MASTER_COUNT):
        record = MASTER_RECORD * k
        if struct.unpack_from(">H", master, record)[0] != k + 1:
            raise ValueError("master record %d does not hold id %d" % (k, k + 1))
        flags = struct.unpack_from(">I", master, record + 4)[0]
        for ps3, x360 in PS3_EQUIP_BITS.items():
            if flags & (1 << x360):
                raise ValueError("item %d already uses bit %d" % (k + 1, x360))
            if flags & (1 << ps3):
                flags = flags & ~(1 << ps3) | (1 << x360)
        struct.pack_into(">I", master, record + 4, flags)

    magic = elf[file_offset(MAGIC_VA):][:MAGIC_RECORD * MAGIC_COUNT]
    for k in range(MAGIC_COUNT):
        if struct.unpack_from(">H", magic, MAGIC_RECORD * k + 2)[0] != k + 1:
            raise ValueError("magic record %d does not hold id %d" % (k, k + 1))
    order = elf[file_offset(ORDER_VA):][:ORDER_ROW * CHARACTERS]
    if struct.unpack_from(">H", order, ORDER_ROW * 10)[0] != 122:
        raise ValueError("magic order row 11 is not where expected")

    lines = [
        "// Generated by scripts/ps3_item_tables.py from the PS3 EBOOT. Do not edit.",
        "#define ETERNALSONATA_PS3_ITEM_TABLES 1",
    ]
    lines += array("kPs3MasterRecords", master)
    lines += array("kPs3MagicRecords", magic)
    lines += array("kPs3MagicOrder", order)
    last = max(master[MASTER_RECORD * k + 2] for k in range(MASTER_COUNT))
    icons = elf[file_offset(ICONS_VA) + 2 * ICONS_FIRST:file_offset(ICONS_VA) + 2 * last]
    lines.append("constexpr uint32_t kPs3ItemIconFirst = %d;" % ICONS_FIRST)
    lines.append("constexpr uint16_t kPs3ItemIcons[] = {%s};"
                 % ", ".join(str(v) for v in struct.unpack(">%dH" % (len(icons) // 2), icons)))
    names = []
    for i, (ps3, x360, what) in enumerate(BLOCKS):
        langs = read_btx(elf, file_offset(ps3))
        if [l for l, _ in langs] != LANGS:
            raise ValueError("%s: unexpected languages" % what)
        langs, filled = fill_gbr(langs)
        blob = write_btx(langs)
        lines.append("// %s, replacing 0x%08X; %d British strings from the US ones" % (what, x360, filled))
        lines += array("kPs3TextBlock%d" % i, blob)
        names.append("{0x%08Xu, kPs3TextBlock%d, sizeof(kPs3TextBlock%d)}" % (x360, i, i))
    lines.append("constexpr Ps3TextBlock kPs3TextBlocks[] = {%s};" % ", ".join(names))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote the PS3 item and magic tables to %s" % out)


if __name__ == "__main__":
    main()
