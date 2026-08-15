#!/usr/bin/env python3
"""Turn the site table from party_relocation_scan.py into mid-ASM hook config
and the matching C++ hook bodies.

Strategy
--------
The four per-character arrays sit back to back with no padding, so none of them
can grow in place. But they do not all have to move. Relocating the two *cold*
arrays leaves holes that the two *hot* ones grow into:

    position    0x8243FC08  10 * 4  ends 0x8243FC30  stays put
    slotbytes   0x8243FC30  10 * 1  ends 0x8243FC3A  RELOCATED
    stats_live  0x8243FD08  10 * 48 ends 0x8243FEE8  stays put
    stats_base  0x8243FEE8  10 * 48 ends 0x82440068  RELOCATED

Moving `slotbytes` frees 0x8243FC30..0x8243FC3A, and `position` at twelve
entries needs 0x8243FC08..0x8243FC38, which fits. Moving `stats_base` frees
0x8243FEE8 onwards, and `stats_live` at twelve entries needs up to 0x8243FF48,
which also fits. So the two most heavily referenced arrays are never touched.

Two more pairs join them for the same reason: the read-only `template` table,
and the pending-award queue, whose id half (`charflags`) has two spare bytes and
stays put while its amount half (`charwords`) is followed by a live global and
moves. See RELOCATED and GROWN_IN_PLACE below for the current set.

Hook shapes
-----------
`addi` sites: hook fires *after* the instruction, when the destination register
holds the finished old address, and remaps it into the new range. One shared
hook per array covers every addi site.

Load/store sites: the address is `base + displacement` inside a single
instruction, so there is nothing to rewrite after the fact. The hook fires
*before*, shifts the base register by the relocation delta, and a second hook
fires *after* to shift it back, because the same register is often still live
(and, at five sites, is a `lis` shared with a different array). Where the load
writes its own base register the restore is skipped, since the original value is
gone either way and nothing downstream can want it.

Usage:
    python scripts/party_relocation_emit.py --sites sites.json \\
        --toml eternalsonata_config.toml --cpp src/party_relocation.generated.cpp
"""

from __future__ import annotations

import argparse
import json
import sys

# Arrays that move, and where they move to. The new bases are fixed guest
# addresses reserved with VirtualHeap::AllocFixed at startup, well clear of the
# XEX image, so the hooks can treat them as compile-time constants.
RELOCATED = {
    "slotbytes":  dict(old=0x8243FC30, count=12, stride=1,  new=0x8B000000),
    "stats_base": dict(old=0x8243FEE8, count=12, stride=48, new=0x8B000100),
    # The template table is read-only source data in the image rather than .bss
    # state, and it has the "adg01" string table hard against its end, so it
    # moves for the same reason the other two do. Unlike them it has to be
    # *seeded* from the ten originals at startup; see ReservePartyRelocationMemory.
    "template":   dict(old=0x82016150, count=12, stride=136, new=0x8B001000),
    # The amount half of the pending-award queue. Its id half (charflags) has two
    # spare bytes and stays put; this one is followed by a live global, so it
    # moves. It is .bss state like the first two, so a zeroed copy is correct.
    "charwords":  dict(old=0x824400C8, count=12, stride=2,  new=0x8B001800),
}

# Arrays that stay put but gain two entries by growing into the vacated space.
GROWN_IN_PLACE = {
    "position":   dict(base=0x8243FC08, stride=4,  old_end=0x8243FC30,
                       new_end=0x8243FC38),
    "stats_live": dict(base=0x8243FD08, stride=48, old_end=0x8243FEE8,
                       new_end=0x8243FF48),
    # The id half of the pending-award queue. Ten bytes with exactly two spare
    # before stats_live, so twelve entries fit in place. Nothing in the image
    # references its end address, so this entry usually contributes no hooks; it
    # is here so that a bound appearing later is caught rather than ignored.
    "charflags":  dict(base=0x8243FCFC, stride=1,  old_end=0x8243FD06,
                       new_end=0x8243FD08),
}

# Old end address -> the grown array whose end it is. A loop that stops when a
# walking pointer reaches one of these has to stop at the new end instead.
SENTINEL_MOVES = {cfg["old_end"]: name for name, cfg in GROWN_IN_PLACE.items()}

