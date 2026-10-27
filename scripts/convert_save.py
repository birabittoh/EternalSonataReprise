#!/usr/bin/env python3
"""Convert an Eternal Sonata save from another release into a save slot here.

Accepted inputs:
  * a PS3 save, any region (BLES00444, BLUS30161, BLJS10017): its directory,
    or the SAVEDATA file inside it, as RPCS3 keeps them (unencrypted);
  * an Xbox 360 container, any region, as Xenia keeps them: its directory, or
    the savedata.txt inside. The USA and JP executables write the PAL layout
    unchanged, so these are copied as they are.

PS3 saves become saves for PS3 data (target = ps3 in reprise.txt).

Both games write the same blocks in the same order, the 360's
sub_82241190 and the PS3's sub_3752D0:

    360 PAL (version 2)            PS3 (version 3)
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
a u32 check derived from the key (sub_8223D4D8).

The PS3 party block widens position[] and slotbytes[] to twelve and realigns
what follows; it adds costume unlocks and choices (+0x919..+0x91F) and the
camp menu flags (+0x920, +0x921). Members 11 and 12, the costumes and the
flags have no room in the 360 layout, so they go to reprise.txt
(src/engine/save_record.cpp).

usage:
  convert_save.py <input> --slot N [--root DIR] [--profile ID] [--force]
  convert_save.py <input> --info
"""

import argparse
import os
import struct
import sys
import zlib

TITLE_ID = 0x4E4D07E2

THUMB_W, THUMB_H = 320, 160
THUMB_SIZE = THUMB_W * THUMB_H * 4
HEADER_SIZE = 24
CHAR_SIZE = 213140
CHARS = 12

PS3_SIZE = 2772900
X360_BODY = 2772432

PS3_BLOCKS = [("party", 2604), ("inventory", 2048), ("stats_base", 576),
              ("stats_live", 576), ("map", 4412), ("b80", 80)]
X360_BLOCKS = [("owned", 4), ("party", 2324), ("inventory", 2048), ("stats_base", 480),
               ("stats_live", 480), ("map", 4412), ("b80", 80)]

STATS_STRIDE = 48
RETAIL_CHARS = 10

# Costume state in the PS3 party block: unlock byte per (character, variant),
# then the variant each of Allegretto, Polka and Beat wears (1 is the default).
PS3_UNLOCKS = [(0x919, "allegretto", 2), (0x91A, "polka", 2), (0x91B, "beat", 2),
               (0x91C, "polka", 3)]
PS3_WORN = [(0x91D, "allegretto"), (0x91E, "polka"), (0x91F, "beat")]
PS3_PARTY_USED = 0x922


class SaveError(Exception):
    pass


def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def split(data, off, layout):
    out = {}
    for name, size in layout:
        out[name] = data[off:off + size]
        off += size
    return out, off


def parse_body(body, ps3):
    """Split a save body (no 360 wrapper) into named blocks."""
    if u32(body, 0) != HEADER_SIZE + THUMB_SIZE:
        raise SaveError("not an Eternal Sonata save (or still encrypted)")
    version = u32(body, 16)
    if version != (3 if ps3 else 2):
        raise SaveError(f"unexpected save version {version}")
    blocks = {"header": body[:HEADER_SIZE],
              "thumb": body[HEADER_SIZE:HEADER_SIZE + THUMB_SIZE]}
    rest, off = split(body, HEADER_SIZE + THUMB_SIZE, PS3_BLOCKS if ps3 else X360_BLOCKS)
    blocks.update(rest)
    blocks["chars"] = body[off:off + CHARS * CHAR_SIZE]
    off += CHARS * CHAR_SIZE
    blocks["b100"] = body[off:off + 100]
    if off + 100 != len(body):
        raise SaveError(f"body is {len(body)} bytes, layout expects {off + 100}")
    return blocks


def ps3_party_to_360(p):
    """The PS3's 2604 byte party block as the 360's 2324, plus what does not fit."""
    out = bytearray(2324)
    out[0x000:0x820] = p[0x000:0x820]
    out[0x820:0x848] = p[0x820:0x848]  # position, characters 1..10
    out[0x848:0x852] = p[0x850:0x85A]  # slotbytes 1..10
    out[0x852:0x8D8] = p[0x85C:0x8E2]  # 3 bytes, Item Set, its scratch copy
    out[0x8D8:0x90D] = p[0x8E4:0x919]  # Item Set count and budget .. score piece Mark
    extra = {
        "position": p[0x848:0x850],
        "slotbytes": p[0x85A:0x85C],
        "unlocks": {(name, variant): p[at] for at, name, variant in PS3_UNLOCKS},
        "worn": {name: p[at] for at, name in PS3_WORN},
        "camp_menu_flags": (p[0x920], p[0x921]),
    }
    if any(p[PS3_PARTY_USED:]):
        print("warning: PS3 party block has data past +0x922 this converter does not "
              "know; it is dropped", file=sys.stderr)
    return bytes(out), extra


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
        print("warning: 360 container check does not match", file=sys.stderr)
    return data[16:end]


