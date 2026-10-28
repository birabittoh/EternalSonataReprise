#!/usr/bin/env python3
"""Convert Eternal Sonata saves between this project, the PS3 and the Xbox 360.

Inputs, by folder or by the file inside:
  * a PS3 save, any region (BLES00444, BLUS30161, BLJS10017), as RPCS3 keeps
    them (SAVEDATA, unencrypted), or as a console writes them (encrypted,
    with PARAM.PFD beside it; needs pycryptodome);
  * an Xbox 360 save, any region, as Xenia keeps them (savedata.txt);
  * one of this project's slots (savedata.txt plus reprise.txt).

Outputs:
  * a slot here (default). PS3 saves become saves for PS3 data;
  * --to 360: a Xenia save container for the PAL, USA or JP release;
  * --to ps3: an RPCS3 save folder for the EU, USA or JP release. Its
    PARAM.SFO is built here; the save list text (no chapter or place, English
    outside Japan) and the icon (the save's picture) are the game's own again
    once it saves.

Whatever the target release lacks is dropped: members 11 and 12 on the 360,
items and magic past its tables (from the inventory, the Item Set and what is
equipped), PS3 costumes and camp flags on the 360, modded costumes and
characters everywhere but here. Other release specific state, such as PS3
only story flags or a save made on a PS3 only map, is carried over as it is.

Both games write the same blocks in the same order, the 360's sub_82241190
and the PS3's sub_3752D0. The USA and JP executables write the PAL layout
unchanged.

    360 (version 2)                PS3 (version 3)
    24      header                 24      header
    204800  thumbnail 320x160      204800  thumbnail
    4       content owned flag     -
    2324    party block            2604    party block
    2048    inventory              2048
    480     base stats, 10 x 48    576     base stats, 12 x 48
    480     live stats             576     live stats
    4412    map state block        4412
    80                             80
    12 x 213140 characters         12 x 213140
    100                            100

The 360 wraps that in "SAV\\0", u32 2, u32 key, u32 end offset, and appends
a u32 check derived from the key (sub_8223D4D8). The PS3's PREVIEW0 is the
first 204824 bytes of SAVEDATA.

The PS3 party block widens position[] and slotbytes[] to twelve and realigns
what follows; it adds costume unlocks and choices (+0x919..+0x91F), the camp
menu flags (+0x920, +0x921) and the score piece marks (+0x928, any number of
them, where the 360 has one at +0x90C). Here, members 11 and 12, the
costumes, the flags and the marks go to reprise.txt
(src/engine/save_record.cpp); a retail 360 save keeps the lowest mark.

usage:
  convert_save.py <input> --slot N [--root DIR] [--profile ID] [--force]
  convert_save.py <input> --to 360 --region pal|usa|jp --out DIR --slot N
  convert_save.py <input> --to ps3 --region eu|usa|jp --out DIR --slot N
  convert_save.py <input> --info
"""

import argparse
import os
import re
import struct
import sys
import zlib

TITLE_IDS = {"pal": 0x4E4D07E2, "usa": 0x4E4D07E4, "jp": 0x4E4D07D5}
TITLE_ID = TITLE_IDS["pal"]

# PS3 save folders are the prefix plus the 0 based slot.
PS3_FOLDERS = {"eu": "BLES00444-", "usa": "BLUS30161", "jp": "BLJS10017TRUSTY-BELL"}
PS3_TITLES = {"eu": "Eternal Sonata\n", "usa": "Eternal Sonata\n",
              "jp": "Trusty Bell ～ショパンの夢～\n"
                    "ルプリーズ"}
NAMES = ["Allegretto", "Polka", "Beat", "Frederic", "Viola", "Salsa", "Jazz", "Falsetto",
         "Claves", "March", "Crescendo", "Serenade"]
# The PS3 EBOOT's own list (Shift JIS at file offset 0x42974A).
NAMES_JP = ["アレグレット", "ポルカ", "ビート",
            "ショパン", "ビオラ", "サルサ",
            "ジルバ", "ファルセット",
            "クラベス", "マーチ",
            "クレッシェンド", "セレナーデ"]
ICON_W, ICON_H = 320, 176

# Crescendo's and Serenade's stats until they join, as every PS3 release
# saves them early on: base, then live.
PS3_NEW_GAME_STATS = (
    "0000001700022A15000000C8000021150000211500540055006E00600192019F00000000007A007D007A007D"
    "00000000000000160001D4CF000000C80000175C0000175C004C004500480059019901A20000000000830082"
    "0083008200000000",
    "0000001700022A15000000C80000211500002115008B005500A600600192019F00000000007A007D007A007D"
    "00000000000000160001D4CF000000C80000175C0000175C007E0045007A0059019901A20000000000830082"
    "0083008200000000",
)

