#!/usr/bin/env python3
"""The guest image's XEX header as source: src/guest_data/xex_header.txt.

Only what the SDK reads when it loads the image is kept: execution info,
entry point, base, stack, TLS, system flags, the page layout and the import
libraries. Digests, signatures, the key and the other optional headers go.

One `name value...` per line, `#` starts a comment. `pages` is followed by
`kind count` rows (XEX section kinds, in 64 KiB pages). `library name id
version min_version` starts an import library: its rows are `record_address
word`, in table order, with 0 for an empty slot; the word is the one the
SDK reads at that address in the image (type and ordinal of the import).

    python scripts/xex_header.py extract assets/default.xex   # one time
"""
import os
import struct
import sys

HEADER_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                           "src", "guest_data", "xex_header.txt")

KEY_RESOURCE = 0x000002FF
KEY_FORMAT = 0x000003FF
KEY_ENTRY = 0x00010100
KEY_BASE = 0x00010201
KEY_IMPORTS = 0x000103FF
KEY_STACK = 0x00020200
KEY_TLS = 0x00020104
KEY_SYSTEM_FLAGS = 0x00030000
KEY_EXECUTION = 0x00040006

SECURITY_SIZE = 0x184
PAGE_SIZE = 0x10000
ALIGN = 0x1000


def parse_int(token):
    return int(token, 0)


