#!/usr/bin/env python3
"""Write the PS3's item and magic tables as source: the records as C++
(src/engine/ps3_item_tables.inc), the text as src/guest_data/ps3/text/ in
guest_data.py's format, compiled by scripts/gen-ps3-item-text.py.

The PS3 keeps the 360's layouts, filled for twelve characters:

* item master records (100 bytes, 0x46D2E8): items 402..431 are new, mostly
  Crescendo's and Serenade's gear, and many shared ones are rebalanced. Their
  equip bits are 22 and 23; the 360 code tests 4 << c, so they move to 13
  and 14.
* magic records (12 bytes, 0x47A598): 119..132 belong to 11 and 12.
* magic display order (u16[11] per character, 0x468DA8), twelve rows.
* item icons (u16 image ids, 0x468CDC, read at icon - 1): the PS3 inserts
  its new ones at 48, where the 360's table runs into the camp portraits.
* the item and magic name and description BTX blocks, each file headed by
  the 360 block it replaces. Their British English strings are empty for
  everything new, so those fall back to the US ones.

The output is committed; the build does not run this.

usage:
    python scripts/ps3_item_tables.py EBOOT.elf
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from btx import LANGS  # noqa: E402
from guest_data import read_text, write_text  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "src", "engine", "ps3_item_tables.inc")
TEXT_DIR = os.path.join(ROOT, "src", "guest_data", "ps3", "text")

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
# PS3 block, 360 block it replaces, its file (the 360 one's name).
BLOCKS = [
    (0x759470, 0x82376400, "items"),
    (0x719920, 0x8233A3A8, "item_descriptions"),
    (0x6EABE0, 0x8230F640, "party_skills"),
    (0x6D9588, 0x822FF598, "party_skill_help"),
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


def array(name, data, row):
    lines = ["constexpr uint8_t %s[] = {" % name]
    for i in range(0, len(data), row):
        lines.append(" ".join("0x%02X," % b for b in data[i:i + row]))
    lines.append("};")
    return lines


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    elf = open(sys.argv[1], "rb").read()
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
        "// Generated by scripts/ps3_item_tables.py from the PS3 EBOOT, one record per row.",
    ]
    lines += array("kPs3MasterRecords", master, MASTER_RECORD)
    lines += array("kPs3MagicRecords", magic, MAGIC_RECORD)
    lines += array("kPs3MagicOrder", order, ORDER_ROW)
    last = max(master[MASTER_RECORD * k + 2] for k in range(MASTER_COUNT))
    icons = elf[file_offset(ICONS_VA) + 2 * ICONS_FIRST:file_offset(ICONS_VA) + 2 * last]
    lines.append("constexpr uint32_t kPs3ItemIconFirst = %d;" % ICONS_FIRST)
    lines.append("constexpr uint16_t kPs3ItemIcons[] = {%s};"
                 % ", ".join(str(v) for v in struct.unpack(">%dH" % (len(icons) // 2), icons)))
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.makedirs(TEXT_DIR, exist_ok=True)
    for ps3, x360, name in BLOCKS:
        langs = read_btx(elf, file_offset(ps3))
        if [l for l, _ in langs] != LANGS:
            raise ValueError("%s: unexpected languages" % name)
        langs, filled = fill_gbr(langs)
        langs = {l.strip(): [s for _, s in strings] for l, strings in langs}
        path = os.path.join(TEXT_DIR, name + ".txt")
        write_text(path, x360, langs)
        if read_text(path) != (x360, langs):
            raise ValueError("%s does not round trip" % path)
        print("%s: %d British strings from the US ones" % (name, filled))
    print("wrote the PS3 item and magic tables to %s and %s" % (OUT, TEXT_DIR))


if __name__ == "__main__":
    main()
