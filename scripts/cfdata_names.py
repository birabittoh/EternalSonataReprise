#!/usr/bin/env python3
"""Build the cfdata area-id -> display-name table.

The display name of a field area lives in its `cfdata` area file (e.g.
`cfdata/tnk01.e`) as BTX string id 0 of the map-info section (usually the
*last* BTX section). Earlier code wrongly
took the first section's string 0, which for multi-section files is the shared
"Handed over %s" blob (empty string 0) or a dialogue/UI prompt.

usage:
    python scripts/cfdata_names.py                 # print id<TAB>name
    python scripts/cfdata_names.py --out docs/cfdata_names.txt
    python scripts/cfdata_names.py --cfdata assets-ps3/cfdata         --exclude docs/cfdata_names.txt --out docs/cfdata_names_ps3.txt
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import btx  # noqa: E402

CFDATA_GLOB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "extracted", "e", "cfdata", "*.e")
LANG = "USA "

# Hand-verified overrides where the map-info section is not the last BTX
# section (a dialogue/UI section follows it). Commented with the true name.
OVERRIDES = {
    "bqm46": "Baroque City: Warp Room",  # last section is the teleport UI
    "bqj63": None,  # PS3: string 0 is the "Warp Room Key" prompt
}

# Every id with no name in the table is an event/scene file, not a field
# area: its bulk section references other .e area files and/or MP*.cxs
# scripts (e.g. rvb40 -> fmt14/zzz02/rvb01/hno01 + MP122.cxs, zzz02 -> the
# master event dispatcher over 22 areas). None has a missing display name.

# The "e<digits>" family (e.g. e1170_020, e3120_120) is the event/scene id
# space; the runtime (room_presence.cpp) already treats any id matching this
# shape as "Main Menu" without consulting this table. For some of these ids
# the "last BTX section" heuristic above lands on a sound-test/jukebox track
# list instead of a map-info section, picking up OST track titles (e.g.
# "Prelude in D flat major Op. 28 No. 15") as if they were area names. Blank
# them here so the source-of-truth file doesn't carry misleading names for
# entries nothing ever displays.
EVENT_ID_RE = re.compile(r"^e\d")

# Strings that are not area names even though they sit at the map-info
# section's string 0. Kept so the table never lies about them.
KNOWN_NON_NAMES = {
    "tnt01": "Game Clear Data",  # special save/clear screen map
    "zzz01": "Go where?",        # world-map select area (36 destinations)
}


def string0(ent):
    raw = ent.get(0)
    if raw is None:
        return None
    s = btx.decode(raw, LANG)
    return re.sub(r"<[^>]*>", "", s).strip() or None


def is_garbled(s):
    # fmt06's map-info string0 is a run of unmappable [?] chars (blank name).
    return s.count("[?]") >= len(s.replace("[?]", "")) or s.count("\ufffd") > 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="write id<TAB>name lines to this file")
    ap.add_argument("--cfdata", help="decoded cfdata directory (default: extracted/e/cfdata)")
    ap.add_argument("--exclude", help="skip ids already listed in this id<TAB>name file")
    args = ap.parse_args(argv)

    pattern = os.path.join(args.cfdata, "*.e") if args.cfdata else CFDATA_GLOB
    known = set()
    if args.exclude:
        with open(args.exclude, encoding="utf-8") as f:
            known = {line.split("	", 1)[0] for line in f if "	" in line}

    rows = []
    for path in sorted(glob.glob(pattern), key=lambda p: os.path.basename(p).lower()):
        aid = os.path.basename(path)[:-2].lower()
        if aid in known:
            continue
        data = open(path, "rb").read()
        blobs = btx.find_btx(data)
        if not blobs:
            rows.append((aid, None))
            continue
        name = None
        for off, langs in reversed(blobs):
            name = string0(langs.get(LANG, {}))
            if name:
                break
        if name and is_garbled(name):
            name = None
        if name and EVENT_ID_RE.match(aid.lower()):
            name = None
        if aid in OVERRIDES:
            name = OVERRIDES[aid]
        elif aid in KNOWN_NON_NAMES:
            name = KNOWN_NON_NAMES[aid]
        rows.append((aid, name))

    lines = ["%s\t%s" % (aid, name or "") for aid, name in rows]
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print("wrote %d entries to %s" % (len(rows), args.out))
    else:
        sys.stdout.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
