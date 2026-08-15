#!/usr/bin/env python3
"""Sweep default.xex for every instruction that touches the per-character party
arrays, so the 10 -> 12 character relocation can be driven from a generated
table instead of sixty hand edits.

The sweep is a raw linear decode of the executable segments rather than a walk
over IDA's function list, because at least one site (0x820E7AC0) lives in code
IDA never assigned to a function, and because a single `lis` is sometimes shared
by `addi`s that resolve to two different arrays.

Usage:
    python scripts/party_relocation_scan.py --idb <path to .i64> [--json out.json]

Requires IDA's `idalib` Python bindings (the `idapro` module) on sys.path. Run
against a *copy* of the database if the real one is open in IDA, since idalib
takes an exclusive lock.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass, field, asdict

# --- the arrays we care about ------------------------------------------------
#
# Each entry is (name, base, stride, count, end). `end` is where the array stops
# today, which for three of the four is exactly where the next live structure
# begins.

ARRAYS = [
    # display position of character c at [c-1], 0 = not in party
    ("position", 0x8243FC08, 4, 10, 0x8243FC30),
    # ten per-character bytes; note the base doubles as the position array's
    # loop-end sentinel, so sites hitting exactly this address are ambiguous
    ("slotbytes", 0x8243FC30, 1, 10, 0x8243FC3A),
    # equipment-adjusted stats, what the screens draw
    ("stats_live", 0x8243FD08, 48, 10, 0x8243FEE8),
    # the character's own stats, what a save holds
    # NB the end is base + 48 * 10 = 0x824400C8, which is exactly where
    # `charwords` begins -- the two are adjacent like everything else in this
    # block. An earlier revision of this table had 0x82440068 here, which is
    # base + 384 (eight entries, not ten), and that silently dropped every site
    # addressing characters 9 and 10's base stats. The save writer settles it:
    # sub_82241190 memcpys 480 bytes from 0x8243FEE8.
    ("stats_base", 0x8243FEE8, 48, 10, 0x824400C8),
    # the read-only per-character template the init and level-up paths build the
    # stats from. Ten 136-byte entries, immediately followed by the "adg01"
    # string table at 0x820166A0, so it has no more room to grow than the others
    # do. Unlike them it lives in the image rather than in .bss, and it is
    # *source* data: relocating it means seeding the copy from the ten originals.
    #
    # The bounds are pinned by the entry id at +0, which reads 1, 2, 3 at
    # 0x82016150, 0x820161D8, 0x82016260. Do not derive the base from the
    # 0x82016188 that IDA named: that is field +0x38 of entry 1, and assuming it
    # was the base put the end four bytes into the string table and produced two
    # false sites in sub_820FA680.
    ("template", 0x82016150, 136, 10, 0x820166A0),
    # A ten-slot pending-award queue, held as two parallel arrays: the character
    # id in `charflags` and the amount in `charwords`. sub_821E7D88 appends to
    # the first free slot and gives up at ten; sub_821E7668 searches the same ten
    # for a character. They are per-character sized (one slot per possible party
    # member), so a cast of twelve needs twelve slots.
    #
    # charflags has exactly two spare bytes before stats_live at 0x8243FD08, so
    # twelve entries fit where they are. charwords does not: 0x824400DC is a live
    # global with 20+ references of its own, so it is the sixth relocation.
    ("charflags", 0x8243FCFC, 1, 10, 0x8243FD06),
    ("charwords", 0x824400C8, 2, 10, 0x824400DC),
]

# Sites the forward-walking classifier cannot settle, resolved by reading the
# disassembly. Every one of these materialises 0x8243FEE8 into a long-lived
# register in a loop preheader, in most cases in the same breath as the
# stats_live base at 0x8243FD08, and the register is then copied, stored or
# indexed rather than compared. A loop bound is only ever consumed by a
# compare, so all of them are the stats_base *base*. The classifier misses them
# because the consuming use is past its straight-line window.
#
# Keep this table small and evidence-backed. If the scan starts reporting new
# unresolved sites, read them; do not extend this list on the assumption that
# the pattern repeats.
ROLE_OVERRIDES = {
    0x820E7AC8: "element",  # addi r5; r8 gets stats_live two insns later
    0x821FAC18: "element",  # addi r22 preheader, with entity table into r21
    0x821FB3F8: "element",  # same shape as 0x821FAC18
    0x82209500: "element",  # addi r9, then mr r10, r9; no compare
    0x82231F6C: "element",  # addi r17, with stats_live into r16 next insn
    0x82238A30: "element",  # addi r18 preheader, alongside unrelated bases
    0x82239340: "element",  # same shape as 0x82238A30
    0x82239ACC: "element",  # same shape as 0x82238A30
}

# Arrays whose "one stride below the base" window must NOT be read as a folded
# 1-based index. The window is one stride wide, which is fine for a 1- or 4-byte
# scalar array but is 136 bytes for the template table, and the bytes below it
# are a float constant pool (flt_82016120) that several hundred unrelated
# functions load from. Reading those as template sites produced 400+ false
# positives on the first run. The two functions that index the template
# (sub_821E7138, sub_821E8A50) both compute `id - 1` explicitly rather than
# folding it into the base, so the window buys nothing here; 0x820160CC, the
# only address a genuine fold could produce, has no references at all.
NO_FOLD = {"template"}

# Arrays whose exact end address must NOT be read as a loop-end sentinel. The
# four .bss arrays sit back to back and really are walked with pointer bounds,
# but the template table is only ever indexed by a counted loop, and the address
# one past its end is `off_820166A0`, the "adg01" string pointer table that
# sub_820FA680 binary-searches. Treating that as a template bound produced a
# false site there.
NO_END_SENTINEL = {"template", "charwords"}
# charwords is the same trap in a worse form: the address one past its end,
# 0x824400DC, is a live global with 20+ references of its own, none of which are
# loop bounds on this array. charflags keeps the rule, since 0x8243FD06 has no
# references at all and a genuine bound there is possible.

# A site that indexes with a 1-based character id folds the -1 into the base, so
# its constant is `base + 4*field - stride` and lands in [base - stride, base).
# `dword_8243FC04[id]` is the position array addressed that way. The window is
# therefore exactly one stride, not a fixed slack: widening it past that just
# swallows the unrelated globals that sit below each array.
FOLD_SLACK = max(a[2] for a in ARRAYS)

# The arrays no longer form one contiguous block: the template table sits in the
# image, megabytes below the .bss party state. Keep one interval per array and
# test membership against all of them, so the cheap prefilter in front of
# classify() stays cheap and the dref sweep does not walk the 4 MB of unrelated
# addresses that a single min..max span would cover.
BLOCKS = [(base - (0 if name in NO_FOLD else stride),
           end + (0 if name in NO_END_SENTINEL else 1))
          for name, base, stride, _, end in ARRAYS]


def in_blocks(target: int) -> bool:
    return any(lo <= target < hi for lo, hi in BLOCKS)

# --- minimal big-endian PPC decoding ----------------------------------------

# D-form memory ops: opcode -> (mnemonic, writes_rD)
DFORM_MEM = {
    32: ("lwz", True),   33: ("lwzu", True),
    34: ("lbz", True),   35: ("lbzu", True),
    36: ("stw", False),  37: ("stwu", False),
    38: ("stb", False),  39: ("stbu", False),
    40: ("lhz", True),   41: ("lhzu", True),
    42: ("lha", True),   43: ("lhau", True),
    44: ("sth", False),  45: ("sthu", False),
    46: ("lmw", True),   47: ("stmw", False),
    48: ("lfs", False),  49: ("lfsu", False),
    50: ("lfd", False),  51: ("lfdu", False),
    52: ("stfs", False), 53: ("stfsu", False),
    54: ("stfd", False), 55: ("stfdu", False),
}

# Opcodes whose bits 21..25 field is a *source* (stores) rather than a dest, so
# scanning must not treat them as clobbering that register.
STORE_OPCODES = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55}

# Opcodes that never write a GPR at all.
NO_GPR_WRITE = {11, 10, 16, 18, 17}  # cmpi, cmpli, bc, b, sc

# --- which field holds the destination register ------------------------------
#
# `addi rD, rA, SIMM` puts its destination in bits 21..25 and its source in bits
# 16..20. The logical, rotate and shift forms are the other way round:
# `ori rA, rS, UIMM` sources bits 21..25 and *writes* bits 16..20. Invalidating
# the wrong one of those two leaves a stale constant live in the register that
# was actually written, which is how a bare `lis rX, 0x8244` (the high half of
# an ordinary address like 0x8243D380) survived a `mr` and got read back as
# 0x82440000, an address inside stats_base. That invented eight sites in loops
# that have nothing to do with the party arrays, and hooking them would have
# shifted a walking pointer over an unrelated global.
#
# D-form opcodes whose destination is the bits 16..20 field.
RA_DEST_DFORM = {20, 21, 23, 24, 25, 26, 27, 28, 29}
# X-form (opcode 31) subopcodes whose destination is the bits 16..20 field:
# the logical, shift and sign-extend group.
RA_DEST_X = {
    24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476,
    536, 539, 792, 794, 824, 826, 922, 954, 986,
}
# X-form subopcodes that write no GPR at all: the compares and the non-update
# indexed stores.
NO_GPR_WRITE_X = {0, 32, 151, 215, 407, 663, 727}
# Indexed stores with update, which write only their base register.
STORE_UPDATE_X = {183, 247, 439, 695, 759}
# Indexed loads with update, which write both the loaded register and the base.
LOAD_UPDATE_X = {55, 119, 311, 375, 567, 631}
# `or rA, rS, rB` with rS == rB is `mr`, the copy the game uses to hand a
# finished address to a walking pointer. Tracking it is what keeps the genuine
# stats_base loop in sub_82209478 (which does `addi r9, ...FEE8` then
# `mr r10, r9`) from being thrown out along with the false positives above.
OR_X = 444


def signed16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


@dataclass
class Site:
    addr: int
    array: str
    target: int          # the guest address this site resolves to
    form: str            # "addi" | "lwz" | "stw" | ...
    reg: int             # dest reg for addi, base reg for loads/stores
    dest_reg: int        # rD field; for loads this is the register written
    base_reg: int        # register the constant came from
    base_value: int      # the constant that register held
    disp: int
    setup_addr: int      # where base_reg's constant was materialised
    setup_distance: int  # instructions between setup and use
    role: str            # "element" | "sentinel_or_base" | "below_base" | ...
    func: str = ""
    note: str = ""
    index_reg: int = -1  # rB, for X-form (indexed) loads and stores
    redundant: bool = False  # dereferences an address an earlier site builds
    branch_target: bool = False  # something jumps here, so the predecessor is
                                 # not the only way in


def is_element_addr(name: str, target: int) -> bool:
    """True if `target` can plausibly be an element or field address of `name`.

    The stats arrays are 48-byte structs, so any byte offset inside them is a
    legal field address. The two small arrays hold one scalar per character, so
    only offsets that are a multiple of the stride can be one of their elements.
    """
    STRUCT_STRIDE = 16   # anything wider than a scalar is addressed by field
    for n, base, stride, count, end in ARRAYS:
        if n != name:
            continue
        if not (base <= target < end):
            return False
        return stride >= STRUCT_STRIDE or (target - base) % stride == 0
    return False


def classify(target: int) -> tuple[str, str]:
    """Return (array name, role) for a resolved address, or ("", "") if outside."""
    # A constant exactly one stride below a base is that array addressed with a
    # 1-based character id, e.g. `byte_8243FC2F[id]` for slotbytes. This has to
    # be tested BEFORE the in-range test, because the arrays sit back to back:
    # slotbytes' fold base 0x8243FC2F lands inside the position array, and
    # letting the range test win there labels twenty real slotbytes sites as
    # position sites, which silently leaves them unrelocated. The fold reading
    # only wins when the containing array cannot legally have an element at that
    # address; when both readings are legal the site is reported as ambiguous
    # rather than guessed at.
    for name, base, stride, count, end in ARRAYS:
        if name in NO_FOLD or target != base - stride:
            continue
        holder = next((n for n, b, s, c, e in ARRAYS
                       if b <= target < e and n != name), "")
        if holder and is_element_addr(holder, target):
            return holder, "ambiguous_fold"
        return name, "below_base"

    # Exact-end / shared-boundary addresses next: 0x8243FC30 is both the
    # slotbytes base and the position array's loop-end sentinel.
    for name, base, stride, count, end in ARRAYS:
        if base <= target < end:
            if target == base:
                # could be the array base, or the previous array's end sentinel
                prev_end = [a for a in ARRAYS if a[4] == target]
                role = "sentinel_or_base" if prev_end else "element"
                return name, role
            return name, "element"
    # a constant exactly one stride below a base is a folded field offset
    for name, base, stride, count, end in ARRAYS:
        if name in NO_FOLD:
            continue
        if base - stride <= target < base:
            return name, "below_base"
    # exact end-of-array addresses used purely as bounds
    for name, base, stride, count, end in ARRAYS:
        if target == end and name not in NO_END_SENTINEL:
            return name, "sentinel_or_base"
    return "", ""


def disambiguate(code: bytes, seg_start: int, site: Site, window: int = 24) -> str:
    """Decide whether a site landing on an array boundary is that array's base
    or the previous array's end-of-loop sentinel.

    The two read identically as constants, but they are used differently: a
    sentinel is *compared* against a walking pointer, while a base is *indexed*
    (used as the rA of a memory op, or added to an index). Walk forward through
    the straight-line code and take whichever use comes first.
    """
    if site.form != "addi":
        # a load/store that touches the address is dereferencing it, so it is
        # being used as storage, not as a bound
        return "element"

    reg = site.reg
    i = (site.addr - seg_start) // 4
    for j in range(i + 1, min(i + 1 + window, len(code) // 4)):
        w = int.from_bytes(code[j * 4:j * 4 + 4], "big")
        if w == 0:
            break
        opcd = w >> 26
        rD = (w >> 21) & 0x1F
        rA = (w >> 16) & 0x1F
        rB = (w >> 11) & 0x1F

        # immediate compares
        if opcd in (10, 11) and rA == reg:
            return "sentinel"
        if opcd in DFORM_MEM and rA == reg:
            return "element"
        # `addi rZ, base, (field - base)` builds a pointer to a field inside the
        # struct. That is indexing, so the constant is a base, not a bound.
        if opcd == 14 and rA == reg:
            return "element"
        if opcd == 31:
            sub = (w >> 1) & 0x3FF
            if sub in (0, 32) and (rA == reg or rB == reg):   # cmp / cmpl
                return "sentinel"
            if sub == 266 and (rA == reg or rB == reg):       # add
                return "element"
            # indexed loads and stores dereference through rA/rB
            if sub in (23, 87, 151, 215, 279, 343, 407, 439) and \
                    (rA == reg or rB == reg):
                return "element"
            # `mr rA, rS` copies the constant into a walking pointer, which is
            # how every counted loop over these arrays starts. Follow the copy
            # rather than giving up: the use that settles base-vs-bound is on
            # the new register. Note rS is the bits 21..25 field here and rA the
            # bits 16..20 one, the opposite way round from addi.
            if sub == OR_X and rD == rB and rD == reg:
                reg = rA
                continue
            # Redefinition ends the register's life, but only if this form
            # actually writes the field we are watching.
            if sub in RA_DEST_X or sub in STORE_UPDATE_X:
                if rA == reg:
                    break
            elif sub in NO_GPR_WRITE_X:
                pass
            elif rD == reg:
                break   # redefined
        elif opcd in (14, 15) and rD == reg:
            break       # redefined
        elif opcd in RA_DEST_DFORM and rA == reg:
            break       # redefined; these write the bits 16..20 field
        elif opcd == 18:
            # A `bl` with the constant sitting in an argument register means the
            # address is being handed to a callee (memcpy and friends), which
            # only makes sense for a base, never for a loop bound.
            if w & 1 and 3 <= reg <= 10:
                return "element"
            break
        # A conditional branch does not end the register's life, so keep going;
        # only an unconditional one leaves the run.

    return "sentinel_or_base"


def scan(code: bytes, seg_start: int) -> list[Site]:
    sites: list[Site] = []
    # reg -> (value, addr where it was set)
    const: dict[int, tuple[int, int]] = {}
    n = len(code) // 4

    for i in range(n):
        ea = seg_start + i * 4
        w = int.from_bytes(code[i * 4:i * 4 + 4], "big")
        if w == 0:
            const.clear()
            continue
        opcd = w >> 26
        rD = (w >> 21) & 0x1F
        rA = (w >> 16) & 0x1F
        simm = signed16(w & 0xFFFF)

        # An unconditional branch ends the run of straight-line code we can
        # trust for constant propagation.
        if opcd in (16, 18):
            const.clear()
            continue

        if opcd == 15:  # addis / lis
            if rA == 0:
                const[rD] = ((simm << 16) & 0xFFFFFFFF, ea)
            elif rA in const:
                const[rD] = ((const[rA][0] + (simm << 16)) & 0xFFFFFFFF, ea)
            else:
                const.pop(rD, None)
            continue

        if opcd == 14:  # addi / li
            if rA == 0:
                const[rD] = (simm & 0xFFFFFFFF, ea)
                continue
            if rA in const:
                base_value, setup = const[rA]
                target = (base_value + simm) & 0xFFFFFFFF
                const[rD] = (target, ea)
                if in_blocks(target):
                    array, role = classify(target)
                    if array:
                        sites.append(Site(
                            addr=ea, array=array, target=target, form="addi",
                            reg=rD, dest_reg=rD, base_reg=rA, base_value=base_value,
                            disp=simm, setup_addr=setup,
                            setup_distance=(ea - setup) // 4, role=role))
            else:
                const.pop(rD, None)
            continue

        if opcd == 24:  # ori (used for the lis/ori constant form)
            if rD == rA == 0:   # nop
                continue
            # `ori rA, rS, UIMM`: the source is the bits 21..25 field and the
            # destination is bits 16..20, the opposite way round from addi. The
            # common `ori rX, rX, imm` hides the difference; `ori r9, r11, imm`
            # does not.
            if rD in const:
                const[rA] = ((const[rD][0] | (w & 0xFFFF)), ea)
            else:
                const.pop(rA, None)
            continue

        if opcd in DFORM_MEM:
            mnem, writes = DFORM_MEM[opcd]
            if rA in const:
                base_value, setup = const[rA]
                target = (base_value + simm) & 0xFFFFFFFF
                if in_blocks(target):
                    array, role = classify(target)
                    if array:
                        sites.append(Site(
                            addr=ea, array=array, target=target, form=mnem,
                            reg=rA, dest_reg=rD, base_reg=rA, base_value=base_value,
                            disp=simm, setup_addr=setup,
                            setup_distance=(ea - setup) // 4, role=role))
            # loads clobber their destination GPR
            if writes and opcd not in STORE_OPCODES:
                const.pop(rD, None)
            continue

        # Anything else: invalidate whatever the instruction actually writes.
        # Over-invalidating only costs us propagation, while under-invalidating
        # invents addresses that are not there -- see RA_DEST_DFORM above for
        # what that cost the first version of this scan.
        if opcd == 31:
            sub = (w >> 1) & 0x3FF
            rB = (w >> 11) & 0x1F

            # `mr rA, rS` is a copy, so the tracked constant moves with it.
            if sub == OR_X and rD == rB:
                if rD in const:
                    const[rA] = const[rD]
                else:
                    const.pop(rA, None)
                continue

            if sub in NO_GPR_WRITE_X:
                continue
            if sub in RA_DEST_X or sub in STORE_UPDATE_X:
                const.pop(rA, None)
            elif sub in LOAD_UPDATE_X:
                const.pop(rD, None)
                const.pop(rA, None)
            else:
                const.pop(rD, None)
            continue

        if opcd in RA_DEST_DFORM:
            const.pop(rA, None)
        elif opcd not in STORE_OPCODES and opcd not in NO_GPR_WRITE:
            const.pop(rD, None)

    return sites


def has_incoming_branch(addr: int) -> bool:
    """True if any instruction other than the immediately preceding one branches
    to `addr`, i.e. the predecessor is not the only path in."""
    import ida_xref  # noqa: E402
    import idc  # noqa: E402

    frm = ida_xref.get_first_cref_to(addr)
    while frm not in (0, idc.BADADDR):
        if frm != addr - 4:
            return True
        frm = ida_xref.get_next_cref_to(addr, frm)
    return False


def collect_from_drefs(seg_bytes: dict, ranges) -> list[Site]:
    """Enumerate sites from IDA's own data xrefs rather than from our constant
    propagation.

    Our linear scan only tracks constants built by a nearby `lis`, and the game
    also forms these addresses relative to *other* anchor globals held in
    long-lived registers, e.g.

        addi r11, r24, (dword_8243FC08 - 0x8243F3E8)

    where r24 was loaded blocks earlier. Propagating that correctly needs a
    cross-block dataflow fixpoint, but IDA has already done the work: it
    resolved that instruction to a data xref against 0x8243FC08. So we take
    IDA's xrefs as the site list and use our decoder only to read back the
    instruction form and register numbers that hook generation needs.
    """
    import ida_xref  # noqa: E402

    sites: list[Site] = []
    targets = sorted({t for lo, hi in ranges for t in range(lo, hi)})
    for target in targets:
        array, role = classify(target)
        if not array:
            continue
        frm = ida_xref.get_first_dref_to(target)
        while frm != 0xFFFFFFFFFFFFFFFF and frm != -1:
            site = decode_at(seg_bytes, frm, target, array, role)
            if site is not None:
                sites.append(site)
            frm = ida_xref.get_next_dref_to(target, frm)
    return sites


def decode_at(seg_bytes: dict, ea: int, target: int, array: str,
              role: str) -> Site | None:
    """Read the instruction at `ea` and describe how it reaches `target`."""
    for (start, end), data in seg_bytes.items():
        if start <= ea < end:
            w = int.from_bytes(data[ea - start:ea - start + 4], "big")
            break
    else:
        return None

    opcd = w >> 26
    rD = (w >> 21) & 0x1F
    rA = (w >> 16) & 0x1F
    simm = signed16(w & 0xFFFF)

    if opcd == 14:
        form, reg = "addi", rD
    elif opcd in DFORM_MEM:
        form, reg = DFORM_MEM[opcd][0], rA
    elif opcd == 15:
        form, reg = "lis", rD
    else:
        form, reg = f"op{opcd}", rD

    return Site(addr=ea, array=array, target=target, form=form, reg=reg,
                dest_reg=rD, base_reg=rA, base_value=0, disp=simm,
                setup_addr=0, setup_distance=0, role=role,
                index_reg=((w >> 11) & 0x1F) if opcd == 31 else -1)


def copy_aliases(seg_bytes: dict, start_addr: int, end_addr: int,
                 reg: int) -> set[int]:
    """The set of registers that `reg` at `end_addr` may have been copied from,
    walking backwards to `start_addr` through `mr` instructions.

    The game builds a relocated address once in a loop preheader and then hands
    it to a walking pointer with `mr`, so the instruction that dereferences it
    names a different register than the `addi` that built it:

        addi r11, r11, word_824400C8@l   <- the site that gets the hook
        mr   r9, r11
    loop:
        sth  r8, 0(r9)                   <- reads through the copy
        addi r9, r9, 2                   <- and advances it

    Without following the copy, those last two read as separate sites and each
    would have the relocation delta applied on top of a register that already
    carries it -- the `addi` once per iteration.
    """
    aliases = {reg}
    for ea in range(end_addr - 4, start_addr - 4, -4):
        w = 0
        for (seg_start, seg_end), data in seg_bytes.items():
            if seg_start <= ea < seg_end:
                off = ea - seg_start
                w = int.from_bytes(data[off:off + 4], "big")
                break
        if w == 0 or (w >> 26) != 31:
            continue
        sub = (w >> 1) & 0x3FF
        if sub != OR_X:
            continue
        rS = (w >> 21) & 0x1F
        rA = (w >> 16) & 0x1F
        rB = (w >> 11) & 0x1F
        if rS == rB and rA in aliases:   # mr rA, rS
            aliases.add(rS)
    return aliases


def word_at(seg_bytes: dict, ea: int) -> int:
    for (seg_start, seg_end), data in seg_bytes.items():
        if seg_start <= ea < seg_end:
            off = ea - seg_start
            return int.from_bytes(data[off:off + 4], "big")
    return 0


def dest_regs(w: int) -> set[int]:
    """The GPRs an instruction writes, by the same rA-vs-rD rules the constant
    propagation uses. Used to prove a register still holds what an earlier
    instruction put there."""
    opcd = w >> 26
    rD = (w >> 21) & 0x1F
    rA = (w >> 16) & 0x1F

    if opcd in NO_GPR_WRITE:
        return set()
    if opcd == 31:
        sub = (w >> 1) & 0x3FF
        if sub in NO_GPR_WRITE_X:
            return set()
        if sub in RA_DEST_X or sub in STORE_UPDATE_X:
            return {rA}
        if sub in LOAD_UPDATE_X:
            return {rD, rA}
        return {rD}
    if opcd in RA_DEST_DFORM:
        return {rA}
    if opcd in STORE_OPCODES:
        # the update forms write their base register
        return {rA} if opcd in (37, 39, 45, 49, 51, 53, 55) else set()
    if opcd in DFORM_MEM:
        _, writes = DFORM_MEM[opcd]
        out = {rD} if writes else set()
        if opcd in (33, 35, 41, 43):    # update forms
            out.add(rA)
        return out
    # A call clobbers the volatile registers; anything else, assume rD.
    if opcd == 18 and (w & 1):
        return set(range(0, 13))
    return {rD}


def clobbered_between(seg_bytes: dict, start: int, end: int, reg: int) -> bool:
    """True if anything between `start` (exclusive) and `end` (exclusive) writes
    `reg`.

    Without this, "the base register was built by an earlier site" is only a
    guess: the register may have been rebuilt from a fresh `lis` in between, in
    which case the later site materialises its own address and dropping its hook
    would leave that access pointing at the old array.
    """
    for ea in range(start + 4, end, 4):
        w = word_at(seg_bytes, ea)
        if w == 0:
            continue
        if reg in dest_regs(w):
            return True
    return False


def mark_redundant(sites: list[Site], seg_bytes: dict, window: int = 128) -> int:
    """Flag sites that merely dereference an address a nearby `addi` site
    already built.

    Two shapes. The indexed one, where IDA annotates both halves of

        addi r11, r11, (byte_8243FC2F - 0x8243F3E8)
        stbx r10, r3, r11

    as references to byte_8243FC2F, so they arrive as two sites. And the
    displaced one, where a walking pointer is built once and then read at
    several field offsets:

        addi r31, r11, (unk_82016188 - 0x82016120)
        ...
        lfs  f0, -0x14(r31)      ; resolves to 0x82016174

    Only the addi materialises the constant. Hooking the dependent site too
    would add the relocation delta twice -- and note that the second shape does
    *not* resolve to the same address as the addi, so matching on the target
    would miss it entirely.

    The window has to reach past a loop preheader, and sometimes past a whole
    loop body: in sub_821E7138 the walking template pointer is built 17
    instructions before its first use, but in sub_821E7668 the charwords base
    goes into r30 in the prologue and is not indexed until 86 instructions
    later, on the other side of two loops. Matches are confined to the site's
    own function, since no register survives a call boundary. There is no clobber check, so every match this produces is worth
    reading once; the diff against party_sites.reference.json is what makes that
    tractable.
    """
    by_addr = {s.addr: s for s in sites}
    marked = 0
    for s in sites:
        indexed = s.form == "op31"
        regs = {s.base_reg, s.index_reg} if indexed else {s.base_reg}
        regs |= copy_aliases(seg_bytes, s.addr - 4 * window, s.addr, s.base_reg)
        for back in range(1, window + 1):
            prev = by_addr.get(s.addr - 4 * back)
            if prev is None or prev.form != "addi" or prev.reg not in regs:
                continue
            # A dependent site must belong to the same array as the address it
            # is reading through. If it does not, the register is being shared
            # between two arrays and that needs a human, not a silent skip.
            if prev.array != s.array:
                continue
            # The window is wide enough now to reach out of the function the
            # site lives in, which would pair a use with a base built in a
            # different function entirely. A register does not survive that.
            if prev.func != s.func:
                continue
            # For the indexed shape keep the original, stricter test: those two
            # instructions name the same address, and loosening it there would
            # start swallowing genuinely separate sites.
            if indexed and prev.target != s.target:
                continue
            # And the register really has to still hold what `prev` put in it.
            # This only applies to a direct match: when the value arrived
            # through a `mr`, the register `prev` wrote is free to be reused
            # after the copy, so the check would reject a real match. Those are
            # rare and are called out in the note instead.
            via_copy = s.base_reg != prev.reg
            if not via_copy and \
                    clobbered_between(seg_bytes, prev.addr, s.addr, s.base_reg):
                continue
            s.redundant = True
            s.note = (s.note + "; " if s.note else "") + \
                f"dereferences r{prev.reg} built at {prev.addr:#010x}" + \
                (f" via a copy into r{s.base_reg}" if via_copy else "")
            marked += 1
            break
    return marked


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--idb", required=True, help="path to the .i64 database")
    ap.add_argument("--json", default="", help="write the site table here")
    ap.add_argument("--source", choices=("drefs", "scan", "both"),
                    default="both",
                    help="drefs (default, complete) uses IDA's xrefs; scan uses "
                         "our own constant propagation and is known to miss "
                         "cross-block anchors; both cross-checks the two")
    args = ap.parse_args()

    import idapro  # noqa: E402

    if idapro.open_database(args.idb, run_auto_analysis=False) != 0:
        print(f"failed to open {args.idb}", file=sys.stderr)
        return 1

    import ida_segment  # noqa: E402
    import ida_bytes  # noqa: E402
    import ida_funcs  # noqa: E402

    try:
        seg_bytes: dict[tuple[int, int], bytes] = {}
        for i in range(ida_segment.get_segm_qty()):
            seg = ida_segment.getnseg(i)
            if seg is None or seg.perm & ida_segment.SEGPERM_EXEC == 0:
                continue
            size = seg.end_ea - seg.start_ea
            data = ida_bytes.get_bytes(seg.start_ea, size)
            if not data:
                continue
            name = ida_segment.get_segm_name(seg)
            print(f"executable segment {name} {seg.start_ea:#x}..{seg.end_ea:#x} "
                  f"({size} bytes)", file=sys.stderr)
            seg_bytes[(seg.start_ea, seg.end_ea)] = data

        scan_sites: list[Site] = []
        for (start, end), data in seg_bytes.items():
            found = scan(data, start)
            for s in found:
                if s.role == "sentinel_or_base":
                    s.role = disambiguate(data, start, s)
            scan_sites.extend(found)

        dref_sites = collect_from_drefs(seg_bytes, BLOCKS)
        for s in dref_sites:
            if s.role == "sentinel_or_base":
                for (start, end), data in seg_bytes.items():
                    if start <= s.addr < end:
                        s.role = disambiguate(data, start, s)
                        break

        # Neither source alone is complete. IDA resolves addresses that appear
        # symbolically in an operand, including ones anchored on a different
        # global in a long-lived register, which our propagation loses across
        # blocks. Our scan resolves addresses that are *computed* into a
        # register and then dereferenced, where there is no operand for IDA to
        # annotate. The union is the real site list.
        #
        # One filter is needed on the scan side: a bare `lis rX, 0x8244` yields
        # 0x82440000, which lands inside the stats_base range but is only a high
        # half, not a reference. Drop lis-form sites.
        if args.source == "scan":
            all_sites = [s for s in scan_sites if s.form != "lis"]
        elif args.source == "drefs":
            all_sites = dref_sites
        else:
            seen = {(s.addr, s.target) for s in dref_sites}
            all_sites = list(dref_sites)
            for s in scan_sites:
                if s.form == "lis":
                    continue
                if (s.addr, s.target) not in seen:
                    all_sites.append(s)

        scan_addrs = {(s.addr, s.target) for s in scan_sites}
        dref_addrs = {(s.addr, s.target) for s in dref_sites}
        only_dref = dref_addrs - scan_addrs
        only_scan = scan_addrs - dref_addrs
        print(f"\nsite sources: {len(dref_addrs)} from IDA xrefs, "
              f"{len(scan_addrs)} from constant propagation", file=sys.stderr)
        print(f"  {len(only_dref)} found only by IDA (cross-block anchors), "
              f"{len(only_scan)} found only by our scan", file=sys.stderr)
        if only_scan:
            for a, t in sorted(only_scan)[:20]:
                print(f"    scan-only {a:#010x} -> {t:#010x}", file=sys.stderr)

        for s in all_sites:
            f = ida_funcs.get_func(s.addr)
            if f is None:
                s.func = ""
                s.note = "not inside any IDA-defined function"
            else:
                s.func = f"sub_{f.start_ea:08X}"

            # Can a hook safely be placed on the instruction *before* this one?
            # Only if control cannot arrive here any other way. A load or store
            # at the head of a loop is branched to on every iteration but the
            # first, so a shift placed on its predecessor would apply once and
            # the other eleven passes would read the old address.
            s.branch_target = has_incoming_branch(s.addr)
    finally:
        idapro.close_database(save=False)

    all_sites.sort(key=lambda s: s.addr)

    applied = 0
    for s in all_sites:
        want = ROLE_OVERRIDES.get(s.addr)
        if want is None or s.role == want:
            continue
        s.note = (s.note + "; " if s.note else "") + \
            f"role {s.role} -> {want} by hand (see ROLE_OVERRIDES)"
        s.role = want
        applied += 1
    stale = sorted(set(ROLE_OVERRIDES) - {s.addr for s in all_sites})
    if stale:
        print("WARNING: ROLE_OVERRIDES entries match no site: "
              + ", ".join(f"{a:#010x}" for a in stale), file=sys.stderr)
    print(f"{applied} sites reclassified by hand", file=sys.stderr)

    redundant = mark_redundant(all_sites, seg_bytes)
    print(f"{redundant} indexed sites marked redundant (they dereference an "
          f"address an earlier site builds)", file=sys.stderr)
    report(all_sites)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump([asdict(s) for s in all_sites], fh, indent=1)
        print(f"\nwrote {len(all_sites)} sites to {args.json}", file=sys.stderr)
    return 0


def report(sites: list[Site]) -> None:
    print(f"\n{len(sites)} sites total\n")

    by_array: dict[str, list[Site]] = {}
    for s in sites:
        by_array.setdefault(s.array, []).append(s)

    print("by array and access form:")
    for name, _, _, _, _ in ARRAYS:
        group = by_array.get(name, [])
        forms: dict[str, int] = {}
        for s in group:
            forms[s.form] = forms.get(s.form, 0) + 1
        forms_txt = ", ".join(f"{k}={v}" for k, v in sorted(forms.items()))
        print(f"  {name:11s} {len(group):4d}  {forms_txt}")

    print("\nby role:")
    roles: dict[str, int] = {}
    for s in sites:
        roles[s.role] = roles.get(s.role, 0) + 1
    for k, v in sorted(roles.items()):
        print(f"  {k:18s} {v}")

    print(f"\n{sum(1 for s in sites if s.redundant)} sites are redundant "
          f"(indexed use of an address another site builds)")

    ambig = [s for s in sites if s.role == "ambiguous_fold"]
    if ambig:
        print(f"\n{len(ambig)} sites are a legal element of one array AND the "
              f"folded base of the next; resolve these by hand:")
        for s in ambig:
            print(f"  {s.addr:#010x} {s.form:6s} -> {s.target:#010x} {s.func}")

    funcs = {s.func for s in sites}
    print(f"\n{len(funcs)} distinct functions touched")

    orphans = [s for s in sites if not s.func]
    if orphans:
        print(f"\n{len(orphans)} sites outside any IDA-defined function:")
        for s in orphans:
            print(f"  {s.addr:#010x} {s.form:6s} -> {s.target:#010x} ({s.array})")

    far = [s for s in sites if s.setup_distance > 8]
    if far:
        print(f"\n{len(far)} sites whose base constant was set >8 instructions "
              f"earlier (review these, propagation is less certain):")
        for s in far[:20]:
            print(f"  {s.addr:#010x} {s.form:6s} -> {s.target:#010x} "
                  f"(set at {s.setup_addr:#010x}, {s.setup_distance} insns)")

    shared = {}
    for s in sites:
        shared.setdefault(s.setup_addr, set()).add(s.array)
    multi = {k: v for k, v in shared.items() if len(v) > 1}
    if multi:
        print(f"\n{len(multi)} `lis` sites feed more than one array (these "
              f"cannot be fixed by rewriting the shared register):")
        for k, v in sorted(multi.items()):
            print(f"  {k:#010x} -> {', '.join(sorted(v))}")

    ambiguous = [s for s in sites if s.role != "element"]
    if ambiguous:
        print(f"\n{len(ambiguous)} sites need manual classification "
              f"(bounds/sentinels/folded field constants):")
        for s in ambiguous:
            print(f"  {s.addr:#010x} {s.form:6s} -> {s.target:#010x} "
                  f"{s.array:11s} {s.role:18s} {s.func}")


if __name__ == "__main__":
    sys.exit(main())
