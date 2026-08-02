#!/usr/bin/env python3
"""Edit a string inside a decoded .e file's `BTX ` text section.

Rebuilds the whole BTX blob from scratch (recomputing every entry offset,
every sub-block "next" link and the blob size), splices it back into the
file, and fixes the two .e header fields that describe where the bulk ends.

Sub-blocks are packed with zero slack, so any length change shifts data. The
transform below is size-exact rather than in-place: it is correct for edits
that grow *or* shrink a string.

    .e header fields touched
      +0x0C  total file size      += delta
      +0x14  reloc_off            += delta   (reloc tables sit after the bulk)
    +0x10 (image size) is untouched -- the image never moves.

usage:
    python scripts/btx_edit.py <decoded.e> --lang ITA --id 1 \
        --replace "così" "quindi" [-o out.e]
    python scripts/btx_edit.py <decoded.e> --lang ITA --id 1 \
        --set "whole new string" -o out.e
"""
import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from btx import CODECS, DEFAULT_CODEC, LANGS, find_btx  # noqa: E402


class BadEdit(Exception):
    pass


def sub_block_at(data, base, want_lang):
    """Locate one language sub-block: (offset, length, next, +0x0C)."""
    first = struct.unpack_from(">I", data, base + 4)[0]
    nlangs = struct.unpack_from(">I", data, base + 0x0C)[0]
    size = struct.unpack_from(">I", data, base + 8)[0]
    q = base + first
    for i in range(nlangs):
        lang = data[q:q + 4].decode("ascii", "replace")
        nxt = struct.unpack_from(">I", data, q + 8)[0]
        length = nxt if i < nlangs - 1 else base + size - q
        if lang == want_lang:
            return q, length, nxt, struct.unpack_from(">I", data, q + 0x0C)[0]
        q += nxt
    raise BadEdit("no %r sub-block" % want_lang)


def build_sub_block(lang, entries, nxt, f12, size):
    """Rebuild one sub-block to exactly `size` bytes.

    Identical strings share a single copy so a longer string can be paid for
    out of the slack that creates; entry offsets are arbitrary, so the reader
    cannot tell. Any leftover space is NUL padding at the end.
    """
    ids = sorted(entries)
    etab_off = 0x14
    data_off = etab_off + 8 * len(ids)
    table, blob, seen = b"", b"", {}
    for sid in ids:
        v = entries[sid]
        if v in seen:
            off = seen[v]
        else:
            off = data_off + len(blob)
            seen[v] = off
            blob += v + b"\0"
        table += struct.pack(">II", sid, off)

    pad = size - (data_off + len(blob))
    if pad < 0:
        raise BadEdit(
            "%s needs %d more bytes than the original sub-block has; no room "
            "even after deduplicating identical strings" % (lang.strip(), -pad))
    out = (struct.pack(">4sIII", lang.encode("ascii"), etab_off, nxt, f12)
           + struct.pack(">I", len(ids)) + table + blob + b"\0" * pad)
    assert len(out) == size
    return out


def sub_block_sizes(data, base):
    """Original byte length of each language sub-block, so a rebuild can be
    padded back to exactly the same footprint."""
    first = struct.unpack_from(">I", data, base + 4)[0]
    nlangs = struct.unpack_from(">I", data, base + 0x0C)[0]
    size = struct.unpack_from(">I", data, base + 8)[0]
    out, q = {}, base + first
    for i in range(nlangs):
        lang = data[q:q + 4].decode("ascii", "replace")
        nxt = struct.unpack_from(">I", data, q + 8)[0]
        # the last block runs to the end of the blob
        out[lang] = nxt if i < nlangs - 1 else base + size - q
        q += nxt
    return out


