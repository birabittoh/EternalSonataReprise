#!/usr/bin/env python3
"""Generate src/area_names.generated.h from docs/cfdata_names.txt and
docs/cfdata_names_ps3.txt (the PS3's areas the 360 lacks).

Embeds the id -> display-name table (the map-info BTX string 0 of each
area's cfdata file). Includes all areas from the source file, with empty
names for event/support files (eXXXX, *60, zzz02, ...) that don't have
display names.

Emitted header is a gitignored build artifact; the .txt is the source of
truth (regenerate with scripts/cfdata_names.py).
"""

import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC = os.path.join(ROOT, "docs", "cfdata_names.txt")
SRC_PS3 = os.path.join(ROOT, "docs", "cfdata_names_ps3.txt")
DST = os.path.join(ROOT, "src", "area_names.generated.h")


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def read_entries(path):
    entries = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if "\t" not in line:
                continue
            aid, name = line.split("\t", 1)
            # Include all entries, even those with empty names (event/unnamed areas)
            entries.append((aid, name))
    return sorted(entries)


def table(function, entries):
    lines = [
        "inline const std::unordered_map<std::string, const char*>& %s() {" % function,
        "  static const std::unordered_map<std::string, const char*> table = {",
    ]
    for aid, name in entries:
        lines.append('      {"%s", "%s"},' % (aid, esc(name)))
    lines += [
        "  };",
        "  return table;",
        "}",
    ]
    return lines


def main():
    entries = read_entries(SRC)
    ps3_entries = read_entries(SRC_PS3)

    lines = [
        "// Auto-generated from docs/cfdata_names*.txt by scripts/gen-area-names.py - DO NOT EDIT",
        "#pragma once",
        "",
        "#include <string>",
        "#include <unordered_map>",
        "",
        "namespace eternalsonata {",
        "",
        '// Maps a cfdata area id (e.g. "tnk01") to its display name (the map-info',
        "// BTX string 0 of the area's cfdata file). Event/support files (eXXXX,",
        "// *60, zzz02, ...) are included with empty string names.",
    ]
    lines += table("AreaNameTable", entries)
    lines += [
        "",
        "// The PS3's areas the 360 lacks, for PS3 mode only.",
    ]
    lines += table("Ps3AreaNameTable", ps3_entries)
    lines += [
        "",
        "}  // namespace eternalsonata",
        "",
    ]

    with open(DST, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote %d + %d PS3 entries to %s" % (len(entries), len(ps3_entries), DST))


if __name__ == "__main__":
    main()
