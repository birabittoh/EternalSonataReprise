#!/usr/bin/env python3
"""The guest image's data as source: src/guest_data/.

Until every byte of the image's data sections comes from there, the game
keeps loading the image built from default.xex (scripts/gen-guest-image.py),
with the text below written over it; this tool is the way to grow the source
and prove it matches.

Text: each BTX blob in .rdata/.data is a UTF-8 file in src/guest_data/text/
named after what it holds. The first line is its guest address; then one
block per language, `[USA]`, holding `id<TAB>text` lines. Japanese is
cp932 in the guest, the other six languages cp1252. `\\n` is a literal
backslash sequence the game interprets, not a line break.

The text is edited here (the console wording is gone), so a blob may differ
from the retail one string by string, but it keeps its languages and ids and
must fit where the retail blob was: the game only points at a blob's start.

Strings: every other NUL terminated ASCII or Japanese string is a line of
src/guest_data/strings.txt, `address<TAB>text`, with `\\\\`, `\\t`, `\\n`,
`\\r` and `\\xNN` escapes, UTF-8 in the file and cp932 in the guest. The
UTF-16 strings in WIDE_RANGES are the same in wide_strings.txt. They are
written in place, so one may not grow.

The PAL-50 refusal (XAPI's sub_82254060) picks its button label and message
by index from a table of UTF-16 strings, each after a 16 bit length; that
table is src/guest_data/pal50_messages.txt, laid out like a text file's
single language block. It may not outgrow the retail table.

Menu layouts are src/guest_data/menu_layouts.txt: each `@address` starts a
stream, then one command per line, the opcode and its operands as signed
decimals; the terminator is implied. A stream may not outgrow its retail
size. sub_821EC050 picks them per screen and language.

    python scripts/guest_data.py check assets/default.xex
    python scripts/guest_data.py extract-text assets/default.xex      # one time
    python scripts/guest_data.py extract-strings assets/default.xex   # one time
    python scripts/guest_data.py extract-pal50 assets/default.xex     # one time
    python scripts/guest_data.py extract-layouts assets/default.xex   # one time
"""
import argparse
import os
import struct
import sys

from xex_image import XexImage

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "guest_data")
TEXT_DIR = os.path.join(ROOT, "text")
STRINGS_FILE = os.path.join(ROOT, "strings.txt")
WIDE_STRINGS_FILE = os.path.join(ROOT, "wide_strings.txt")
PAL50_FILE = os.path.join(ROOT, "pal50_messages.txt")
PAL50_ADDRESS = 0x82000698
LAYOUT_FILE = os.path.join(ROOT, "menu_layouts.txt")

# Menu layouts: sub_821F2F38 runs a stream of commands, an opcode word and
# its operand words, up to STREAM_END. Operand counts come from the
# interpreter's cases; these are the opcodes the image uses.
STREAM_END = 0xFFFF
LAYOUT_OPERANDS = {
    1: 4, 2: 6, 4: 3, 9: 5, 100: 6, 104: 3, 110: 7, 120: 6, 200: 9, 300: 6,
    500: 5, 600: 3, 601: 3, 700: 6, 707: 2, 800: 4, 1000: 5, 1100: 5,
    1200: 3, 1300: 5, 1500: 1, 1501: 0, 1502: 3, 1503: 5, 2100: 1, 2101: 0,
    3000: 1, 3100: 1,
}
# Where the streams lie, and the words among them that are other data.
LAYOUT_RANGES = [(0x8202CAB4, 0x82031A00), (0x82057018, 0x82074A20)]
NOT_LAYOUTS = [(0x8202D764, 0x8202D790), (0x8205CC74, 0x8205CC78),
               (0x8205CDA4, 0x8205CDA8), (0x8205CEFC, 0x8205CF00),
               (0x8205DDE4, 0x8205DDE8), (0x8205E87C, 0x8205E880),
               (0x8205F0EC, 0x8205F108)]

LANGS = ["JPN", "USA", "GBR", "FRA", "ITA", "DEU", "ESP"]
CODEC = {"JPN": "cp932"}
WESTERN_CODEC = "cp1252"

