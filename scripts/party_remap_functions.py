#!/usr/bin/env python3
"""List the functions that may touch a party array, for config/party.toml.

Run it while generated/ still has REX_PC markers (all_functions = true), or
against any generated tree that was built with them. It prints the union of:
  * a constant tracking scan over each function's PPC comments,
  * the PCs the handlers special case (PC literals in the engine sources),
  * every midasm_hook address in config/party.toml and config/shop.toml,
  * functions named by 'touches a party array' lines in logs/*.log.

  python scripts/party_remap_functions.py            # TOML list on stdout
  python scripts/party_remap_functions.py --why      # reasons on stderr too
"""
import argparse
import bisect
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RANGES = [
    (0x8243FC08, 0x8243FC3C),
    (0x8243FCFC, 0x8243FD08),
    (0x8243FD08, 0x82440128),
    (0x82016150, 0x820167B0),
    (0x8202CA28, 0x8202CA54),
    (0x82560114, 0x825601A2),
]
SLACK = 0x400

# Sources whose 0x82xxxxxx literals are PCs the remap handlers depend on.
PC_SOURCES = [
    "src/engine/party_arrays.cpp",
    "src/engine/ps3_item_tables.cpp",
    "src/engine/shop_stock.cpp",
    "src/engine/party_positions.cpp",
    "src/engine/party_bounds.cpp",
    "src/engine/character_roster.cpp",
]
HOOK_TOMLS = ["config/party.toml", "config/shop.toml"]

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
CMT_RE = re.compile(r"^\t// (\S+)\s*(.*)$")
LABEL_RE = re.compile(r"^loc_[0-9A-F]{8}:")

LOADS = ("lwz", "lhz", "lbz", "lha", "lfs", "lfd", "ld", "lwa")
STORES = ("stw", "sth", "stb", "stfs", "stfd", "std", "stfiwx")


# Float constants sitting just below the template that most of the game reads.
BENIGN = {0x82016120, 0x82016124, 0x82016128}


def near(v):
    v &= 0xFFFFFFFF
    if v in BENIGN:
        return False
    return any(lo - SLACK <= v < hi + SLACK for lo, hi in RANGES)


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def num(s):
    return int(s, 0)


def scan_function(lines):
    """Return the set of in-range constants derived in one function body."""
    regs = {}
    hits = set()

    def note(v):
        v &= 0xFFFFFFFF
        if near(v):
            hits.add(v)

    for ln in lines:
        if LABEL_RE.match(ln):
            regs.clear()
            continue
        m = CMT_RE.match(ln)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)
        args = [a.strip() for a in rest.split(",")] if rest else []
        base_op = op.rstrip(".")

        def gpr(a):
            mm = re.fullmatch(r"r(\d+)", a)
            return int(mm.group(1)) if mm else None

        if base_op == "bl" or base_op == "bctrl" or base_op == "blrl":
            for r in range(3, 13):
                regs.pop(r, None)
            continue
        if base_op.startswith("cmp") or base_op.startswith("tw") or base_op in (
                "mtctr", "mtlr", "mtcr", "mtmsr", "mtfsf", "b", "blr", "bctr"):
            continue
        if base_op.startswith("b") and len(args) >= 1 and base_op not in (
                "bl", "addic", "xor"):
            # conditional branches: no register written
            if base_op in ("beq", "bne", "blt", "bgt", "ble", "bge", "bdnz",
                           "bdz", "bso", "bns", "bnl", "bng"):
                continue

        # memory forms: op rD,DISP(rA)
        mm = re.fullmatch(r"(-?\w+)\((r\d+)\)", args[-1]) if args else None
        if mm and len(args) == 2:
            ra = int(mm.group(2)[1:])
            if ra in regs:
                note(regs[ra] + s32(num(mm.group(1))))
            stem = base_op.rstrip("ux")
            if base_op.startswith(STORES):
                if base_op.endswith("u") and ra:
                    regs.pop(ra, None)
            else:
                rd = gpr(args[0])
                if rd is not None:
                    regs.pop(rd, None)
                if base_op.endswith("u") or base_op.endswith("ux"):
                    regs.pop(ra, None)
            continue

        rd = gpr(args[0]) if args else None
        if rd is None:
            continue
        if base_op == "lis" and len(args) == 2:
            # A bare page base matches unrelated globals; its uses are checked.
            regs[rd] = s32(num(args[1]) << 16) & 0xFFFFFFFF
        elif base_op == "li" and len(args) == 2:
            regs[rd] = s32(num(args[1])) & 0xFFFFFFFF
            note(regs[rd])
        elif base_op in ("addi", "addic") and len(args) == 3:
            ra = gpr(args[1])
            if ra in regs:
                regs[rd] = (regs[ra] + s32(num(args[2]))) & 0xFFFFFFFF
                note(regs[rd])
            else:
                regs.pop(rd, None)
        elif base_op == "addis" and len(args) == 3:
            ra = gpr(args[1])
            if ra in regs:
                regs[rd] = (regs[ra] + (num(args[2]) << 16)) & 0xFFFFFFFF
                note(regs[rd])
            else:
                regs.pop(rd, None)
        elif base_op in ("ori", "oris") and len(args) == 3:
            # ori rA,rS,imm: destination is the first operand
            rs = gpr(args[1])
            sh = 16 if base_op == "oris" else 0
            if rs in regs:
                regs[rd] = (regs[rs] | (num(args[2]) << sh)) & 0xFFFFFFFF
                note(regs[rd])
            else:
                regs.pop(rd, None)
        elif base_op == "mr" and len(args) == 2:
            rs = gpr(args[1])
            if rs in regs:
                regs[rd] = regs[rs]
            else:
                regs.pop(rd, None)
        else:
            regs.pop(rd, None)
    return hits