def blob_meta(data, base):
    """Read the two fields the reader ignores but that we must preserve to
    rebuild a blob byte-exactly: sub-block +0x0C, and whether the chain is
    terminated with next=0. Both shapes ship in the retail files."""
    first = struct.unpack_from(">I", data, base + 4)[0]
    nlangs = struct.unpack_from(">I", data, base + 0x0C)[0]
    q = base + first
    f12 = struct.unpack_from(">I", data, q + 0x0C)[0]
    for _ in range(nlangs - 1):
        q += struct.unpack_from(">I", data, q + 8)[0]
    return f12, struct.unpack_from(">I", data, q + 8)[0] == 0


def fix_relocations(out, base, old_btx_size, delta):
    """Adjust list B raw dwords that point to the post-BTX string pool.

    At load time, the .e loader does `*(imagedata+off) += bulk_base` for every
    list B entry.  Each raw dword encodes a bulk offset.  When the BTX blob
    grows (or shrinks), any raw dword that points *after* the old BTX end must
    be adjusted by delta so it still lands on the (now shifted) string pool or
    other bulk data.

    Without this fix the relocated pointer targets the correct *absolute*
    address but the data at that address is wrong (shifted by delta).  The
    script VM then reads garbage and a wild-handler pointer like 0x52054163
    appears.
    """
    magic = struct.unpack_from(">I", out, 0)[0]
    if magic not in (0x180, 0x181):
        return  # not a .e file

    imgsz = struct.unpack_from(">I", out, 0x10)[0]
    image_end = 0x18 + imgsz
    relofs = struct.unpack_from(">I", out, 0x14)[0]
    # reloc tables have shifted by delta (BTX blob growth/shrink), but the
    # header field relofs is not yet updated — caller does it after us.
    reloc_base = image_end + relofs + delta

    (nA,) = struct.unpack_from(">I", out, reloc_base)
    off = reloc_base + 4 + 4 * nA
    (nB,) = struct.unpack_from(">I", out, off)
    b_entries = list(struct.unpack_from(">%dI" % nB, out, off + 4))

    # The image allocation starts at file offset 0 (memcpy includes the header),
    # so each table entry IS a file offset.  The raw dword at that offset
    # encodes a bulk offset.  Adjust it when it points past the BTX blob.
    btx_end_bulk = base + old_btx_size - image_end  # bulk-offset threshold

    patched = 0
    for entry in b_entries:
        raw = struct.unpack_from(">I", out, entry)[0]
        if raw >= btx_end_bulk:
            struct.pack_into(">I", out, entry, raw + delta)
            patched += 1

    if patched:
        print("  adjusted %d list B relocation%s by %+d (post-BTX data shift)"
              % (patched, "s" if patched != 1 else "", delta))