THUMB_W, THUMB_H = 320, 160
THUMB_SIZE = THUMB_W * THUMB_H * 4
HEADER_SIZE = 24
CHAR_SIZE = 213140
CHARS = 12

PS3_SIZE = 2772900

# For a console's protected saves: Bruteforce Save Data's games.conf (one id
# for every release) and the PS3's syscon manager key.
PS3_SECURE_FILE_ID = bytes.fromhex("35FCBD0EF48B9AE723DCCDCE15F2C454")
PS3_SYSCON_MANAGER_KEY = bytes.fromhex("D413B89663E1FE9F75143D3BB4565274")
X360_BODY = 2772432

PS3_BLOCKS = [("party", 2604), ("inventory", 2048), ("stats_base", 576),
              ("stats_live", 576), ("map", 4412), ("b80", 80)]
X360_BLOCKS = [("owned", 4), ("party", 2324), ("inventory", 2048), ("stats_base", 480),
               ("stats_live", 480), ("map", 4412), ("b80", 80)]

STATS_STRIDE = 48
RETAIL_CHARS = 10
TICKS_PER_SECOND = 300

# Highest item and magic ids each release's tables hold (ps3_item_tables.cpp;
# the 360 count is sub_821E51B0's li r9, 0x76).
LIMITS = {"360": (402, 118), "ps3": (431, 132)}

# The 360 party block (offsets from 0x8243F3E8).
POSITION, SLOTBYTES = 0x820, 0x848
ITEM_SET, ITEM_SET_SCRATCH, ITEM_SET_COUNT = 0x856, 0x896, 0x8D8
BUDGET_LEFT, BUDGET_SPENT = 0x8DC, 0x8DD
ITEM_SET_SIZE = 32

# Equipment and equipped magic in the 48 byte stats (docs/equipment.md).
EQUIP = (0x1C, 0x1E, 0x20, 0x22)
MAGIC = (0x24, 0x26, 0x28, 0x2A)

# Costume state in the PS3 party block: unlock byte per (character, variant),
# then the variant each of Allegretto, Polka and Beat wears (1 is the default).
PS3_UNLOCKS = [(0x919, "allegretto", 2), (0x91A, "polka", 2), (0x91B, "beat", 2),
               (0x91C, "polka", 3)]
PS3_WORN = [(0x91D, "allegretto"), (0x91E, "polka"), (0x91F, "beat")]
PS3_CAMP_FLAGS = 0x920
# The score piece marks, u32 bit n - 1 per piece, after six bytes of padding.
PS3_MARKS = 0x928
PS3_PARTY_USED = 0x92C
# The 360 keeps one, the piece number at party +0x90C.
MARK = 0x90C
MARKS_KEY = "score_pieces.marked"

# The PS3's item master records, for Item Set costs (u16 at +0x36).
ITEM_TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "engine",
                          "ps3_item_tables.inc")


class SaveError(Exception):
    pass


def warn(message):
    print(f"warning: {message}", file=sys.stderr)


def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def u16(data, off):
    return struct.unpack_from(">H", data, off)[0]


# ---------------------------------------------------------------------------
# Body layouts
# ---------------------------------------------------------------------------

def parse_body(body, ps3):
    """Split a save body (no 360 wrapper) into named blocks."""
    if len(body) < HEADER_SIZE or u32(body, 0) != HEADER_SIZE + THUMB_SIZE:
        raise SaveError("not an Eternal Sonata save (or still encrypted)")
    version = u32(body, 16)
    if version != (3 if ps3 else 2):
        raise SaveError(f"unexpected save version {version}")
    blocks = {"header": body[:HEADER_SIZE],
              "thumb": body[HEADER_SIZE:HEADER_SIZE + THUMB_SIZE]}
    off = HEADER_SIZE + THUMB_SIZE
    for name, size in PS3_BLOCKS if ps3 else X360_BLOCKS:
        blocks[name] = body[off:off + size]
        off += size
    blocks["chars"] = body[off:off + CHARS * CHAR_SIZE]
    off += CHARS * CHAR_SIZE
    blocks["b100"] = body[off:off + 100]
    if off + 100 != len(body):
        raise SaveError(f"body is {len(body)} bytes, layout expects {off + 100}")
    return blocks


