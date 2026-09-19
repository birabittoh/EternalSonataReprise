#!/usr/bin/env python3
r"""Scaffold a text-replacement mod seeded with every string of one language.

The result is a mod folder as docs/making-mods.md describes it: mod.toml,
assets.toml and one assets/text/<LANG>.csv holding every string of the chosen
language from every .e container and from the 23 BTX blobs baked into
default.xex. Every row is the shipped text, so the mod changes nothing until a
row is edited; --prune deletes the rows left alone before shipping.

    # patch the shipped Italian in place, with the English above each row
    python scripts/new_text_mod.py --patch ITA --reference USA --id ita_fix

    # add Portuguese as a new language, starting from the Italian text and
    # living in the Spanish BTX slot (see "Adding a new language" in the docs)
    python scripts/new_text_mod.py --new PT --label Portugues --slot ESP \
        --from ITA --id portuguese

    # before shipping: drop every row still equal to the shipped text
    python scripts/new_text_mod.py --prune mods/ita_fix

The mod is written to mods/<id>/ so the game picks it up on the next launch.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from btx import DEFAULT_CODEC, LANGS, find_btx  # noqa: E402
from unpack_e import load_toc, unpack  # noqa: E402
from xex_image import XexImage  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(REPO, "assets")
XEX = os.path.join(ASSETS, "default.xex")
UNPACK_EXE = os.path.join(REPO, "scripts", "unpack_e.exe")

# XLanguage ids the game itself uses; a new language must pick another one.
BUILTIN_XLANGUAGE = {"USA": 1, "GBR": 1, "DEU": 3, "FRA": 4, "ESP": 5, "ITA": 6}

# Second line of every table this script writes, so --prune knows which
# shipped language to compare against.
SOURCE_TAG = b"# source: "


def decoded_containers():
    """Yield (guest_path, decoded bytes) for every .e in index.vmtoc."""
    toc = load_toc(ASSETS)
    names = sorted(n for n in toc if n.endswith(".e"))
    if os.path.exists(UNPACK_EXE):
        tmp = tempfile.mkdtemp(prefix="es_text_")
        try:
            subprocess.run([UNPACK_EXE, ASSETS, tmp], check=True, stdout=subprocess.DEVNULL)
            for rel in names:
                with open(os.path.join(tmp, rel), "rb") as f:
                    yield rel, f.read()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        return
    # The Python decoder is a reference implementation and takes minutes over
    # the whole game; the C one is the normal path.
    print("unpack_e.exe not found, decoding with the slow Python reference", file=sys.stderr)
    for rel in names:
        size, flag = toc[rel]
        with open(os.path.join(ASSETS, rel), "rb") as f:
            yield rel, unpack(f.read(), size, flag)


def strings_of(data, lang):
    """[(blob_index, id, bytes)] for one language, blobs in scan order.

    The blob index is the position among the valid BTX blobs found by scanning
    for the magic, which is exactly how the host numbers them."""
    out = []
    for blob_index, (_, langs) in enumerate(find_btx(data)):
        entries = langs.get(lang)
        if not entries:
            continue
        for sid in sorted(entries):
            out.append((blob_index, sid, entries[sid]))
    return out


def collect_all(src, ref_lang=None, with_xex=True):
    """Every string of `src` as [(file, blob, id, bytes)], plus a dict of the
    same keys to the `ref_lang` string when one is asked for."""
    rows, ref, files = [], {}, 0

    def collect(rel, data):
        found = strings_of(data, src)
        rows.extend((rel, blob, sid, raw) for blob, sid, raw in found)
        if ref_lang:
            ref.update(((rel, blob, sid), raw) for blob, sid, raw in strings_of(data, ref_lang))
        return len(found)

    for rel, data in decoded_containers():
        if collect(rel, data):
            files += 1
    print("%d strings from %d .e containers" % (len(rows), files))
    if with_xex:
        print("%d strings from default.xex" % collect("default.xex", XexImage.load(XEX).data))
    return rows, ref


def to_utf8(raw, lang, where, strict=True):
    if lang == "JPN ":
        # The host passes JPN through untouched, so the CSV carries Shift-JIS
        # bytes rather than UTF-8. Kept as bytes all the way to the file.
        return raw
    try:
        return raw.decode(DEFAULT_CODEC, "strict" if strict else "replace").encode("utf-8")
    except UnicodeDecodeError as e:
        bad = raw[e.start]
        raise SystemExit("%s: byte 0x%02X has no cp1252 meaning; the host cannot "
                         "round-trip it either" % (where, bad))


def to_game(text, lang):
    """The host's transcoding of a CSV cell, to compare against the shipped bytes."""
    if lang == "JPN ":
        return text
    return (text.decode("utf-8").replace("\r", "").replace("\n", "\\n")
            .encode(DEFAULT_CODEC, "replace"))


