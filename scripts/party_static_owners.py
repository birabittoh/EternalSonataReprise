#!/usr/bin/env python3
"""Find which party array each instruction of config/party.toml's functions reads.

Entries 10 and 11 of one party array sit at the retail address of the next one
(stats_live[10] is stats_base[0]), so party_arrays.cpp cannot tell them apart by
address. The code can: every access derives its address from the array's base
constant. This propagates register constants (lis/addi/ori, adds of an index,
stack spills) forward over each function's flow graph and names the array each
load or store's base points into.

A base inside an array's last retail entry is the next array's 1 based access
(base - stride + field), never a pointer to entry 9 itself.

Needs IDA's idalib and the PAL database; works on a copy of it.

  python scripts/party_static_owners.py [--idb PATH] [--out src/engine/party_static_owners.inc]
"""
import argparse
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Same order as party_arrays.cpp's PartyArray.
ARRAYS = [
    ("position", 0x8243FC08, 4),
    ("slotbytes", 0x8243FC30, 1),
    ("charflags", 0x8243FCFC, 1),
    ("stats_live", 0x8243FD08, 48),
    ("stats_base", 0x8243FEE8, 48),
    ("charwords", 0x824400C8, 2),
    ("template", 0x82016150, 136),
    ("portrait_a", 0x8202CA28, 2),
    ("portrait_b", 0x8202CA3C, 2),
]

DLOAD = {32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51}
DSTORE = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55}
DUPD = {33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55}
DSIZE = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2, 47: 128, 52: 4, 53: 4, 54: 8, 55: 8, 62: 8}
XLOAD = {23, 55, 87, 119, 279, 311, 343, 375, 535, 567, 599, 631, 534, 790, 21, 53, 341, 373,
         103, 359, 519, 551, 7, 39, 71}
XSTORE = {151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 662, 918, 149, 181, 231, 487,
          647, 679, 135, 167, 199, 983}
XUPD = {55, 119, 311, 375, 567, 631, 53, 373, 183, 247, 439, 695, 759, 181}
XVECTOR_OR_FLOAT = {103, 359, 519, 551, 7, 39, 71, 535, 567, 599, 631}
# cmp, tw, cache ops, sync, mtspr, mtcrf, mtmsr: no GPR written.
NOWRITE31 = {0, 32, 4, 68, 86, 54, 278, 246, 1014, 598, 854, 470, 467, 144, 146, 178}
STACK = 1000  # register file keys from here on are stack slots, by offset
# config/crt.toml's natives: (destination, source) argument registers. The
# remap sees their two sides keyed by return address | 2 and | 3.
CRT = {0x822CF5B0: (3, 4), 0x822CE550: (3, 4), 0x822CFB40: (3, None),
       0x822D5748: (3, 5), 0x822530B8: (3, 4)}


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def near(v):
    return any(base - stride <= v < base + 12 * stride for _, base, stride in ARRAYS)


def owner_of(hint):
    """The array a base pointer belongs to, or None."""
    for i, (_, base, stride) in enumerate(ARRAYS):
        if base <= hint < base + 10 * stride:
            if (hint - base) // stride == 9:
                for j, (_, base2, _) in enumerate(ARRAYS):
                    if base2 == base + 10 * stride:
                        return j
            return i
    for i, (_, base, stride) in enumerate(ARRAYS):
        if base - stride <= hint < base:
            return i
    return None


def meet(a, b):
    if a == b:
        return a
    if a is None or b is None:
        return None
    oa, ob = owner_of(a[1]), owner_of(b[1])
    if oa is not None and oa == ob:
        return ("B", min(a[1], b[1]))
    return None