def check_key(key):
    """sub_8223D4D8's check over "SAV\\0" for a given key."""
    rem = 0
    for b in b"SAV\0":
        rem = ((rem << 8) + b) % key
    rem = (rem << 16) % key
    check = 0
    if rem:
        for bit in range(16):
            if ((check + rem) ^ key) & (1 << bit):
                check |= 1 << bit
    return check


def wrap_360(body, key=42):
    head = b"SAV\0" + struct.pack(">III", 2, key, 16 + len(body))
    return head + body + struct.pack(">I", check_key(key))


def unwrap_360(data):
    if data[:4] != b"SAV\0":
        raise SaveError("no SAV header: not a 360 savedata.txt")
    version, key, end = struct.unpack_from(">III", data, 4)
    if version != 2 or end != len(data) - 4:
        raise SaveError("unexpected 360 container header")
    if u32(data, end) != check_key(key):
        warn("360 container check does not match")
    return data[16:end]


# ---------------------------------------------------------------------------
# The save in between: the 360 layout plus everything twelve wide
# ---------------------------------------------------------------------------

class Save:
    """header, thumb, party (360 layout), inventory, map, b80, chars, b100;
    position[12], slotbytes[12], base[12] and live[12] (48 bytes, None when
    unknown); PS3 costume and camp state; the reprise.txt keys nothing here
    models; target ("360" or "ps3", the data the save was made on)."""

    def __init__(self):
        self.unlocks = {}
        self.worn = {}
        self.camp = (0, 0)
        self.marks = 0
        self.passthrough = {}
        self.target = "360"


def stats_rows(block, count):
    return [block[i * STATS_STRIDE:(i + 1) * STATS_STRIDE] for i in range(count)]


def from_ps3(blocks):
    p = blocks["party"]
    party = bytearray(2324)
    party[0x000:0x820] = p[0x000:0x820]
    party[0x820:0x848] = p[0x820:0x848]  # position, characters 1..10
    party[0x848:0x852] = p[0x850:0x85A]  # slotbytes 1..10
    party[0x852:0x8D8] = p[0x85C:0x8E2]  # 3 bytes, Item Set, its scratch copy
    party[0x8D8:0x90D] = p[0x8E4:0x919]  # Item Set count and budget .. score piece Mark
    if any(p[PS3_PARTY_USED:]):
        warn("the PS3 party block has data past +0x92C this converter does not know; "
             "it is dropped")
    s = Save()
    s.target = "ps3"
    for name in ("header", "thumb", "inventory", "map", "b80", "chars", "b100"):
        setattr(s, name, blocks[name])
    s.party = bytes(party)
    s.position = [u32(p, 0x820 + 4 * c) for c in range(CHARS)]
    s.slotbytes = list(p[0x850:0x85C])
    s.base = stats_rows(blocks["stats_base"], CHARS)
    s.live = stats_rows(blocks["stats_live"], CHARS)
    s.unlocks = {(name, variant): bool(p[at]) for at, name, variant in PS3_UNLOCKS}
    s.worn = {name: p[at] for at, name in PS3_WORN}
    s.camp = (p[PS3_CAMP_FLAGS], p[PS3_CAMP_FLAGS + 1])
    s.marks = u32(p, PS3_MARKS)
    return s