def csv_field(text):
    return b'"' + text.replace(b'"', b'""') + b'"'


def parse_csv_row(line):
    """The host's own CSV rules: quotes toggle anywhere, "" is a quote."""
    fields, field, quoted, i = [], bytearray(), False, 0
    while i < len(line):
        c = line[i:i + 1]
        if quoted:
            if c == b'"' and line[i + 1:i + 2] == b'"':
                field += b'"'
                i += 1
            elif c == b'"':
                quoted = False
            else:
                field += c
        elif c == b'"':
            quoted = True
        elif c == b",":
            fields.append(bytes(field))
            field = bytearray()
        elif c != b"\r":
            field += c
        i += 1
    fields.append(bytes(field))
    return fields


def write_csv(path, rows, lang, src_lang, ref_lang=None, ref=None):
    """rows: [(file, blob, id, bytes)] in output order; ref maps (file, blob,
    id) to the same string in ref_lang, written as a comment above each row."""
    with open(path, "wb") as f:
        # The host only recognises the header on the first line.
        f.write(b"file,blob,id,text\n")
        f.write(SOURCE_TAG + src_lang.strip().encode() + b"\n")
        f.write(("# Every %s string in the game, one per row. Edit the text column; "
                 "--prune deletes the rows left unchanged.\n" % src_lang.strip()).encode())
        if lang == "JPN ":
            f.write(b"# JPN is Shift-JIS and is written here as raw Shift-JIS bytes, not UTF-8.\n")
        else:
            f.write(b"# UTF-8, but only characters with a cp1252 equivalent exist in the "
                    b"game's font. A newline is the two characters \\n.\n")
        current = None
        for file, blob, sid, raw in rows:
            if file != current:
                current = file
                f.write(b"\n# " + file.encode() + b"\n")
            if ref is not None and (file, blob, sid) in ref:
                f.write(b"# %s: %s\n" % (ref_lang.strip().encode(),
                                         to_utf8(ref[(file, blob, sid)], ref_lang,
                                                 "%s#%d" % (file, sid), strict=False)))
            f.write(b"%s,%d,%d,%s\n" % (file.encode(), blob, sid,
                                         csv_field(to_utf8(raw, src_lang, "%s#%d" % (file, sid)))))


def prune_table(path):
    """Rewrite one table without the rows that still match the shipped text.
    A reference comment stays with its row; per-file headings are regenerated."""
    lines = open(path, "rb").read().split(b"\n")
    src = None
    for l in lines:
        if l.startswith(SOURCE_TAG):
            src = l[len(SOURCE_TAG):].strip().decode("ascii").ljust(4)
    if src not in LANGS:
        raise SystemExit("%s: no '%s' line; was it written by this script?"
                         % (path, SOURCE_TAG.decode()))
    rows, _ = collect_all(src)
    shipped = {(f, b, i): raw for f, b, i, raw in rows}
    files = {f for f, _, _ in shipped}

    head, body, pending, current, dropped, kept = [], [], [], None, 0, 0
    in_head = True  # the explanatory comments above the first per-file heading
    for l in lines:
        if not l.strip():
            continue
        if l.startswith(b"#"):
            if l[1:].strip().decode("latin1") in files:
                in_head = False
            elif in_head:
                if not l.startswith(SOURCE_TAG):
                    head.append(l)
            else:
                pending.append(l)
            continue
        fields = parse_csv_row(l)
        if len(fields) < 4 or fields[0] == b"file":
            continue
        file = fields[0].decode("latin1").lower().replace("\\", "/")
        key = (file, int(fields[1] or 0), int(fields[2]))
        if key in shipped and to_game(fields[3], src) == shipped[key]:
            dropped += 1
            pending = []
            continue
        kept += 1
        if file != current:
            current = file
            body += [b"", b"# " + file.encode()]
        body += pending
        pending = []
        body.append(l)

    with open(path, "wb") as f:
        f.write(b"file,blob,id,text\n" + SOURCE_TAG + src.strip().encode() + b"\n")
        f.write(b"\n".join(head + body) + b"\n")
    print("%s: dropped %d unchanged rows, kept %d" % (os.path.basename(path), dropped, kept))