# What each BTX blob in the image holds, by guest address.
TEXT_BLOBS = {
    0x8202A188: "epithets",
    0x8202B8A8: "unlock_key_messages",
    0x82031A00: "names",
    0x820384B8: "shop_sort_orders",
    0x82038978: "battle_level_help",
    0x8203DD60: "chopin_pieces",
    0x82053E10: "score_titles",
    0x822F43F8: "save_locations",
    0x822F94F0: "save_messages",
    0x822FD6F8: "chapters",
    0x822FDD00: "storage_messages",
    0x822FF598: "party_skill_help",
    0x8230F640: "party_skills",
    0x82316300: "battle_commands",
    0x82316500: "enemy_skills",
    0x82332D90: "enemies",
    0x82337590: "shop_lines",
    0x8233A3A8: "item_descriptions",
    0x82376400: "items",
    0x823857D0: "characters",
    0x82385C98: "shops",
    0x823864E8: "photo_shop_lines",
    0x82386910: "shop_checkout_lines",
}

# Data left out of the image, as (start, end, why). Nothing else reads it.
DROPPED = [
    (0x8238EC50, 0x8240AF44, "shader blob: sub_82129260 walks it into the "
     "shader tables, which the native renderer seeds from its own pack"),
    (0x82570000, 0x8257D593, "XDBF: achievement text and icons come from "
     "achievement_text.h and icon.generated.h"),
]

# The image's data sections, as (start, end) guest addresses.
# UTF-16 strings, NUL terminated and 4 aligned, with nothing else between
# them: the sign-in change and save file messages in six languages.
# XContent's file and field names follow the PAL-50 table.
WIDE_RANGES = [(0x82001188, 0x820011A8), (0x820011C4, 0x820011FC),
               (0x820AA058, 0x820AA430), (0x820AA56C, 0x820AA652)]

DATA_RANGES = [(0x82000400, 0x820AB81C), (0x822F0000, 0x82566B3C),
               (0x82566C00, 0x82566C0C), (0x82570000, 0x8257D593)]


def codec(lang):
    return CODEC.get(lang, WESTERN_CODEC)


def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def parse_btx(data, off):
    """{lang: [string bytes]} of the blob at `off`, ids in order."""
    q = off + u32(data, off + 4)
    out = {}
    for _ in range(u32(data, off + 12)):
        lang = data[q:q + 4].decode("ascii").strip()
        table = q + u32(data, q + 4)
        strings = []
        for i in range(u32(data, q + 16)):
            if u32(data, table + 8 * i) != i:
                raise ValueError(f"{lang} string ids are not dense")
            start = q + u32(data, table + 8 * i + 4)
            strings.append(bytes(data[start:data.index(b"\0", start)]))
        out[lang] = strings
        q += u32(data, q + 8)
    return out


def encode_btx(langs):
    """The guest layout: strings packed in id order, the last language's
    link left 0."""
    out = bytearray(b"BTX " + struct.pack(">III", 16, 0, len(langs)))
    for n, (lang, strings) in enumerate(langs.items()):
        table = 20 + 8 * len(strings)
        sub = bytearray(f"{lang:<4}".encode() + struct.pack(">IIII", 20, 0, 0, len(strings)))
        body = bytearray()
        for i, s in enumerate(strings):
            sub += struct.pack(">II", i, table + len(body))
            body += s + b"\0"
        sub += body
        if n < len(langs) - 1:
            struct.pack_into(">I", sub, 8, len(sub))
        out += sub
    struct.pack_into(">I", out, 8, len(out))
    return bytes(out)