# The same idea for the arrays that move. A relocated array's end address is
# also the next array's base -- stats_base ends at 0x824400C8, which is exactly
# where charwords begins -- so a loop bound there reads as a reference to the
# neighbour and would be given the neighbour's delta, sending the bound to a
# different part of the reserved page than the pointer it is compared against.
# Keyed by old end address, valued with the array whose end it is.
RELOCATED_ENDS = {cfg["old"] + cfg["stride"] * 10: name
                  for name, cfg in RELOCATED.items()}

# Mid-ASM hooks are keyed by instruction address in the codegen, so an address
# can carry exactly one hook. A load or store needs its base register shifted
# before the instruction and restored after, which is two hooks at one address;
# the shift therefore goes on the *preceding* instruction with
# after_instruction = true, which is only sound when nothing branches straight
# to the site. The scan records that as `branch_target`, and a site that is one
# is reported for review rather than hooked.
#
# Two of the five sites that need this land on an instruction that already
# carries a hand-written hook from src/party_counts.cpp. One address still means
# one hook, so those two get a merged hook that does both jobs.
MERGED_WITH_COUNT_HOOK = {
    0x821E5AA0: "PartyCount_ClearPositionTail_ShiftSlotbytes",
    0x821E5DB4: "PartyCount_ClearPositionTail_ShiftSlotbytes",
}

BEGIN = "# >>> generated by scripts/party_relocation_emit.py -- do not edit by hand"
END = "# <<< end generated party relocation hooks"

# Integer loads only. The restore hook is dropped when a load overwrites its own
# base register, and that test compares rD against rA -- which is only meaningful
# when rD names a GPR. A float load writes an FPR, so `lfs f11, x(r11)` would
# compare 11 against 11 and wrongly conclude the base register was clobbered,
# leaving the shifted value live for whatever reads r11 next.
LOAD_FORMS = {"lwz", "lwzu", "lbz", "lbzu", "lhz", "lhzu", "lha", "lhau", "lmw"}