def from_360(blocks, record):
    s = Save()
    for name in ("header", "thumb", "party", "inventory", "map", "b80", "chars", "b100"):
        setattr(s, name, blocks[name])
    party = blocks["party"]
    extra = {}
    for key, size in (("position", 8), ("slotbytes", 2), ("stats_base", 96),
                      ("stats_live", 96)):
        value = record.get("party." + key, "")
        extra[key] = bytes.fromhex(value) if len(value) == size * 2 else None
    s.position = [u32(party, POSITION + 4 * c) for c in range(RETAIL_CHARS)]
    s.position += ([u32(extra["position"], 4 * i) for i in range(2)]
                   if extra["position"] else [0, 0])
    s.slotbytes = list(party[SLOTBYTES:SLOTBYTES + RETAIL_CHARS])
    s.slotbytes += list(extra["slotbytes"]) if extra["slotbytes"] else [0, 0]
    s.base = stats_rows(blocks["stats_base"], RETAIL_CHARS)
    s.live = stats_rows(blocks["stats_live"], RETAIL_CHARS)
    s.base += stats_rows(extra["stats_base"], 2) if extra["stats_base"] else [None, None]
    s.live += stats_rows(extra["stats_live"], 2) if extra["stats_live"] else [None, None]
    s.target = record.get("target") or ("ps3" if "ps3.camp_menu_flags" in record else "360")
    if MARKS_KEY in record:
        s.marks = int(record[MARKS_KEY], 16)
    elif 1 <= party[MARK] <= 32:
        s.marks = 1 << (party[MARK] - 1)
    for key, value in record.items():
        m = re.fullmatch(r"unlocked\.(\w+)\.ps3/(\d+)", key)
        if m:
            s.unlocks[(m.group(1), int(m.group(2)))] = value == "1"
            continue
        m = re.fullmatch(r"costume\.(\w+)", key)
        if m and re.fullmatch(r"ps3/\d+", value):
            s.worn[m.group(1)] = int(value[4:])
            continue
        if key == "ps3.camp_menu_flags":
            s.camp = tuple(int(value[i:i + 1] == "1") for i in range(2))
            continue
        if key == MARKS_KEY:
            continue
        if not key.startswith("party.") and key not in ("version", "target"):
            s.passthrough[key] = value
    # Members 11 and 12 are Crescendo and Serenade only on PS3 data.
    if s.target != "ps3":
        for c in (10, 11):
            s.position[c] = s.slotbytes[c] = 0
            s.base[c] = s.live[c] = None
    return s


# ---------------------------------------------------------------------------
# Dropping what a release lacks
# ---------------------------------------------------------------------------

_costs = None


def item_cost(item):
    global _costs
    if _costs is None:
        _costs = {}
        try:
            text = open(ITEM_TABLE).read().split("kPs3MasterRecords[] = {")[1].split("};")[0]
            table = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", text))
            for i in range(0, len(table), 100):
                _costs[u16(table, i)] = u16(table, i + 0x36)
        except (OSError, IndexError):
            pass
    return _costs.get(item)


def drop_items(s, item_max, magic_max):
    """Removes items and magic past a release's tables."""
    inventory = bytearray(s.inventory)
    kept = [inventory[i:i + 4] for i in range(0, len(inventory), 4)
            if 0 < u16(inventory, i) <= item_max]
    dropped = sorted({u16(inventory, i) for i in range(0, len(inventory), 4)
                      if u16(inventory, i) > item_max})
    s.inventory = b"".join(kept).ljust(len(inventory), b"\0")
    if dropped:
        warn(f"items dropped from the inventory: {dropped}")

    party = bytearray(s.party)
    entries = [u16(party, ITEM_SET + 2 * i) for i in range(ITEM_SET_SIZE)]
    entries = [e for e in entries if e]
    removed = [e for e in entries if e > item_max]
    if removed:
        entries = [e for e in entries if e <= item_max]
        refund = 0
        for item in removed:
            cost = item_cost(item)
            if cost is None:
                warn(f"no Item Set cost known for item {item}; its points are not refunded")
            refund += cost or 0
        party[BUDGET_LEFT] = min(255, party[BUDGET_LEFT] + refund)
        party[BUDGET_SPENT] = max(0, party[BUDGET_SPENT] - refund)
        listed = b"".join(struct.pack(">H", e) for e in entries)
        party[ITEM_SET:ITEM_SET + 64] = listed.ljust(64, b"\0")
        party[ITEM_SET_SCRATCH:ITEM_SET_SCRATCH + 66] = listed.ljust(66, b"\0")
        struct.pack_into(">I", party, ITEM_SET_COUNT, len(entries))
        warn(f"items dropped from the Item Set: {removed}")
    s.party = bytes(party)

    for rows in (s.base, s.live):
        for c, row in enumerate(rows):
            if row is None:
                continue
            row = bytearray(row)
            for off, limit in [(o, item_max) for o in EQUIP] + [(o, magic_max) for o in MAGIC]:
                if u16(row, off) > limit:
                    if rows is s.base:
                        warn(f"character {c + 1} unequips {u16(row, off)}")
                    struct.pack_into(">H", row, off, 0)
            rows[c] = bytes(row)


def drop_members(s, members):
    """Takes members out of the party, closing the gaps they leave in the
    display positions as party_arrays.cpp does on load."""
    for c in members:
        if s.position[c]:
            warn(f"character {c + 1} is dropped from the party")
        s.position[c] = s.slotbytes[c] = 0
    order = sorted((p, c) for c, p in enumerate(s.position) if p)
    for next_position, (p, c) in enumerate(order, 1):
        if p != next_position:
            s.position[c] = next_position
            if s.slotbytes[c]:
                s.slotbytes[c] = next_position