def step(w, ea, regs, out):
    """Applies one instruction to regs; records party accesses in out."""
    op = w >> 26
    rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    simm = sx(w & 0xFFFF)

    def kill(*rs):
        for r in rs:
            regs.pop(r, None)

    def kill_stack(lo=None, hi=None):
        for k in [k for k in regs if k >= STACK]:
            if lo is None or lo < k - STACK + 4 and k - STACK < hi:
                kill(k)

    def access(h):
        if out is not None and h is not None and h[0] == "B" and near(h[1]):
            out.append((ea, h[1]))

    if op in (14, 15):  # addi, addis
        imm = simm if op == 14 else simm << 16
        v = ("C", 0) if ra == 0 else regs.get(ra)
        if v:
            regs[rd] = (v[0], (v[1] + imm) & 0xFFFFFFFF)
        else:
            kill(rd)
        return
    if op == 24:  # ori
        v = regs.get(rd)
        if v and v[0] == "C":
            regs[ra] = ("C", v[1] | (w & 0xFFFF))
        elif v and not w & 0xFFFF:
            regs[ra] = v
        else:
            kill(ra)
        return
    if op in DLOAD or op in DSTORE or op in (58, 62):
        d = simm & ~3 if op in (58, 62) else simm
        store = op in DSTORE or op == 62
        if ra == 1:
            if op == 32:
                v = regs.get(STACK + d)
                if v:
                    regs[rd] = v
                else:
                    kill(rd)
            elif op == 36:
                kill_stack(d, d + 4)
                if regs.get(rd):
                    regs[STACK + d] = regs[rd]
            elif op == 37:  # stwu r1 opens a new frame
                kill_stack()
            elif store:
                kill_stack(d, d + DSIZE.get(op, 8))
            elif op == 46:
                kill(*range(rd, 32))
            elif op in DLOAD and op < 48 or op == 58:
                kill(rd)
            return
        v = ("C", 0) if ra == 0 else regs.get(ra)
        h = (v[0], (v[1] + d) & 0xFFFFFFFF) if v else None
        access(h)
        if store and not (v and near(v[1])):
            kill_stack()  # may be a pointer into the frame
        if op == 46:
            kill(*range(rd, 32))
        elif op in DLOAD and op < 48 or op == 58:
            kill(rd)
        if op in DUPD or (op in (58, 62) and (w & 3) == 1):
            if h:
                regs[ra] = h
            else:
                kill(ra)
        return
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in XLOAD or xo in XSTORE:
            va = ("C", 0) if ra == 0 else regs.get(ra)
            vb = regs.get(rb)
            h = None
            if va and vb and va[0] == "C" and vb[0] == "C":
                h = ("C", (va[1] + vb[1]) & 0xFFFFFFFF)
            elif va and near(va[1]):
                h = ("B", va[1])
            elif vb and near(vb[1]):
                h = ("B", vb[1])
            access(h)
            if xo in XSTORE and not h:
                kill_stack()
            if xo in XLOAD and xo not in XVECTOR_OR_FLOAT:
                kill(rd)
            if xo in XUPD:
                if h:
                    regs[ra] = h
                else:
                    kill(ra)
            return
        if xo == 266:  # add
            va, vb = regs.get(ra), regs.get(rb)
            if va and vb and va[0] == "C" and vb[0] == "C":
                regs[rd] = ("C", (va[1] + vb[1]) & 0xFFFFFFFF)
            elif va and near(va[1]) and not (vb and near(vb[1])):
                regs[rd] = ("B", va[1])
            elif vb and near(vb[1]) and not (va and near(va[1])):
                regs[rd] = ("B", vb[1])
            else:
                kill(rd)
            return
        if xo == 444 and rd == rb:  # mr
            if regs.get(rd):
                regs[ra] = regs[rd]
            else:
                kill(ra)
            return
        if xo in NOWRITE31:
            return
        kill(rd, ra)  # arithmetic writes rD, logical and shifts write rA
        return
    if op in (2, 3, 10, 11, 16, 17, 4, 59, 63):
        return
    if op == 18:
        if w & 1:
            target = (ea + (((w & 0x3FFFFFC) ^ 0x2000000) - 0x2000000)) & 0xFFFFFFFF
            if out is not None and target in CRT:
                for side, r in enumerate(CRT[target]):
                    v = regs.get(r) if r is not None else None
                    if v and near(v[1]):
                        out.append(((ea + 4) | 2 | side, v[1]))
            kill(0, *range(3, 13))
        return
    if op == 19:
        if (w >> 1) & 0x3FF in (16, 528) and w & 1:
            kill(0, *range(3, 13))
        return
    kill(rd, ra)


def analyze(fn, ida_gdl, get_dword):
    blocks = list(ida_gdl.FlowChart(fn))
    ins = {b.start_ea: None for b in blocks}
    ins[fn.start_ea] = {}
    succ = {b.start_ea: [s.start_ea for s in b.succs()] for b in blocks}
    for _ in range(64):
        changed = False
        for b in blocks:
            if ins[b.start_ea] is None:
                continue
            regs = dict(ins[b.start_ea])
            for ea in range(b.start_ea, b.end_ea, 4):
                step(get_dword(ea), ea, regs, None)
            for s in succ[b.start_ea]:
                if s not in ins:
                    continue
                if ins[s] is None:
                    ins[s] = dict(regs)
                    changed = True
                    continue
                new = {}
                for r in set(ins[s]) & set(regs):
                    m = meet(ins[s][r], regs[r])
                    if m:
                        new[r] = m
                if new != ins[s]:
                    ins[s] = new
                    changed = True
        if not changed:
            break
    owners = {}
    for b in blocks:
        if ins[b.start_ea] is None:
            continue
        regs = dict(ins[b.start_ea])
        out = []
        for ea in range(b.start_ea, b.end_ea, 4):
            step(get_dword(ea), ea, regs, out)
        for ea, hint in out:
            o = owner_of(hint)
            if o is not None:
                owners[ea] = o
    return owners


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--idb", default=os.path.join(ROOT, "..", "EternalSonataReprise-Assets",
                                                  "default.xex.i64"))
    ap.add_argument("--out", default=os.path.join(ROOT, "src", "engine",
                                                  "party_static_owners.inc"))
    args = ap.parse_args()

    toml = open(os.path.join(ROOT, "config", "party.toml")).read()
    block = toml[toml.index("functions = ["):]
    funcs = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{8}", block[:block.index("]")])]

    tmp = tempfile.mkdtemp()
    idb = os.path.join(tmp, os.path.basename(args.idb))
    shutil.copy(args.idb, idb)
    import idapro
    idapro.open_database(idb, True)
    import ida_bytes
    import ida_funcs
    import ida_gdl

    owners = {}
    skipped = []
    for f in funcs:
        fn = ida_funcs.get_func(f)
        if not fn or fn.start_ea != f:
            skipped.append(f)
            continue
        owners.update(analyze(fn, ida_gdl, ida_bytes.get_dword))
    idapro.close_database(False)
    shutil.rmtree(tmp, ignore_errors=True)

    with open(args.out, "w", newline="\n") as f:
        f.write("// Generated by scripts/party_static_owners.py: instruction, or a CRT\n"
                "// call's return address | 2 (destination) or | 3 (source); PartyArray.\n")
        for ea in sorted(owners):
            f.write(f"{{0x{ea:08X}u, {owners[ea]}}},  // {ARRAYS[owners[ea]][0]}\n")
    print(f"{len(owners)} instructions in {len(funcs) - len(skipped)} functions", file=sys.stderr)
    if skipped:
        print("not IDA functions: " + " ".join(f"{s:08X}" for s in skipped), file=sys.stderr)


if __name__ == "__main__":
    main()
