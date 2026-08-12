#!/usr/bin/env python3
"""Generate src/area_names.generated.h from docs/cfdata_names.txt.

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
DST = os.path.join(ROOT, "src", "area_names.generated.h")


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    entries = []
    with open(SRC, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if "\t" not in line:
                continue
            aid, name = line.split("\t", 1)
            # Include all entries, even those with empty names (event/unnamed areas)
            entries.append((aid, name))
    entries.sort()

    lines = [
        "// Auto-generated from docs/cfdata_names.txt by scripts/gen-area-names.py - DO NOT EDIT",
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
        "inline const std::unordered_map<std::string, const char*>& AreaNameTable() {",
        "  static const std::unordered_map<std::string, const char*> table = {",
    ]
    for aid, name in entries:
        lines.append('      {"%s", "%s"},' % (aid, esc(name)))
    lines += [
        "  };",
        "  return table;",
        "}",
        "",
        "}  // namespace eternalsonata",
        "",
    ]

    with open(DST, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote %d named entries to %s" % (len(entries), DST))


if __name__ == "__main__":
    main()