FLOAT_LOAD_FORMS = {"lfs", "lfsu", "lfd", "lfdu"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sites", required=True)
    ap.add_argument("--toml", required=True)
    ap.add_argument("--cpp", required=True)
    args = ap.parse_args()

    with open(args.sites, encoding="utf-8") as fh:
        sites = json.load(fh)

    # An indexed load or store that merely dereferences a register an earlier
    # `addi` site already built is not its own site: the scan flags it, and
    # hooking it too would apply the delta twice.
    redundant = [s for s in sites if s.get("redundant")]
    sites = [s for s in sites if not s.get("redundant")]

    # Sites that use a *grown* array's old end address as a loop bound. Nothing
    # relocates here, but the bound has to follow the array to its new end, or
    # the loop silently keeps running to ten. These read as the next array's
    # base because that is exactly what the old end address is.
    sentinel_fixes = [s for s in sites
                      if s["role"] == "sentinel"
                      and s["target"] in SENTINEL_MOVES]

    # A bound on a *relocated* array's end has to follow that array to its new
    # home, not follow the neighbour whose base shares the address.
    relocated_bounds = [s for s in sites
                        if s["role"] == "sentinel"
                        and s["target"] in RELOCATED_ENDS]

    unresolved = [s for s in sites
                  if s["role"] in ("sentinel_or_base", "ambiguous_fold")
                  or (s["role"] == "sentinel"
                      and s["target"] not in SENTINEL_MOVES
                      and s["target"] not in RELOCATED_ENDS)]

    # A load or store whose shift would have to sit on the preceding
    # instruction, at a site something branches to. The shift would run once and
    # every later pass through the loop would use the old address, so these are
    # reported rather than emitted.
    unsafe = [s for s in sites
              if s["array"] in RELOCATED
              and s["role"] in ("element", "below_base")
              and s["form"] != "addi"
              and s["branch_target"]]
    unresolved += unsafe

    # Folded bases (`base - stride`, indexed with a 1-based character id) need
    # exactly the same delta as the base itself, so they are emitted alongside
    # the element sites rather than left for review. Folded bases of the two
    # arrays that stay put need nothing at all.
    reloc = [s for s in sites
             if s["array"] in RELOCATED
             and s["role"] in ("element", "below_base")
             and not s["branch_target"]]

    hooks: list[dict] = []
    for s in reloc:
        hooks += site_hooks(s, s["array"], "Reloc")
    for s in sentinel_fixes:
        hooks += site_hooks(s, SENTINEL_MOVES[s["target"]], "Bound")
    for s in relocated_bounds:
        hooks += site_hooks(s, RELOCATED_ENDS[s["target"]], "RelocEnd")

    # One hook per address is all the codegen can hold, so a collision would
    # silently drop one of the two. Catch it here instead.
    seen: dict[tuple[int, bool], str] = {}
    for h in hooks:
        key = (h["address"], h["after"])
        if key in seen and seen[key] != h["name"]:
            raise SystemExit(
                f"two different hooks at {h['address']:#010x} "
                f"(after={h['after']}): {seen[key]} and {h['name']}. "
                "The codegen keeps one hook per address; merge them.")
        seen[key] = h["name"]

    write_toml(args.toml, hooks, unresolved)
    write_cpp(args.cpp, hooks)

    print(f"{len(reloc)} relocated sites + {len(sentinel_fixes)} moved bounds "
          f"+ {len(relocated_bounds)} relocated-array bounds -> {len(hooks)} hooks")
    print(f"{len(redundant)} indexed sites skipped as redundant")
    print(f"{len(unresolved)} sites left for manual review")
    for s in unresolved:
        print(f"  MANUAL {s['addr']:#010x} {s['form']:6s} -> {s['target']:#010x} "
              f"{s['array']} {s['role']} {s['func']}")
    return 0


def site_hooks(s: dict, group: str, kind: str) -> list[dict]:
    """Build the hooks for one site.

    `addi` finishes the address in a register, so a single hook after the
    instruction can rewrite it. A load or store forms `base + displacement`
    inside the instruction itself, with nothing to rewrite afterwards, so the
    base register is shifted before and shifted back after; the restore is
    dropped when the instruction overwrites its own base register, since the
    old value is gone either way. That only applies to integer loads: a float
    load writes an FPR and always leaves its base GPR intact, so it always needs
    the restore.
    """
    form = s["form"]
    if form == "addi":
        return [dict(address=s["addr"], name=f"Party{kind}_{group}_Remap",
                     registers=[f"r{s['reg']}"], after=True, site=s)]

    # The shift cannot share the site's address with the restore, so it goes on
    # the instruction before, fired after that one has run. Control has to be
    # unable to enter in between.
    shift_at = s["addr"] - 4
    name = MERGED_WITH_COUNT_HOOK.get(shift_at,
                                      f"Party{kind}_{group}_Shift")
    out = [dict(address=shift_at, name=name,
                registers=[f"r{s['base_reg']}"], after=True, site=s)]
    if not (form in LOAD_FORMS and s["dest_reg"] == s["base_reg"]):
        out.append(dict(address=s["addr"], name=f"Party{kind}_{group}_Unshift",
                        registers=[f"r{s['base_reg']}"], after=True, site=s))
    return out


def write_toml(path: str, hooks, unresolved) -> None:
    with open(path, encoding="utf-8") as fh:
        text = fh.read()

    if BEGIN in text:
        head = text[:text.index(BEGIN)]
        tail = text[text.index(END) + len(END):]
    else:
        head, tail = text.rstrip() + "\n\n", "\n"

    lines = [BEGIN,
             f"# {len(hooks)} hooks relocating "
             f"{', '.join(sorted(RELOCATED))} so the arrays that stay put can",
             "# grow from ten to twelve entries. Regenerate, never hand edit.",
             f"# {len(unresolved)} sites whose role the scanner could not settle "
             "are deliberately absent; see the script output.",
             ""]
    for h in hooks:
        s = h["site"]
        lines.append("[[midasm_hook]]")
        lines.append(f"address = {h['address']:#010x}  "
                     f"# {s['form']} -> {s['target']:#010x} in {s['func'] or 'no function'}")
        lines.append(f'name = "{h["name"]}"')
        lines.append(f'registers = ["{h["registers"][0]}"]')
        if h["after"]:
            lines.append("after_instruction = true")
        lines.append("")
    lines.append(END)

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(head + "\n".join(lines) + tail)


def write_cpp(path: str, hooks) -> None:
    used = {h["name"] for h in hooks}
    out = ['// Generated by scripts/party_relocation_emit.py -- do not edit.',
           '//',
           '// Relocates the cold per-character arrays out of the way so the hot',
           '// ones can grow from ten to twelve entries in place. See the script',
           '// for the layout reasoning.',
           '',
           '#include "party_relocation.h"',
           '',
           '#include <rex/ppc/context.h>',
           '']
    out += ['namespace {',
            '',
            '// The new bases belong to party_relocation.h, which owns the guest',
            '// memory they point at. Assert rather than redeclare, so the two',
            '// halves cannot drift apart.']
    for name, cfg in sorted(RELOCATED.items()):
        tag = name.title().replace("_", "")
        span = cfg["count"] * cfg["stride"]
        out += [f'static_assert(eternalsonata::k{tag}Base == {cfg["new"]:#010x},',
                f'              "{name} moved; regenerate the relocation hooks");',
                f'static_assert({cfg["new"]:#010x} + {span} <= '
                f'eternalsonata::kRelocationBase + eternalsonata::kRelocationSize,',
                f'              "{name} ({cfg["count"]} * {cfg["stride"]} bytes) '
                f'does not fit the reserved page");']
    out += [f'static_assert(eternalsonata::kRelocatedCharacterCount == '
            f'{max(c["count"] for c in RELOCATED.values())},',
            '              "the hook table was generated for a different cast size");',
            '',
            '}  // namespace',
            '',
            '// The hook bodies are looked up by name by the recompiler, so they',
            '// keep external linkage.',
            '']

    groups = [("Reloc", name,
               RELOCATED[name]["new"] - RELOCATED[name]["old"],
               f'{name}: {RELOCATED[name]["old"]:#010x} -> '
               f'{RELOCATED[name]["new"]:#010x}')
              for name in sorted(RELOCATED)]
    # A loop bound sitting on a relocated array's end address: it has to land on
    # the end of the *twelve*-entry array in its new home.
    groups += [("RelocEnd", name,
                (RELOCATED[name]["new"] + RELOCATED[name]["stride"]
                 * RELOCATED[name]["count"])
                - (RELOCATED[name]["old"] + RELOCATED[name]["stride"] * 10),
                f'{name} loop bound: '
                f'{RELOCATED[name]["old"] + RELOCATED[name]["stride"] * 10:#010x} -> '
                f'{RELOCATED[name]["new"] + RELOCATED[name]["stride"] * RELOCATED[name]["count"]:#010x} '
                f'(the end of the relocated array, two entries longer)')
               for name in sorted(RELOCATED)]
    groups += [("Bound", name,
                GROWN_IN_PLACE[name]["new_end"] - GROWN_IN_PLACE[name]["old_end"],
                f'{name} loop bound: {GROWN_IN_PLACE[name]["old_end"]:#010x} -> '
                f'{GROWN_IN_PLACE[name]["new_end"]:#010x} (two more entries)')
               for name in sorted(GROWN_IN_PLACE)]

    for kind, name, delta, comment in groups:
        prefix = f"Party{kind}_{name}"
        if not any(h.startswith(prefix + "_") for h in used):
            continue
        tag = f'{kind}{name.title().replace("_", "")}'
        out += [f'// {comment}',
                f'constexpr int32_t k{tag}Delta = '
                f'static_cast<int32_t>({delta:#010x});',
                '']
        if f"{prefix}_Remap" in used:
            out += [f'void {prefix}_Remap(PPCRegister& reg) {{',
                    '    // fired after the addi, so reg holds the finished '
                    'old address',
                    f'    reg.u32 += k{tag}Delta;',
                    '}',
                    '']
        if f"{prefix}_Shift" in used:
            out += [f'void {prefix}_Shift(PPCRegister& reg) {{',
                    f'    reg.u32 += k{tag}Delta;',
                    '}',
                    '']
        if f"{prefix}_Unshift" in used:
            out += [f'void {prefix}_Unshift(PPCRegister& reg) {{',
                    f'    reg.u32 -= k{tag}Delta;',
                    '}',
                    '']

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