def png(width, height, rgba):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))
    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def thumbnail_png(argb):
    rgba = bytearray(len(argb))
    rgba[0::4] = argb[1::4]
    rgba[1::4] = argb[2::4]
    rgba[2::4] = argb[3::4]
    rgba[3::4] = b"\xff" * (len(argb) // 4)
    return png(THUMB_W, THUMB_H, bytes(rgba))


def ps3_to_project(blocks):
    """Returns the 360 body and the reprise.txt entries."""
    party, extra = ps3_party_to_360(blocks["party"])
    split_at = RETAIL_CHARS * STATS_STRIDE
    header = bytearray(blocks["header"])
    struct.pack_into(">I", header, 16, 2)
    body = b"".join([
        bytes(header), blocks["thumb"], b"\0\0\0\0", party, blocks["inventory"],
        blocks["stats_base"][:split_at], blocks["stats_live"][:split_at],
        blocks["map"], blocks["b80"], blocks["chars"], blocks["b100"],
    ])
    assert len(body) == X360_BODY
    record = {
        "version": "1",
        "target": "ps3",
        "party.position": extra["position"].hex().upper(),
        "party.slotbytes": extra["slotbytes"].hex().upper(),
        "party.stats_base": blocks["stats_base"][split_at:].hex().upper(),
        "party.stats_live": blocks["stats_live"][split_at:].hex().upper(),
        "ps3.camp_menu_flags": "".join("1" if f else "0" for f in extra["camp_menu_flags"]),
    }
    for (name, variant), unlocked in extra["unlocks"].items():
        if unlocked:
            record[f"unlocked.{name}.ps3/{variant}"] = "1"
    for name, variant in extra["worn"].items():
        if variant > 1:
            record[f"costume.{name}"] = f"ps3/{variant}"
    return body, record


def load_input(path):
    """(kind, raw bytes) for a PS3 SAVEDATA or a 360 savedata.txt, file or folder."""
    if os.path.isdir(path):
        for name, kind in (("SAVEDATA", "ps3"), ("savedata.txt", "360")):
            if os.path.isfile(os.path.join(path, name)):
                path = os.path.join(path, name)
                break
        else:
            raise SaveError(f"no SAVEDATA or savedata.txt in {path}")
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"SAV\0":
        return "360", data
    if len(data) == PS3_SIZE:
        return "ps3", data
    raise SaveError(f"{path}: unrecognised save ({len(data)} bytes)")


def convert(path):
    kind, data = load_input(path)
    if kind == "ps3":
        body, record = ps3_to_project(parse_body(data, ps3=True))
    else:
        body = unwrap_360(data)
        parse_body(body, ps3=False)
        record = {"version": "1", "target": "360"}
    return kind, body, record


def describe(body):
    party = body[HEADER_SIZE + THUMB_SIZE + 4:]
    return f"party level {u32(party, 4)}"


def save_root(root, profile):
    if profile:
        return os.path.join(root, profile)
    candidates = [d for d in os.listdir(root)
                  if os.path.isdir(os.path.join(root, d, f"{TITLE_ID:08X}"))]
    if len(candidates) != 1:
        raise SaveError(f"{root} holds {len(candidates)} profiles with saves; "
                        "pick one with --profile")
    return os.path.join(root, candidates[0])


def content_header(slot):
    name = f"savecontent{slot:02}"
    header = bytearray(328)
    struct.pack_into(">II", header, 0, 1, 1)
    display = f"Save File {slot + 1:02}".encode("utf-16-be")
    header[8:8 + len(display)] = display
    header[0x108:0x108 + len(name)] = name.encode()
    struct.pack_into(">I", header, 0x140, TITLE_ID)
    return bytes(header)


def write_slot(profile_dir, slot, body, record, force):
    title = os.path.join(profile_dir, f"{TITLE_ID:08X}")
    container = os.path.join(title, "00000001", f"savecontent{slot:02}")
    header = os.path.join(title, "Headers", "00000001", f"savecontent{slot:02}.header")
    if os.path.exists(container) and not force:
        raise SaveError(f"slot {slot} is in use ({container}); pass --force to replace it")
    os.makedirs(container, exist_ok=True)
    os.makedirs(os.path.dirname(header), exist_ok=True)
    with open(os.path.join(container, "savedata.txt"), "wb") as f:
        f.write(wrap_360(body))
    with open(os.path.join(container, "reprise.txt"), "w", newline="\n") as f:
        for key in sorted(record):
            f.write(f"{key} = {record[key]}\n")
    with open(os.path.join(container, "__thumbnail.png"), "wb") as f:
        f.write(thumbnail_png(body[HEADER_SIZE:HEADER_SIZE + THUMB_SIZE]))
    with open(header, "wb") as f:
        f.write(content_header(slot))
    return container


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="PS3 save folder or SAVEDATA, or 360 container or savedata.txt")
    parser.add_argument("--slot", type=int, help="destination slot, 0..9 (Save File 01..10)")
    parser.add_argument("--root", default=os.path.join(os.path.expanduser("~"), "Documents",
                                                        "eternalsonata"),
                        help="user data root (default: %(default)s)")
    parser.add_argument("--profile", help="profile folder under the root, if there are several")
    parser.add_argument("--force", action="store_true", help="replace an existing slot")
    parser.add_argument("--info", action="store_true", help="only describe the input")
    args = parser.parse_args(argv)

    try:
        kind, body, record = convert(args.input)
        print(f"{kind} save: {describe(body)}")
        if args.info:
            for key in sorted(record):
                print(f"  {key} = {record[key]}")
            return 0
        if args.slot is None or not 0 <= args.slot <= 9:
            parser.error("--slot 0..9 is required")
        container = write_slot(save_root(args.root, args.profile), args.slot, body, record,
                               args.force)
        print(f"wrote {container}")
        if kind == "ps3":
            print("load it with PS3 data (--game_version=ps3-...)")
    except (SaveError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