def write_text(path, address, langs):
    lines = [f"0x{address:08X}"]
    for lang, strings in langs.items():
        lines.append(f"[{lang}]")
        for i, s in enumerate(strings):
            lines.append(f"{i}\t{s.decode(codec(lang))}")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_text(path):
    """(address, {lang: [string bytes]})."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    address = int(lines[0], 16)
    langs = {}
    current = None
    for n, line in enumerate(lines[1:], 2):
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = langs.setdefault(line[1:-1], [])
            lang = line[1:-1]
            continue
        sid, _, text = line.partition("\t")
        if int(sid) != len(current):
            raise ValueError(f"{path}:{n}: expected id {len(current)}")
        current.append(text.encode(codec(lang)))
    return address, langs


def compile_items():
    """Every (address, bytes) the source defines."""
    items = []
    for name in sorted(os.listdir(TEXT_DIR)):
        if name.endswith(".txt"):
            address, langs = read_text(os.path.join(TEXT_DIR, name))
            items.append((address, encode_btx(langs), name))
    return items


ESCAPES = {"\\": "\\\\", "\t": "\\t", "\n": "\\n", "\r": "\\r"}
UNESCAPES = {"\\": "\\", "t": "\t", "n": "\n", "r": "\r"}

# Strings: (file, codec, terminator width).
NARROW = (STRINGS_FILE, "cp932", 1)
WIDE = (WIDE_STRINGS_FILE, "utf-16-be", 2)

JAPANESE = (("　", "ヿ"), ("一", "鿿"), ("！", "～"))


def escape(raw, codec):
    """Text in the codec when it round trips, else bytes as `\\xNN`."""
    try:
        text = raw.decode(codec)
        if text.encode(codec) != raw:
            raise UnicodeError
    except UnicodeError:
        return "".join(ESCAPES.get(chr(c)) or (chr(c) if 0x20 <= c < 0x7F else f"\\x{c:02x}")
                       for c in raw)
    return "".join(ESCAPES.get(c) or (c if c >= " " and c != "\x7f" else f"\\x{ord(c):02x}")
                   for c in text)


def unescape(text, codec):
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] != "\\":
            out += text[i].encode(codec)
            i += 1
        elif text[i + 1] == "x":
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        else:
            out += UNESCAPES[text[i + 1]].encode(codec)
            i += 2
    return bytes(out)


def read_strings(kind):
    """[(address, bytes without the terminator)]."""
    path, codec, _ = kind
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            if line:
                address, _, text = line.partition("\t")
                out.append((int(address, 16), unescape(text, codec)))
    return out


def write_strings(kind, found):
    path, codec, _ = kind
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(f"0x{address:08X}\t{escape(raw, codec)}\n" for address, raw in found)


def string_size(data, off, width):
    """Bytes before the terminator."""
    end = off
    while any(data[end:end + width]):
        end += width
    return end - off


def find_strings(data, base, covered):
    """Guess the image's narrow strings: 4 aligned, NUL terminated and zero
    padded to the next 4 bytes, at least 4 bytes of printable ASCII or
    Japanese. Shorter ones are indistinguishable from floats and stay with
    the tables."""
    def text(raw):
        try:
            t = raw.decode("cp932")
        except UnicodeDecodeError:
            return False
        return t.encode("cp932") == raw and all(
            " " <= c < "\x7f" or c in "\t\n\r" or any(lo <= c <= hi for lo, hi in JAPANESE)
            for c in t)
    found = []
    for start, end in DATA_RANGES:
        i, end = start - base, end - base
        while i < end:
            if i % 4 == 0 and data[i - 1] == 0 and data[i] and not covered[i]:
                j = data.index(b"\0", i)
                raw = bytes(data[i:j])
                if (j < end and j - i >= 4 and not any(covered[i:j])
                        and not any(data[j:(j + 4) & ~3]) and text(raw)):
                    found.append((base + i, raw))
                    i = j
                    continue
            i += 1
    return found


def find_wide_strings(data, base):
    """The UTF-16 strings in WIDE_RANGES, which hold nothing else."""
    found = []
    for start, end in WIDE_RANGES:
        i = start - base
        while i < end - base:
            size = string_size(data, i, 2)
            found.append((base + i, bytes(data[i:i + size])))
            i += size + 2
            while i < end - base and not any(data[i:i + 2]):
                i += 2
    return found


def overlay_strings(image, base):
    for kind in (NARROW, WIDE):
        width = kind[2]
        for address, raw in read_strings(kind):
            off = address - base
            size = string_size(image, off, width)
            if len(raw) > size:
                raise ValueError(f"string at 0x{address:08X} is {len(raw)} bytes, {size} available")
            image[off:off + size + width] = raw + bytes(size + width - len(raw))


def parse_counted(data, off, count):
    out = []
    for _ in range(count):
        n = struct.unpack_from(">H", data, off)[0] * 2
        out.append(bytes(data[off + 2:off + 2 + n]))
        off += 2 + n
    return out


def encode_counted(strings):
    return b"".join(struct.pack(">H", len(s) // 2) + s for s in strings)


def read_pal50():
    """[string bytes] in id order."""
    with open(PAL50_FILE, encoding="utf-8") as f:
        lines = [line for line in f.read().split("\n") if line]
    if int(lines[0], 16) != PAL50_ADDRESS:
        raise ValueError(f"{PAL50_FILE}: the table lives at 0x{PAL50_ADDRESS:08X}")
    out = []
    for n, line in enumerate(lines[1:], 2):
        sid, _, text = line.partition("\t")
        if int(sid) != len(out):
            raise ValueError(f"{PAL50_FILE}:{n}: expected id {len(out)}")
        out.append(unescape(text, "utf-16-be"))
    return out


def pal50_retail_size(data, base, count):
    return len(encode_counted(parse_counted(data, PAL50_ADDRESS - base, count)))


def overlay_pal50(image, base):
    strings = read_pal50()
    data = encode_counted(strings)
    off = PAL50_ADDRESS - base
    size = pal50_retail_size(image, base, len(strings))
    if len(data) > size:
        raise ValueError(f"PAL-50 messages are {len(data)} bytes, {size} available")
    image[off:off + size] = data + bytes(size - len(data))


def stream_size(data, off):
    """Bytes the retail stream at `off` takes, terminator included."""
    end = off
    while True:
        op = u32(data, end)
        if op == STREAM_END:
            return end + 4 - off
        end += 4 * (1 + LAYOUT_OPERANDS[op])


def read_layouts():
    """[(address, [words without the terminator])]."""
    out = []
    with open(LAYOUT_FILE, encoding="utf-8") as f:
        for n, line in enumerate(f.read().split("\n"), 1):
            line = line.split("#")[0].split()
            if not line:
                continue
            if line[0].startswith("@"):
                out.append((int(line[0][1:], 16), []))
                continue
            words = [int(v) & 0xFFFFFFFF for v in line]
            if LAYOUT_OPERANDS.get(words[0]) != len(words) - 1:
                raise ValueError(f"{LAYOUT_FILE}:{n}: opcode {words[0]} takes "
                                 f"{LAYOUT_OPERANDS.get(words[0])} operands")
            out[-1][1].extend(words)
    return out


def encode_layout(words):
    return struct.pack(f">{len(words) + 1}I", *words, STREAM_END)


def overlay_layouts(image, base):
    for address, words in read_layouts():
        data = encode_layout(words)
        off = address - base
        size = stream_size(image, off)
        if len(data) > size:
            raise ValueError(f"layout at 0x{address:08X} is {len(data)} bytes, {size} available")
        image[off:off + size] = data + bytes(size - len(data))


def retail_size(data, off):
    """Bytes the retail blob at `off` takes, which a replacement must fit in."""
    return len(encode_btx(parse_btx(data, off)))


def overlay_text(image, base):
    """Write the source text over `image`, zeroing what each blob no longer
    uses."""
    for address, data, name in compile_items():
        off = address - base
        size = retail_size(image, off)
        if len(data) > size:
            raise ValueError(f"{name} is {len(data)} bytes, {size} available")
        image[off:off + size] = data + bytes(size - len(data))


def drop_unread(image, base):
    for start, end, _ in DROPPED:
        image[start - base:end - base] = bytes(end - start)


def cmd_extract_text(args):
    xex = XexImage.load(args.xex)
    os.makedirs(TEXT_DIR, exist_ok=True)
    for address, name in TEXT_BLOBS.items():
        off = address - xex.base
        write_text(os.path.join(TEXT_DIR, name + ".txt"), address, parse_btx(xex.data, off))
    print(f"{len(TEXT_BLOBS)} text files in {TEXT_DIR}")
    return 0


def cmd_extract_strings(args):
    xex = XexImage.load(args.xex)
    covered = bytearray(len(xex.data))
    for address, _, _ in compile_items():
        off = address - xex.base
        size = retail_size(xex.data, off)
        covered[off:off + size] = b"\1" * size
    for start, end, _ in DROPPED:
        covered[start - xex.base:end - xex.base] = b"\1" * (end - start)
    wide = find_wide_strings(xex.data, xex.base)
    for address, raw in wide:
        off = address - xex.base
        covered[off:off + len(raw) + 2] = b"\1" * (len(raw) + 2)
    found = find_strings(xex.data, xex.base, covered)
    write_strings(NARROW, found)
    write_strings(WIDE, wide)
    print(f"{len(found)} strings, {len(wide)} wide strings in {ROOT}")
    return 0


def cmd_extract_pal50(args):
    xex = XexImage.load(args.xex)
    strings = parse_counted(xex.data, PAL50_ADDRESS - xex.base, 18)
    with open(PAL50_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"0x{PAL50_ADDRESS:08X}\n")
        f.writelines(f"{i}\t{escape(s, 'utf-16-be')}\n" for i, s in enumerate(strings))
    print(f"{len(strings)} PAL-50 strings in {PAL50_FILE}")
    return 0


def find_layouts(data, base):
    """[(address, [words])] of every stream in LAYOUT_RANGES."""
    found = []
    for start, end in LAYOUT_RANGES:
        a = start
        while a < end:
            skip = next((e for s, e in NOT_LAYOUTS if s == a), None)
            if skip:
                a = skip
                continue
            if not u32(data, a - base):
                a += 4
                continue
            size = stream_size(data, a - base)
            found.append((a, list(struct.unpack_from(f">{size // 4 - 1}I", data, a - base))))
            a += size
    return found


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def cmd_extract_layouts(args):
    xex = XexImage.load(args.xex)
    lines = []
    for address, words in find_layouts(xex.data, xex.base):
        lines.append(f"@0x{address:08X}")
        i = 0
        while i < len(words):
            n = 1 + LAYOUT_OPERANDS[words[i]]
            lines.append(" ".join(str(signed(v)) for v in words[i:i + n]))
            i += n
        lines.append("")
    with open(LAYOUT_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"{lines.count('')} layouts in {LAYOUT_FILE}")
    return 0


def cmd_check(args):
    xex = XexImage.load(args.xex)
    covered = bytearray(len(xex.data))
    failures = edited = 0
    for address, data, name in compile_items():
        off = address - xex.base
        size = retail_size(xex.data, off)
        if len(data) > size:
            print(f"TOO BIG {name}: {len(data)} bytes, {size} available")
            failures += 1
        _, langs = read_text(os.path.join(TEXT_DIR, name))
        retail = parse_btx(xex.data, off)
        if {k: len(v) for k, v in langs.items()} != {k: len(v) for k, v in retail.items()}:
            print(f"MISMATCH {name}: languages or string ids differ")
            failures += 1
        else:
            edited += sum(a != b for k in langs for a, b in zip(langs[k], retail[k]))
        if any(covered[off:off + size]):
            print(f"OVERLAP {name} at 0x{address:08X}")
            failures += 1
        covered[off:off + size] = b"\1" * size
    for kind in (NARROW, WIDE):
        for address, raw in read_strings(kind):
            off = address - xex.base
            size = string_size(xex.data, off, kind[2])
            if len(raw) > size:
                print(f"TOO BIG string at 0x{address:08X}: {len(raw)} bytes, {size} available")
                failures += 1
            edited += raw != xex.data[off:off + size]
            if any(covered[off:off + size]):
                print(f"OVERLAP string at 0x{address:08X}")
                failures += 1
            covered[off:off + size] = b"\1" * size
    pal50 = read_pal50()
    off = PAL50_ADDRESS - xex.base
    size = pal50_retail_size(xex.data, xex.base, len(pal50))
    if len(encode_counted(pal50)) > size:
        print(f"TOO BIG PAL-50 messages: {len(encode_counted(pal50))} bytes, {size} available")
        failures += 1
    edited += sum(a != b for a, b in zip(pal50, parse_counted(xex.data, off, len(pal50))))
    if any(covered[off:off + size]):
        print("OVERLAP PAL-50 messages")
        failures += 1
    covered[off:off + size] = b"\1" * size
    for address, words in read_layouts():
        off = address - xex.base
        size = stream_size(xex.data, off)
        data = encode_layout(words)
        if len(data) > size:
            print(f"TOO BIG layout at 0x{address:08X}: {len(data)} bytes, {size} available")
            failures += 1
        edited += data != xex.data[off:off + size]
        if any(covered[off:off + size]):
            print(f"OVERLAP layout at 0x{address:08X}")
            failures += 1
        covered[off:off + size] = b"\1" * size
    for start, end, _ in DROPPED:
        covered[start - xex.base:end - xex.base] = b"\1" * (end - start)
    total = done = 0
    for start, end in DATA_RANGES:
        for i in range(start - xex.base, end - xex.base):
            if xex.data[i]:
                total += 1
                done += covered[i]
    print(f"{done} of {total} nonzero data bytes come from source or are dropped "
          f"({100 * done / total:.1f}%), {edited} strings edited, {failures} failures")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, fn in (("check", cmd_check), ("extract-text", cmd_extract_text),
                     ("extract-strings", cmd_extract_strings),
                     ("extract-pal50", cmd_extract_pal50),
                     ("extract-layouts", cmd_extract_layouts)):
        p = sub.add_parser(name)
        p.add_argument("xex")
        p.set_defaults(fn=fn)
    args = parser.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