def fix_header(s, version):
    """The three members shown in the load menu, the highest level among them,
    as sub_82241190 works them out, and the format version."""
    header = bytearray(s.header)
    best, best_level = 0, 0
    for p in range(3):
        c = next((c for c, pos in enumerate(s.position) if pos == p + 1), None)
        header[4 + p] = c + 1 if c is not None else 0
        if c is not None and s.live[c] is not None and u32(s.live[c], 0) > best_level:
            best, best_level = c + 1, u32(s.live[c], 0)
    header[7] = best
    header[8] = best_level & 0xFF
    struct.pack_into(">I", header, 16, version)
    s.header = bytes(header)


def blank_stats(rows, count):
    return b"".join(rows[c] or bytes(STATS_STRIDE) for c in range(count))


# ---------------------------------------------------------------------------
# Writers
# ---------------------------------------------------------------------------

def lowest_mark(marks):
    return (marks & -marks).bit_length()


def to_360_body(s):
    header = bytearray(s.header)
    struct.pack_into(">I", header, 16, 2)
    party = bytearray(s.party)
    party[MARK] = lowest_mark(s.marks)
    for c in range(RETAIL_CHARS):
        struct.pack_into(">I", party, POSITION + 4 * c, s.position[c])
    party[SLOTBYTES:SLOTBYTES + RETAIL_CHARS] = bytes(s.slotbytes[:RETAIL_CHARS])
    return b"".join([
        bytes(header), s.thumb, b"\0\0\0\0", bytes(party), s.inventory,
        blank_stats(s.base, RETAIL_CHARS), blank_stats(s.live, RETAIL_CHARS),
        s.map, s.b80, s.chars, s.b100,
    ])


def to_project(s):
    """Returns the 360 body and the reprise.txt entries."""
    body = to_360_body(s)
    record = dict(s.passthrough)
    record["version"] = "1"
    record["target"] = s.target
    record[MARKS_KEY] = f"{s.marks:08X}"
    if s.target == "ps3":
        record["party.position"] = b"".join(struct.pack(">I", p)
                                            for p in s.position[10:]).hex().upper()
        record["party.slotbytes"] = bytes(s.slotbytes[10:]).hex().upper()
        if s.base[10] is not None:
            record["party.stats_base"] = blank_stats(s.base[10:], 2).hex().upper()
            record["party.stats_live"] = blank_stats(s.live[10:], 2).hex().upper()
        record["ps3.camp_menu_flags"] = "".join("1" if f else "0" for f in s.camp)
        for (name, variant), unlocked in s.unlocks.items():
            if unlocked:
                record[f"unlocked.{name}.ps3/{variant}"] = "1"
        for name, variant in s.worn.items():
            if variant > 1:
                record[f"costume.{name}"] = f"ps3/{variant}"
    return body, record


def to_retail_360(s):
    if s.passthrough:
        warn("dropped: " + ", ".join(sorted(s.passthrough)))
    if s.marks & (s.marks - 1):
        warn(f"the 360 marks one score piece; only piece {lowest_mark(s.marks)} stays marked")
    drop_members(s, (10, 11))
    s.base[10:] = s.live[10:] = [None, None]
    drop_items(s, *LIMITS["360"])
    fix_header(s, 2)
    return wrap_360(to_360_body(s))


def to_ps3(s):
    """SAVEDATA for the PS3; members 11 and 12 the save has no record of
    start as they do on a new game."""
    if s.passthrough:
        warn("dropped: " + ", ".join(sorted(s.passthrough)))
    for c in (10, 11):
        if s.base[c] is None:
            s.base[c] = bytes.fromhex(PS3_NEW_GAME_STATS[0])[(c - 10) * 48:(c - 9) * 48]
            s.live[c] = bytes.fromhex(PS3_NEW_GAME_STATS[1])[(c - 10) * 48:(c - 9) * 48]
            s.position[c] = s.slotbytes[c] = 0
    if s.target != "ps3":
        s.unlocks, s.worn, s.camp = {}, {}, (0, 0)
    drop_members(s, ())
    drop_items(s, *LIMITS["ps3"])
    fix_header(s, 3)

    q = s.party
    p = bytearray(2604)
    p[0x000:0x820] = q[0x000:0x820]
    for c in range(CHARS):
        struct.pack_into(">I", p, 0x820 + 4 * c, s.position[c])
    p[0x850:0x85C] = bytes(s.slotbytes)
    p[0x85C:0x8E2] = q[0x852:0x8D8]
    p[0x8E4:0x919] = q[0x8D8:0x90D]
    p[0x8E4 + MARK - 0x8D8] = lowest_mark(s.marks)
    struct.pack_into(">I", p, PS3_MARKS, s.marks)
    for at, name, variant in PS3_UNLOCKS:
        p[at] = 1 if s.unlocks.get((name, variant)) else 0
    for at, name in PS3_WORN:
        p[at] = s.worn.get(name, 1) or 1
    p[PS3_CAMP_FLAGS], p[PS3_CAMP_FLAGS + 1] = s.camp
    data = b"".join([
        s.header, s.thumb, bytes(p), s.inventory, blank_stats(s.base, CHARS),
        blank_stats(s.live, CHARS), s.map, s.b80, s.chars, s.b100,
    ])
    assert len(data) == PS3_SIZE
    return data