def read_header(path=HEADER_FILE):
    spec = {"pages": [], "libraries": []}
    section = None
    with open(path, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            words = line.split("#")[0].split()
            if not words:
                continue
            if words[0] == "pages":
                section = spec["pages"]
            elif words[0] == "library":
                section = []
                spec["libraries"].append((words[1], [parse_int(w) for w in words[2:]], section))
            elif (words[0].startswith("0x") or words[0] == "0") and section is not None and section is not spec["pages"]:
                section.append(tuple(parse_int(w) for w in words))
            elif words[0].isdigit() and section is spec["pages"]:
                section.append(tuple(parse_int(w) for w in words))
            else:
                spec[words[0]] = [parse_int(w) for w in words[1:]]
    return spec


def import_words(spec):
    """{record address: word} the image must hold outside tables.txt."""
    return {r[0]: r[1] for _, _, rows in spec["libraries"] for r in rows if len(r) > 1}


def imports_block(spec):
    names = b"".join(n.encode() + b"\0" + bytes(-(len(n) + 1) % 4) for n, _, _ in spec["libraries"])
    libs = b""
    for i, (_, (lib_id, version, minimum), rows) in enumerate(spec["libraries"]):
        libs += struct.pack(">I20xIIIHH", 0x28 + 4 * len(rows), lib_id, version, minimum,
                            i, len(rows))
        libs += struct.pack(f">{len(rows)}I", *(r[0] for r in rows))
    total = 12 + len(names) + len(libs)
    return struct.pack(">III", total, len(names), len(spec["libraries"])) + names + libs


def build_header(spec, runs):
    """The header for an image laid out as (data_size, zero_size) `runs`."""
    pages = spec["pages"]
    image_size = PAGE_SIZE * sum(c for _, c in pages)
    sec = bytearray(SECURITY_SIZE + 24 * len(pages))
    struct.pack_into(">II", sec, 0, len(sec), image_size)
    struct.pack_into(">III", sec, 0x10C, spec["image_flags"][0], spec["load_address"][0], 0)
    struct.pack_into(">III", sec, 0x178, spec["region"][0], spec["media_types"][0], len(pages))
    for i, (kind, count) in enumerate(pages):
        struct.pack_into(">I", sec, SECURITY_SIZE + 24 * i, count << 4 | kind)
    struct.pack_into(">I", sec, 0x128, len(spec["libraries"]))

    fmt = struct.pack(">IHH", 8 + 8 * len(runs), 0, 1)
    fmt += b"".join(struct.pack(">II", d, z) for d, z in runs)
    blocks = {
        KEY_FORMAT: fmt,
        KEY_IMPORTS: imports_block(spec),
        KEY_TLS: struct.pack(">4I", *spec["tls"]),
        KEY_EXECUTION: struct.pack(">6I", *spec["execution_info"]),
    }
    inline = {KEY_ENTRY: spec["entry"][0], KEY_BASE: spec["load_address"][0],
              KEY_STACK: spec["stack_size"][0], KEY_SYSTEM_FLAGS: spec["system_flags"][0]}
    keys = sorted(list(blocks) + list(inline))
    pos = 24 + 8 * len(keys)
    table = b""
    body = b""
    for key in keys:
        if key in inline:
            table += struct.pack(">II", key, inline[key])
            continue
        data = blocks[key]
        table += struct.pack(">II", key, pos + len(body))
        body += data + bytes(-len(data) % 4)
    security_offset = pos + len(body)
    header = struct.pack(">4sIIIII", b"XEX2", spec["module_flags"][0], 0, 0,
                         security_offset, len(keys)) + table + body + bytes(sec)
    header += bytes(-len(header) % ALIGN)
    return struct.pack(">4sIIII", b"XEX2", spec["module_flags"][0], len(header), 0,
                       security_offset) + header[20:]


def extract(path):
    raw = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", raw, o)[0]
    sec = u32(16)
    opt = {u32(24 + 8 * i): u32(28 + 8 * i) for i in range(u32(20))}
    out = ["# The guest image's XEX header. See scripts/xex_header.py.", "",
           f"module_flags 0x{u32(4):X}", f"image_flags 0x{u32(sec + 0x10C):X}",
           f"load_address 0x{u32(sec + 0x110):08X}", f"region 0x{u32(sec + 0x178):X}",
           f"media_types 0x{u32(sec + 0x17C):X}", f"entry 0x{opt[KEY_ENTRY]:08X}",
           f"stack_size 0x{opt[KEY_STACK]:X}", f"system_flags 0x{opt[KEY_SYSTEM_FLAGS]:X}",
           "execution_info " + " ".join(f"0x{u32(opt[KEY_EXECUTION] + 4 * i):X}" for i in range(6)),
           "tls " + " ".join(f"0x{u32(opt[KEY_TLS] + 4 * i):X}" for i in range(4)),
           "", "pages"]
    runs = []
    for i in range(u32(sec + 0x180)):
        v = u32(sec + 0x184 + 24 * i)
        kind, count = v & 15, v >> 4
        if runs and runs[-1][0] == kind:
            runs[-1][1] += count
        else:
            runs.append([kind, count])
    out += [f"{k} {c}" for k, c in runs]
    base = opt[KEY_IMPORTS]
    total, names_size = u32(base), u32(base + 4)
    names = raw[base + 12:base + 12 + names_size].split(b"\0")
    lib = names_size + 12
    while lib < total and u32(base + lib):
        size = u32(base + lib)
        name = names[struct.unpack_from(">H", raw, base + lib + 0x24)[0] & 0xFF].decode()
        count, = struct.unpack_from(">H", raw, base + lib + 0x26)
        out += ["", f"library {name} 0x{u32(base + lib + 0x18):X} 0x{u32(base + lib + 0x1C):X} "
                    f"0x{u32(base + lib + 0x20):X}"]
        out += [f"0x{u32(base + lib + 0x28 + 4 * i):08X}" for i in range(count)]
        lib += size
    return out


def main():
    if len(sys.argv) != 3 or sys.argv[1] != "extract":
        print(__doc__, file=sys.stderr)
        return 2
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from xex_image import XexImage
    from guest_data import DATA_RANGES
    image = XexImage.load(sys.argv[2])
    lines = extract(sys.argv[2])
    final = []
    for line in lines:
        if line.startswith("0x") and " " not in line:
            addr = int(line, 16)
            word = image.u32(addr) if addr else 0
            if not addr:
                final.append("0")
            elif any(a <= addr < b for a, b in DATA_RANGES):
                final.append(f"0x{addr:08X}")
            else:
                final.append(f"0x{addr:08X} 0x{word:08X}")
        else:
            final.append(line)
    with open(HEADER_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(final) + "\n")
    print(f"wrote {HEADER_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