def prune(mod_dir):
    text_dir = os.path.join(mod_dir, "assets", "text")
    tables = sorted(n for n in os.listdir(text_dir) if n.lower().endswith(".csv")) \
        if os.path.isdir(text_dir) else []
    if not tables:
        raise SystemExit("no assets/text/*.csv under %s" % mod_dir)
    for name in tables:
        prune_table(os.path.join(text_dir, name))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--patch", metavar="LANG",
                      help="replace this shipped language in place (JPN USA GBR FRA ITA DEU ESP)")
    mode.add_argument("--new", metavar="CODE",
                      help="add a new language with this two-letter code (folder name)")
    mode.add_argument("--prune", metavar="MOD_DIR",
                      help="delete every row of an existing mod's tables that still matches "
                           "the shipped text")
    ap.add_argument("--from", dest="source", metavar="LANG",
                    help="language whose text seeds the table (default: the patched one, or USA)")
    ap.add_argument("--label", help="new language: name shown in the language menus")
    ap.add_argument("--slot", default="ESP", help="new language: donor BTX block (default ESP)")
    ap.add_argument("--xlanguage", type=int, default=9,
                    help="new language: XLanguage id, must not be 1 3 4 5 6 (default 9)")
    ap.add_argument("--id", help="mod folder name (default: derived from the language)")
    ap.add_argument("--name", help="mod display name")
    ap.add_argument("--author", default="")
    ap.add_argument("-o", "--out", help="parent directory for the mod (default: mods/)")
    ap.add_argument("--reference", metavar="LANG",
                    help="write this language's string as a comment above each row, e.g. USA "
                         "to keep the English in view while fixing another language")
    ap.add_argument("--no-xex", action="store_true",
                    help="skip the menu chrome baked into default.xex")
    args = ap.parse_args(argv)

    if args.prune:
        prune(args.prune)
        return 0

    if args.patch:
        lang = args.patch.upper().ljust(4)
        if lang not in LANGS:
            ap.error("unknown language %r; one of %s" % (args.patch, " ".join(l.strip() for l in LANGS)))
        src = args.source.upper().ljust(4) if args.source else lang
        folder = lang.strip()
        mod_id = args.id or "%s_text" % folder.lower()
        name = args.name or "%s text rewrite" % folder
    else:
        folder = args.new.upper()
        if not (1 <= len(folder) <= 2) or not folder.isalpha():
            ap.error("--new takes a one or two letter code")
        if not args.label:
            ap.error("--new needs --label")
        slot = args.slot.upper()
        if slot.ljust(4) not in LANGS:
            ap.error("unknown --slot %r" % args.slot)
        if args.xlanguage in set(BUILTIN_XLANGUAGE.values()):
            ap.error("--xlanguage %d is one the game uses" % args.xlanguage)
        src = (args.source or "USA").upper().ljust(4)
        mod_id = args.id or "%s_language" % args.label.lower().replace(" ", "_")
        name = args.name or args.label
        lang = None
    if src not in LANGS:
        ap.error("unknown --from language %r" % args.source)
    ref_lang = args.reference.upper().ljust(4) if args.reference else None
    if ref_lang and ref_lang not in LANGS:
        ap.error("unknown --reference language %r" % args.reference)

    out_root = args.out or os.path.join(REPO, "mods")
    mod_dir = os.path.join(out_root, mod_id)
    if os.path.exists(mod_dir):
        raise SystemExit("%s already exists; pick another --id or remove it" % mod_dir)

    rows, ref = collect_all(src, ref_lang, not args.no_xex)

    os.makedirs(os.path.join(mod_dir, "assets", "text"))
    write_csv(os.path.join(mod_dir, "assets", "text", folder + ".csv"), rows,
              lang or src, src, ref_lang, ref if ref_lang else None)

    with open(os.path.join(mod_dir, "mod.toml"), "w", encoding="utf-8") as f:
        f.write('name = "%s"\nversion = "0.1.0"\nauthor = "%s"\n' % (name, args.author))
        if args.patch:
            f.write('description = "Replaces the game\'s %s text."\n' % folder)
        else:
            f.write('description = "Adds %s as a selectable language."\n' % args.label)

    with open(os.path.join(mod_dir, "assets.toml"), "w", encoding="utf-8") as f:
        f.write("# Edited strings may outgrow the original; this lets the container be\n"
                "# rebuilt for them. It does not apply to default.xex, whose language block\n"
                "# must stay within its original total size.\n"
                "[defaults]\nallow_resize = true\n")
        if not args.patch:
            f.write("\n[[language]]\nid = %d\nlabel = \"%s\"\ncode = \"%s\"\nslot = \"%s\"\n"
                    % (args.xlanguage, args.label, folder, slot))

    print("wrote %s" % mod_dir)
    print("edit assets/text/%s.csv, then enable the mod in the F1 mod manager" % folder)
    return 0


if __name__ == "__main__":
    sys.exit(main())
