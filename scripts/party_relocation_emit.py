#!/usr/bin/env python3
"""Turn the site table from party_relocation_scan.py into mid-ASM hook config
and the matching C++ hook bodies.

Strategy
--------
The seven per-character arrays sit back to back with no padding, so none of them
can grow in place. Every one of them therefore moves to a reserved page, and the
whole original block is left behind as dead space.

    position    0x8243FC08  10 * 4   ends 0x8243FC30  RELOCATED
    charflags   0x8243FCFC  10 * 1   ends 0x8243FD06  RELOCATED
    slotbytes   0x8243FC30  10 * 1   ends 0x8243FC3A  RELOCATED
    stats_live  0x8243FD08  10 * 48  ends 0x8243FEE8  RELOCATED
    stats_base  0x8243FEE8  10 * 48  ends 0x824400C8  RELOCATED
    charwords   0x824400C8  10 * 2   ends 0x824400DC  RELOCATED
    template    0x82016150  10 * 136 ends 0x820166A0  RELOCATED

An earlier revision moved only the four *cold* arrays and let the three hot ones
grow into the space that freed up: `position` grew from 0x8243FC30 to 0x8243FC38,
over slotbytes' old home, and `stats_live` grew over stats_base's. That halved
the hook count, and it made every enumeration miss unrecoverable. The site table
is built from IDA's data xrefs plus a linear constant-propagation scan, and
neither is complete; a site the sweep never saw keeps addressing the old block.
Under grow-in-place that stale access lands *inside a live array* -- a missed
slotbytes store writes over position[10] and position[11] -- so the failure is
silent corruption of the two new characters, which is exactly the symptom that
sent the first live build back.

With everything relocated, the original block is untouched by any array, so a
missed site reads and writes dead memory instead. party_relocation.cpp fills that
block with a canary and checks it once per guest frame, which turns the miss into
a report naming the address. Growing in place is a size optimisation and can come
back once that check runs clean; it buys nothing functional.

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

The shift cannot share an address with the restore, so it goes on the preceding
instruction. Once every array relocates, that collides: the reset paths clear the
position table as ten consecutive `stw`s through one register, so site N's
restore and site N+1's shift both want 0x821E5A7C and friends. Consecutive sites
sharing a base register are therefore collapsed into a *run*: shift once before
the run, adjust by the difference at each point where the array changes (those
runs end by walking off position into slotbytes), and restore once after the
last. See build_runs().

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
    # The three that used to grow in place. They are the hot ones, and moving
    # them is what makes the original block dead space the canary can watch.
    "position":   dict(old=0x8243FC08, count=12, stride=4,  new=0x8B001900),
    "charflags":  dict(old=0x8243FCFC, count=12, stride=1,  new=0x8B001A00),
    "stats_live": dict(old=0x8243FD08, count=12, stride=48, new=0x8B002000),
}

# Arrays that stay put but gain two entries by growing into the vacated space.
# Empty on purpose: see the module docstring for why grow-in-place was dropped.
# The machinery below is kept because the optimisation is worth restoring once
# the canary check in party_relocation.cpp runs clean across a full playthrough.
GROWN_IN_PLACE: dict[str, dict] = {}

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
    0x821E5AA0: "PartyCount_ClearPositionTail_RebaseSlotbytes",
    0x821E5DB4: "PartyCount_ClearPositionTail_RebaseSlotbytes",
}

# Hook bodies whose delta is not simply "one array's move", collected while the
# hooks are built and emitted by write_cpp. name -> (delta, comment).
EXTRA_BODIES: dict[str, tuple[int, str]] = {}


def delta_of(name: str) -> int:
    """How far `name` moved."""
    cfg = RELOCATED[name]
    return cfg["new"] - cfg["old"]


def new_end(name: str) -> int:
    """One past the last entry of `name` in its new home, at twelve entries."""
    cfg = RELOCATED[name]
    return cfg["new"] + cfg["stride"] * cfg["count"]


def add_body(name: str, delta: int, comment: str) -> str:
    """Record a hook body, checking that a name never means two deltas."""
    delta &= 0xFFFFFFFF
    prev = EXTRA_BODIES.get(name)
    if prev is not None and prev[0] != delta:
        raise SystemExit(f"{name} would need two different deltas, "
                         f"{prev[0]:#010x} and {delta:#010x}")
    EXTRA_BODIES[name] = (delta, comment)
    return name


def rebase_body(frm: str, to: str) -> str:
    """A register that already carries `frm`'s delta but is addressing `to`.

    Both the run case (a walking pointer crossing from one array into the next)
    and the chained case (`addi rD, rA, (to - frm)`, where rA has already been
    moved) leave the register holding `to_old + delta(frm)`, and both want
    `to_old + delta(to)`.
    """
    return add_body(f"PartyRebase_{frm}_To_{to}",
                    delta_of(to) - delta_of(frm),
                    f"{frm} -> {to}: a register carrying {frm}'s delta while it "
                    f"addresses {to}.")


def rebase_end_body(frm: str, group: str, target: int) -> str:
    """The same, for a site that builds a *loop bound* off another array's base.

    `addi rD, rA, (end - base)` where rA already carries `frm`'s delta leaves rD
    at `end_old + delta(frm)`, which is neither array's end. What it wants is the
    end of the twelve-entry array in its new home, so the correction is whatever
    closes that particular gap -- for a bound reached from its own array's base
    that works out to two entries' worth of stride, which is the two characters
    the array grew by.
    """
    current = (target + delta_of(frm)) & 0xFFFFFFFF
    return add_body(
        f"PartyRebaseEnd_{frm}_To_{group}",
        new_end(group) - current,
        f"{group} loop bound reached from {frm}: {current:#010x} -> "
        f"{new_end(group):#010x} (the end of the relocated array, two entries "
        f"longer)")


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
    # the element sites rather than left for review.
    #
    # `branch_target` only disqualifies a load or store. Those need their base
    # register shifted by a hook on the *preceding* instruction, which is only
    # sound if the preceding instruction is the only way in -- and `unsafe`
    # above collects the ones where it is not. An `addi` has no such
    # requirement: its hook fires after the instruction and rewrites the address
    # the instruction just finished, so it is correct however control arrived.
    # Filtering both forms on `branch_target` dropped 11 real addi sites (7
    # slotbytes, 4 charflags), which then kept addressing the arrays' old homes.
    reloc = [s for s in sites
             if s["array"] in RELOCATED
             and s["role"] in ("element", "below_base")
             and (s["form"] == "addi" or not s["branch_target"])]

    # `addi` sites are independent: the hook fires after the instruction and
    # rewrites the finished address, so nothing is left live across a boundary.
    # Load and store sites are not, and are emitted per run below.
    def chained_hook(s: dict, body: str) -> dict:
        return dict(address=s["addr"],
                    name=MERGED_WITH_COUNT_HOOK.get(s["addr"], body),
                    body=body, registers=[f"r{s['reg']}"], after=True, site=s)

    hooks: list[dict] = []
    for s in reloc:
        if s["form"] != "addi":
            continue
        # A site that reaches its array as an offset from a *different* one --
        # `addi r9, r11, (charflags - charwords)` after r11 has already been
        # moved to charwords' new home. Adding its own delta on top would apply
        # two, which is what crashed the first build of this layout on startup.
        # What it needs is the difference.
        if s.get("chained_from"):
            hooks.append(chained_hook(s, rebase_body(s["chained_from"],
                                                     s["array"])))
        else:
            hooks += site_hooks(s, s["array"], "Reloc")
    for s in sentinel_fixes:
        hooks += site_hooks(s, SENTINEL_MOVES[s["target"]], "Bound")
    for s in relocated_bounds:
        group = RELOCATED_ENDS[s["target"]]
        # A bound can be chained too: `addi rD, rA, (end - base)` off a register
        # that already carries some array's delta. Two of these exist, and both
        # build an array's own end off its own base.
        if s.get("chained_from"):
            hooks.append(chained_hook(
                s, rebase_end_body(s["chained_from"], group, s["target"])))
        else:
            hooks += site_hooks(s, group, "RelocEnd")

    runs = build_runs([s for s in reloc if s["form"] != "addi"])
    for run in runs:
        hooks += run_hooks(run)

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
    multi = [r for r in runs if len(r) > 1]
    print(f"{len(runs)} load/store runs ({len(multi)} of more than one site, "
          f"longest {max((len(r) for r in runs), default=0)})")
    print(f"{len(redundant)} indexed sites skipped as redundant")
    print(f"{len(unresolved)} sites left for manual review")
    for s in unresolved:
        print(f"  MANUAL {s['addr']:#010x} {s['form']:6s} -> {s['target']:#010x} "
              f"{s['array']} {s['role']} {s['func']}")
    return 0


def clobbers_own_base(s: dict) -> bool:
    """True if this instruction leaves its own base register holding something
    else, so there is nothing to restore afterwards. Integer loads only: a float
    load writes an FPR, and comparing its rD against rA would be comparing a
    float register number with a GPR number."""
    return s["form"] in LOAD_FORMS and s["dest_reg"] == s["base_reg"]


def build_runs(sites: list[dict]) -> list[list[dict]]:
    """Group load/store sites into runs of consecutive instructions that share a
    base register.

    A single site needs its base shifted before the instruction and shifted back
    after, and since one address can only carry one hook, the shift has to sit on
    the *preceding* instruction. Adjacent sites therefore fight over an address:
    site N's restore and site N+1's shift both want N's address. That is not a
    rare shape -- the two reset paths clear the position table as ten consecutive
    `stw`s through r31, and then walk straight on into the slotbytes byte clear,
    which is ten collisions per path.

    Collapsing them is also what the code means: the register holds one walking
    pointer for the whole run, so it wants shifting once on the way in and
    restoring once on the way out. Where the array changes mid-run the delta
    changes with it, which is a third hook shape (see run_hooks).

    A run ends at a gap in addresses, at a change of base register, or at an
    instruction that overwrites its own base register, after which the register
    holds an unrelated value and the next site is starting over.
    """
    runs: list[list[dict]] = []
    for s in sorted(sites, key=lambda s: s["addr"]):
        run = runs[-1] if runs else None
        prev = run[-1] if run else None
        if (prev is not None
                and s["addr"] == prev["addr"] + 4
                and s["base_reg"] == prev["base_reg"]
                and not clobbers_own_base(prev)):
            run.append(s)
        else:
            runs.append([s])
    return runs


def run_hooks(run: list[dict]) -> list[dict]:
    """Hooks for one run: shift in, rebase at every array change, restore out.

    Every site in a run is guaranteed to have `branch_target` false (the caller
    drops those), so control cannot enter partway and find the register already
    shifted, and the shift placed on the instruction before the run cannot be
    jumped over.
    """
    first, last = run[0], run[-1]
    reg = first["base_reg"]

    def hook(address: int, body: str, site: dict) -> dict:
        # An address that already carries a hand-written hook from
        # src/party_counts.cpp keeps that name, since the codegen holds one hook
        # per address; the hand-written body calls `body` itself. `body` is
        # recorded either way so the generated function is still emitted.
        return dict(address=address,
                    name=MERGED_WITH_COUNT_HOOK.get(address, body),
                    body=body, registers=[f"r{reg}"], after=True, site=site)

    out = [hook(first["addr"] - 4, f"PartyReloc_{first['array']}_Shift", first)]

    # Where a run crosses from one array into the next -- the position clear
    # walking on into the slotbytes clear -- the register keeps its pointer but
    # needs a different delta from that instruction on. Correct it on the site
    # *before* the change, which is also where a restore would have gone, so the
    # two shapes never both want the same address.
    for a, b in zip(run, run[1:]):
        if a["array"] != b["array"]:
            out.append(hook(a["addr"],
                            rebase_body(a["array"], b["array"]), a))

    if not clobbers_own_base(last):
        out.append(hook(last["addr"],
                        f"PartyReloc_{last['array']}_Unshift", last))
    return out


def site_hooks(s: dict, group: str, kind: str) -> list[dict]:
    """Build the hook for one `addi` site.

    `addi` finishes the address in a register, so a single hook after the
    instruction can rewrite it, and nothing is left live across a boundary the
    way a shifted base register is. Load and store sites go through build_runs()
    and run_hooks() instead; every loop bound in the table is an `addi`, so this
    only ever sees that form. If a non-addi one turns up, it needs the run
    treatment and a group of its own, not a shift/restore pair improvised here.
    """
    if s["form"] != "addi":
        raise SystemExit(
            f"{s['addr']:#010x} is a {s['form']} in the {kind} group, which only "
            "handles addi; route it through build_runs() instead")
    name = f"Party{kind}_{group}_Remap"
    return [dict(address=s["addr"], name=name, body=name,
                 registers=[f"r{s['reg']}"], after=True, site=s)]


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
    # Key off the *body* name, not the hook name: an address that already holds a
    # hand-written hook from src/party_counts.cpp is registered under that name
    # and calls the generated function itself, so the function is still needed.
    used = {h["body"] for h in hooks}
    out = ['// Generated by scripts/party_relocation_emit.py -- do not edit.',
           '//',
           '// Relocates every per-character array out of the original block, so',
           '// that block can be left poisoned and any site the sweep missed shows',
           '// up as a canary hit rather than as corruption. See the script for the',
           '// layout reasoning.',
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

    # Corrections for a register that already carries some other array's delta:
    # a walking pointer that has crossed from one array into the next, or an
    # `addi` that reaches its array as an offset from a different one. Both leave
    # the register short by exactly the difference between the two moves, so this
    # adjusts rather than restoring and re-shifting -- which would need two hooks
    # at one address, the thing runs exist to avoid.
    for name in sorted(used & EXTRA_BODIES.keys()):
        delta, comment = EXTRA_BODIES[name]
        out += [f'// {comment}',
                f'void {name}(PPCRegister& reg) {{',
                f'    reg.u32 += static_cast<int32_t>({delta:#010x});',
                '}',
                '']

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