# ---------------------------------------------------------------------------
# Files around the save
# ---------------------------------------------------------------------------

def png_chunks(width, height, colour_type, rows):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def png(width, height, rgba):
    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return png_chunks(width, height, 6, rows)


def thumbnail_png(argb):
    rgba = bytearray(len(argb))
    rgba[0::4] = argb[1::4]
    rgba[1::4] = argb[2::4]
    rgba[2::4] = argb[3::4]
    rgba[3::4] = b"\xff" * (len(argb) // 4)
    return png(THUMB_W, THUMB_H, bytes(rgba))


def content_header(slot, title_id):
    name = f"savecontent{slot:02}"
    header = bytearray(328)
    struct.pack_into(">II", header, 0, 1, 1)
    display = f"Save File {slot + 1:02}".encode("utf-16-be")
    header[8:8 + len(display)] = display
    header[0x108:0x108 + len(name)] = name.encode()
    struct.pack_into(">I", header, 0x140, title_id)
    return bytes(header)


def write_container(profile_dir, title_id, slot, savedata, record, thumb, force):
    title = os.path.join(profile_dir, f"{title_id:08X}")
    container = os.path.join(title, "00000001", f"savecontent{slot:02}")
    header = os.path.join(title, "Headers", "00000001", f"savecontent{slot:02}.header")
    if os.path.exists(container) and not force:
        raise SaveError(f"slot {slot} is in use ({container}); pass --force to replace it")
    os.makedirs(container, exist_ok=True)
    os.makedirs(os.path.dirname(header), exist_ok=True)
    with open(os.path.join(container, "savedata.txt"), "wb") as f:
        f.write(savedata)
    reprise = os.path.join(container, "reprise.txt")
    if record is not None:
        with open(reprise, "w", newline="\n") as f:
            for key in sorted(record):
                f.write(f"{key} = {record[key]}\n")
    elif os.path.exists(reprise):
        os.remove(reprise)
    with open(os.path.join(container, "__thumbnail.png"), "wb") as f:
        f.write(thumbnail_png(thumb))
    with open(header, "wb") as f:
        f.write(content_header(slot, title_id))
    return container


def build_sfo(entries):
    """PARAM.SFO from (key, format, max length, value) in key order; format
    0x404 is a u32, 0x204 a UTF-8 string, 0x004 raw bytes."""
    keys = b"".join(key.encode() + b"\0" for key, _, _, _ in entries)
    keys += b"\0" * (-len(keys) % 4)
    key_table = 0x14 + 16 * len(entries)
    data_table = key_table + len(keys)
    index, data = b"", b""
    key_off = 0
    for key, fmt, max_length, value in entries:
        if fmt == 0x404:
            value = struct.pack("<I", value)
        elif fmt == 0x204:
            value = value.encode() + b"\0"
        index += struct.pack("<HHIII", key_off, fmt, len(value), max_length, len(data))
        data += value.ljust(max_length, b"\0")
        key_off += len(key) + 1
    header = b"\0PSF" + struct.pack("<IIII", 0x101, key_table, data_table, len(entries))
    return header + index + keys + data


def ps3_sfo(region, slot, savedata):
    """PARAM.SFO as the game writes it, less the chapter and place."""
    seconds = min(u32(savedata, 12) // TICKS_PER_SECOND, 99 * 3600 + 59 * 60 + 59)
    clock = f"{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}"
    leader, level = savedata[7], savedata[8]
    if region == "jp":
        name = NAMES_JP[leader - 1] if 0 < leader <= CHARS else ""
        detail = (f"セーブファイル {slot + 1:02}\n"
                  f"{name} Lv.{level}  TIME {clock}")
    else:
        name = NAMES[leader - 1] if 0 < leader <= CHARS else ""
        detail = f"Save File {slot + 1:02}\n{name} Lv. {level}  Time {clock}"
    return build_sfo([
        ("*ICON0.PNG", 0x404, 4, 0),
        ("*PREVIEW0", 0x404, 4, 1),
        ("*SAVEDATA", 0x404, 4, 1),
        ("ACCOUNT_ID", 0x004, 16, b"0" * 16),
        ("ATTRIBUTE", 0x404, 4, 0),
        ("CATEGORY", 0x204, 4, "SD"),
        ("DETAIL", 0x204, 1024, detail),
        ("PARAMS", 0x204, 1024, ""),
        ("PARAMS2", 0x204, 12, ""),
        ("PARENTAL_LEVEL", 0x404, 4, 0),
        ("RPCS3_BLIST", 0x204, 28, "PREVIEW0/SAVEDATA/ICON0.PNG"),
        ("SAVEDATA_DIRECTORY", 0x204, 32, f"{PS3_FOLDERS[region]}{slot:02}"),
        ("SAVEDATA_LIST_PARAM", 0x204, 8, ""),
        ("SUB_TITLE", 0x204, 128, ""),
        ("TITLE", 0x204, 128, PS3_TITLES[region]),
    ])


def ps3_icon(argb):
    """The save's picture centred on black, at the 320x176 a save icon is."""
    rgb = bytearray(ICON_W * ICON_H * 3)
    top = (ICON_H - THUMB_H) // 2
    for y in range(THUMB_H):
        row = argb[y * THUMB_W * 4:(y + 1) * THUMB_W * 4]
        line = bytearray(THUMB_W * 3)
        line[0::3], line[1::3], line[2::3] = row[1::4], row[2::4], row[3::4]
        at = (top + y) * ICON_W * 3
        rgb[at:at + THUMB_W * 3] = line
    rows = b"".join(b"\0" + rgb[y * ICON_W * 3:(y + 1) * ICON_W * 3] for y in range(ICON_H))
    return png_chunks(ICON_W, ICON_H, 2, rows)


def write_ps3(region, slot, out_dir, savedata, force):
    target = os.path.join(out_dir, f"{PS3_FOLDERS[region]}{slot:02}")
    if os.path.exists(target) and not force:
        raise SaveError(f"{target} exists; pass --force to replace it")
    os.makedirs(target, exist_ok=True)
    files = {
        "SAVEDATA": savedata,
        "PREVIEW0": savedata[:HEADER_SIZE + THUMB_SIZE],
        "ICON0.PNG": ps3_icon(savedata[HEADER_SIZE:HEADER_SIZE + THUMB_SIZE]),
        "PARAM.SFO": ps3_sfo(region, slot, savedata),
    }
    for name, data in files.items():
        with open(os.path.join(target, name), "wb") as f:
            f.write(data)
    return target


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------

def decrypt_protected(pfd, name, data):
    """A file PARAM.PFD lists, as a console writes it. The entry key is CBC
    encrypted under the syscon manager key, with an IV made from the secure
    file id; its first 16 bytes are the file key. Each block is then ECB
    decrypted and XORed with its encrypted index."""
    try:
        from Crypto.Cipher import AES
    except ImportError:
        raise SaveError("decrypting a console's PS3 save needs pycryptodome")
    if pfd[:8] != b"\0\0\0\0PFDB":
        raise SaveError("PARAM.PFD is not a PFD")
    capacity, _, used = struct.unpack_from(">QQQ", pfd, 0x60)
    entries = 0x60 + 24 + capacity * 8
    for n in range(used):
        entry = pfd[entries + n * 272:entries + (n + 1) * 272]
        if entry[8:8 + 65].split(b"\0")[0] != name.encode():
            continue
        sfid = PS3_SECURE_FILE_ID
        iv = sfid[:1] + bytes([11, 15]) + sfid[1:3] + bytes([14]) + sfid[3:5] + bytes([10]) + sfid[5:12]
        key = AES.new(PS3_SYSCON_MANAGER_KEY, AES.MODE_CBC, iv).decrypt(entry[80:144])[:16]
        size = struct.unpack_from(">Q", entry, 264)[0]
        if len(data) % 16 or size > len(data):
            raise SaveError(f"{name} does not match its PARAM.PFD entry")
        aes = AES.new(key, AES.MODE_ECB)
        counters = aes.encrypt(b"".join(struct.pack(">QQ", i, 0) for i in range(len(data) // 16)))
        plain = aes.decrypt(data)
        out = (int.from_bytes(plain, "big") ^ int.from_bytes(counters, "big")).to_bytes(len(data), "big")
        return out[:size]
    raise SaveError(f"PARAM.PFD does not list {name}")


def read_record(path):
    record = {}
    if os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\r\n")
            if " = " in line and not line.startswith("#"):
                key, value = line.split(" = ", 1)
                record[key] = value
    return record


def load(path):
    """A Save from a PS3 SAVEDATA, a 360 savedata.txt, or their folders."""
    folder = path if os.path.isdir(path) else os.path.dirname(path)
    if os.path.isdir(path):
        for name in ("SAVEDATA", "savedata.txt"):
            if os.path.isfile(os.path.join(path, name)):
                path = os.path.join(path, name)
                break
        else:
            raise SaveError(f"no SAVEDATA or savedata.txt in {path}")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) == (PS3_SIZE + 15) // 16 * 16:
        pfd = os.path.join(folder, "PARAM.PFD")
        if not os.path.isfile(pfd):
            raise SaveError(f"{path} is encrypted and has no PARAM.PFD beside it")
        with open(pfd, "rb") as f:
            data = decrypt_protected(f.read(), "SAVEDATA", data)
    if data[:4] == b"SAV\0":
        record = read_record(os.path.join(folder, "reprise.txt"))
        return from_360(parse_body(unwrap_360(data), ps3=False), record)
    if len(data) == PS3_SIZE:
        return from_ps3(parse_body(data, ps3=True))
    raise SaveError(f"{path}: unrecognised save ({len(data)} bytes)")


def describe(s):
    seconds = u32(s.header, 12) // TICKS_PER_SECOND
    members = [c + 1 for c, p in sorted(enumerate(s.position), key=lambda x: x[1]) if p]
    return (f"{s.target} data, party level {u32(s.party, 4)}, play time "
            f"{seconds // 3600}:{seconds // 60 % 60:02}:{seconds % 60:02}, "
            f"members {members}")


def save_root(root, profile):
    if profile:
        return os.path.join(root, profile)
    candidates = [d for d in os.listdir(root)
                  if os.path.isdir(os.path.join(root, d, f"{TITLE_ID:08X}"))]
    if len(candidates) != 1:
        raise SaveError(f"{root} holds {len(candidates)} profiles with saves; "
                        "pick one with --profile")
    return os.path.join(root, candidates[0])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="a PS3 save, a 360 save or a slot here (folder or file)")
    parser.add_argument("--to", choices=("project", "360", "ps3"), default="project")
    parser.add_argument("--slot", type=int, help="destination slot, 0..9 (Save File 01..10)")
    parser.add_argument("--root", default=os.path.join(os.path.expanduser("~"), "Documents",
                                                        "eternalsonata"),
                        help="this project's user data root (default: %(default)s)")
    parser.add_argument("--profile", help="profile folder under the root, if there are several")
    parser.add_argument("--region", choices=("eu", "pal", "usa", "jp"),
                        help="--to 360 or ps3: the release (eu and pal are the same)")
    parser.add_argument("--out", help="--to 360: a Xenia profile folder (content/<xuid>); "
                                      "--to ps3: RPCS3's dev_hdd0/home/<user>/savedata")
    parser.add_argument("--force", action="store_true", help="replace an existing save")
    parser.add_argument("--info", action="store_true", help="only describe the input")
    args = parser.parse_args(argv)

    try:
        save = load(args.input)
        print(describe(save))
        if args.info:
            return 0
        if args.slot is None or not 0 <= args.slot <= 9:
            parser.error("--slot 0..9 is required")
        if args.to != "project" and (not args.region or not args.out):
            parser.error(f"--to {args.to} needs --region and --out")
        if args.to == "ps3":
            region = "eu" if args.region == "pal" else args.region
            container = write_ps3(region, args.slot, args.out, to_ps3(save), args.force)
        elif args.to == "360":
            region = "pal" if args.region == "eu" else args.region
            data = to_retail_360(save)
            container = write_container(args.out, TITLE_IDS[region], args.slot, data, None,
                                        save.thumb, args.force)
        else:
            body, record = to_project(save)
            container = write_container(save_root(args.root, args.profile), TITLE_ID, args.slot,
                                        wrap_360(body), record, save.thumb, args.force)
            if save.target == "ps3":
                print("load it with PS3 data (--game_version=ps3-...)")
        print(f"wrote {container}")
    except (SaveError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
