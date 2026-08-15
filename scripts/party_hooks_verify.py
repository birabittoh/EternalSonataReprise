#!/usr/bin/env python3
"""Check that every [[midasm_hook]] in the config actually reached the generated
code.

Worth its own script because of how the codegen stores hooks: they live in a
`std::unordered_map<uint32_t, MidAsmHook>` keyed by address, so **one address
holds exactly one hook** and a second declaration at the same address silently
replaces the first. Nothing fails, nothing warns at the default log level, and
the build links cleanly -- the hook is simply not there at runtime.

That is easy to walk into. A load or store needs its base register shifted
before the instruction and restored after, which is two hooks at one address;
the relocation emitter therefore puts the shift on the preceding instruction.
Run this after regenerating to confirm nothing was dropped:

    python scripts/build.py           # regenerates generated/
    python scripts/party_hooks_verify.py

A mismatch means either two hooks collided at one address or a hook was declared
for an address the codegen never emitted (e.g. inside a function it did not
recompile).
"""

from __future__ import annotations

import glob
import re
import sys
from collections import Counter

CONFIG = "eternalsonata_config.toml"
GENERATED = "generated/*.cpp"


def main() -> int:
    with open(CONFIG, encoding="utf-8") as fh:
        cfg = fh.read()

    # Each [[midasm_hook]] block runs to the next [[ ... ]] header.
    blocks = re.findall(r"\[\[midasm_hook\]\]\s*\n((?:(?!\[\[).*\n)*)", cfg)

    declared: Counter[str] = Counter()
    addresses: dict[str, list[str]] = {}
    for block in blocks:
        name = re.search(r'name\s*=\s*"([^"]+)"', block)
        addr = re.search(r"address\s*=\s*(0x[0-9a-fA-F]+)", block)
        if not name or not addr:
            print(f"malformed [[midasm_hook]] block:\n{block}", file=sys.stderr)
            return 2
        declared[name.group(1)] += 1
        addresses.setdefault(name.group(1), []).append(addr.group(1))

    # The codegen emits a hook as a call statement at the start of a line, either
    # bare or as the condition of an `if` for the bool-returning forms.
    emitted: Counter[str] = Counter()
    for path in glob.glob(GENERATED):
        with open(path, encoding="utf-8", errors="ignore") as fh:
            text = fh.read()
        for m in re.finditer(r"^\t([A-Za-z_][A-Za-z0-9_]*)\(", text, re.M):
            emitted[m.group(1)] += 1
        for m in re.finditer(r"^\tif \(([A-Za-z_][A-Za-z0-9_]*)\(", text, re.M):
            emitted[m.group(1)] += 1

    if not emitted:
        print(f"no generated sources matched {GENERATED}; run the build first",
              file=sys.stderr)
        return 2

    bad = 0
    for name, count in sorted(declared.items()):
        got = emitted.get(name, 0)
        if got == count:
            continue
        bad += 1
        print(f"MISSING {name}: declared {count}, emitted {got}")
        print(f"        at {', '.join(addresses[name])}")

    total = sum(declared.values())
    if bad:
        print(f"\n{bad} of {len(declared)} hook names did not survive codegen "
              f"({total} hooks declared)")
        return 1

    print(f"all {total} declared hooks emitted ({len(declared)} distinct names)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