def parse_generated():
    starts = []  # (start, name_addr)
    body = {}
    pcs = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "generated", "eternalsonata_recomp.*.cpp"))):
        cur = None
        with open(path, "r", errors="replace") as f:
            for ln in f:
                ln = ln.rstrip("\n")
                m = FUNC_RE.match(ln)
                if m:
                    cur = int(m.group(1), 16)
                    starts.append(cur)
                    body[cur] = []
                    pcs[cur] = [cur, cur]
                    continue
                if cur is None:
                    continue
                body[cur].append(ln)
                if ln.startswith("#define REX_PC "):
                    pc = int(ln.split()[2].rstrip("u"), 16)
                    pcs[cur][1] = max(pcs[cur][1], pc)
    starts = sorted(set(starts))
    return starts, body, pcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--why", action="store_true")
    args = ap.parse_args()

    starts, body, pcs = parse_generated()
    if not starts:
        sys.exit("no generated/eternalsonata_recomp.*.cpp found; run scripts/build.py first")
    lo_code = starts[0]

    def func_of(addr):
        if addr < lo_code:
            return None
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i]

    def end_of(fn):
        i = bisect.bisect_right(starts, fn)
        return starts[i] if i < len(starts) else pcs[fn][1] + 4

    why = {}

    def add(fn, reason):
        if fn is not None:
            why.setdefault(fn, set()).add(reason)

    # 3.1 constant scan
    for fn in starts:
        for v in sorted(scan_function(body[fn])):
            add(fn, "scan %08X" % v)

    # 3.2 runtime logs
    for path in glob.glob(os.path.join(ROOT, "logs", "*.log")):
        with open(path, "r", errors="replace") as f:
            for ln in f:
                m = re.search(r"party arrays: (?:__imp__)?(?:sub_)?([0-9A-F]{8}) (?:touches|reaches)", ln)
                if m:
                    a = int(m.group(1), 16)
                    if "reaches" in ln:
                        add(func_of(a), "log pc %08X" % a)
                    else:
                        add(a if a in body else func_of(a), "log")

    # 3.3 handler PCs and hook addresses
    for rel in PC_SOURCES:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            continue
        text = open(p, errors="replace").read()
        # `pc >= A && pc < B` style ranges
        for a, b in re.findall(r"pc >= 0x(82[0-9A-F]{6})u\s*&&\s*pc < 0x(82[0-9A-F]{6})u", text):
            a, b = int(a, 16), int(b, 16)
            i = max(bisect.bisect_right(starts, a) - 1, 0)
            while i < len(starts) and starts[i] < b:
                add(starts[i], "range %08X..%08X in %s" % (a, b, rel))
                i += 1
        for lit in re.findall(r"0x(82[0-9A-F]{6})u", text):
            v = int(lit, 16)
            if lo_code <= v < starts[-1] + 0x10000 and not any(lo <= v < hi for lo, hi in RANGES):
                fn = func_of(v)
                if fn is not None and v < end_of(fn):
                    add(fn, "pc %08X in %s" % (v, rel))
    for rel in HOOK_TOMLS:
        text = open(os.path.join(ROOT, rel)).read()
        for a in re.findall(r"^address\s*=\s*0x([0-9A-Fa-f]+)", text, re.M):
            v = int(a, 16)
            add(func_of(v), "hook %08X in %s" % (v, rel))

    fns = sorted(why)
    if args.why:
        for fn in fns:
            print("%08X  %s" % (fn, "; ".join(sorted(why[fn])[:4])), file=sys.stderr)
    print("functions = [")
    for fn in fns:
        print("  0x%08X," % fn)
    print("]")
    print("%d functions" % len(fns), file=sys.stderr)


if __name__ == "__main__":
    main()