def build_btx(langs, f12=0, terminate=False, sizes=None):
    """Serialise {lang: {id: bytes}} back into a BTX blob.

    Layout matches what the game ships: header, then sub-blocks in LANGS
    order, each holding its entry table followed by its string data.

    `sizes` maps lang -> required sub-block length. When given, each block is
    padded with NULs to exactly that length, so the rebuilt blob occupies the
    same bytes as the original and nothing downstream in the .e moves. To make
    room for a longer string, identical values are deduplicated and share one
    copy -- entry offsets are arbitrary, so this is invisible to the reader.
    """
    header = struct.pack(">4sIII", b"BTX ", 0x10, 0, len(langs))
    blocks = []
    for lang in LANGS:
        if lang not in langs:
            continue
        entries = langs[lang]
        ids = sorted(entries)
        etab_off = 0x14
        data_off = etab_off + 8 * len(ids)

        table, data = b"", b""
        seen = {}
        for sid in ids:
            v = entries[sid]
            if sizes is not None and v in seen:
                off = seen[v]              # share an identical earlier string
            else:
                off = data_off + len(data)
                seen[v] = off
                data += v + b"\0"
            table += struct.pack(">II", sid, off)

        if sizes is not None:
            want = sizes[lang] - (data_off + len(data))
            if want < 0:
                raise BadEdit(
                    "%s needs %d more bytes than the original sub-block has; "
                    "no room even after deduplication" % (lang.strip(), -want))
            data += b"\0" * want

        # +0x08 (next) is patched once every block's length is known; +0x0C is
        # a constant the reader never touches, carried over by the caller.
        blocks.append(bytearray(
            struct.pack(">4sIII", lang.encode("ascii"), etab_off, 0, f12)
            + struct.pack(">I", len(ids)) + table + data))

    for i, blk in enumerate(blocks):
        last = i == len(blocks) - 1
        struct.pack_into(">I", blk, 8, 0 if (last and terminate) else len(blk))
    blob = header + b"".join(bytes(b) for b in blocks)
    blob = bytearray(blob)
    struct.pack_into(">I", blob, 8, len(blob))
    return bytes(blob)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--lang", required=True, help="e.g. ITA")
    ap.add_argument("--id", type=int, required=True, help="string id")
    ap.add_argument("--blob", type=int, default=0,
                    help="which BTX blob, if the file has several (default 0)")
    ap.add_argument("--replace", nargs=2, metavar=("OLD", "NEW"),
                    help="substring replacement within the string")
    ap.add_argument("--set", dest="whole", help="replace the entire string")
    ap.add_argument("-o", "--out", help="output file (default: in place)")
    ap.add_argument("--preserve-size", action="store_true",
                    help="keep every sub-block at its original length so no "
                         "byte in the file moves (dedups identical strings to "
                         "make room). Use this if a grown file crashes.")
    args = ap.parse_args(argv)
    if not args.replace and not args.whole:
        ap.error("need --replace or --set")

    lang = args.lang.upper().ljust(4)
    if lang not in LANGS:
        ap.error("unknown language %r (want one of %s)"
                 % (lang, " ".join(x.strip() for x in LANGS)))
    codec = CODECS.get(lang, DEFAULT_CODEC)

    data = bytearray(open(args.file, "rb").read())
    blobs = find_btx(bytes(data))
    if not blobs:
        sys.exit("%s: no BTX section" % args.file)
    base, langs = blobs[args.blob]
    f12, terminate = blob_meta(data, base)

    entries = langs.get(lang)
    if entries is None or args.id not in entries:
        sys.exit("no string id %d in %r" % (args.id, lang))

    old = entries[args.id]
    if args.whole is not None:
        new = args.whole.encode(codec)
    else:
        find, repl = (s.encode(codec) for s in args.replace)
        if find not in old:
            sys.exit("%r not found in %s id %d:\n  %s"
                     % (args.replace[0], lang, args.id,
                        old.decode(codec, "replace")))
        new = old.replace(find, repl)
    entries[args.id] = new

    old_size = struct.unpack_from(">I", data, base + 8)[0]
    try:
        if args.preserve_size:
            # Touch only the edited sub-block; every other byte of the file,
            # including the other languages, stays exactly where it was.
            q, length, nxt, sf12 = sub_block_at(data, base, lang)
            out = bytearray(data[:q] + build_sub_block(lang, entries, nxt, sf12, length)
                            + data[q + length:])
            delta = 0
        else:
            blob = build_btx(langs, f12, terminate)
            delta = len(blob) - old_size
            out = bytearray(data[:base] + blob + data[base + old_size:])
            if delta:
                fix_relocations(out, base, old_size, delta)
    except BadEdit as e:
        sys.exit("error: %s" % e)

    total, = struct.unpack_from(">I", out, 0x0C)
    relofs, = struct.unpack_from(">I", out, 0x14)
    struct.pack_into(">I", out, 0x0C, total + delta)
    struct.pack_into(">I", out, 0x14, relofs + delta)

    dest = args.out or args.file
    open(dest, "wb").write(out)
    print("%s\n  %s id %d: %s\n             -> %s\n  blob 0x%x -> 0x%x "
          "(%+d bytes), file %d -> %d%s"
          % (dest, lang.strip(), args.id, old.decode(codec, "replace"),
             new.decode(codec, "replace"), old_size, old_size + delta, delta,
             len(data), len(out),
             "  [size-preserving: nothing outside the %s block moved]"
             % lang.strip() if args.preserve_size else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
