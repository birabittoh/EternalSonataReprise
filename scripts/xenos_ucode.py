#!/usr/bin/env python3
"""Offline decoder / disassembler for Xenos shader microcode.

Input is a raw `.ucode` blob as produced by `extract_shaders.py`, i.e. the
microcode payload of an Xbox 360 shader container, stored as big endian dwords.

Field layouts follow `../xenia-canary/src/xenia/gpu/ucode.h` (control flow, ALU
and fetch instruction bitfields) and the opcode name tables in `ucode.cc`.
Nothing here executes at runtime; this is the front end of the offline shader
translation step described in the renderer work.

Usage:
    python scripts/xenos_ucode.py out/shaders/vs_020.ucode
    python scripts/xenos_ucode.py --all out/shaders
    python scripts/xenos_ucode.py --all out/shaders --json out/shaders/ucode.json
    python scripts/xenos_ucode.py out/shaders --vertex-inputs
    python scripts/xenos_ucode.py out/shaders --verify-dumps dumps/shaders
"""

import argparse
import json
import os
import struct
import sys

CF_OPCODES = {
    0: "nop", 1: "exec", 2: "exece", 3: "cexec", 4: "cexece",
    5: "cexecp", 6: "cexecpe", 7: "loop", 8: "endloop", 9: "ccall",
    10: "ret", 11: "cjmp", 12: "alloc", 13: "cexecpc", 14: "cexecpce",
    15: "mvfd",
}

CF_EXEC = {1, 2, 3, 4, 5, 6, 13, 14}
CF_EXEC_END = {2, 4, 6, 14}
CF_COND_BOOL = {3, 4, 13, 14}
CF_COND_PRED = {5, 6}

ALLOC_TYPES = {0: "no_alloc", 1: "position", 2: "interpolators/colors", 3: "memory"}

SCALAR_OPS = [
    ("adds", 1), ("adds_prev", 1), ("muls", 1), ("muls_prev", 1),
    ("muls_prev2", 1), ("maxs", 1), ("mins", 1), ("seqs", 1),
    ("sgts", 1), ("sges", 1), ("snes", 1), ("frcs", 1),
    ("truncs", 1), ("floors", 1), ("exp", 1), ("logc", 1),
    ("log", 1), ("rcpc", 1), ("rcpf", 1), ("rcp", 1),
    ("rsqc", 1), ("rsqf", 1), ("rsq", 1), ("maxas", 1),
    ("maxasf", 1), ("subs", 1), ("subs_prev", 1), ("setp_eq", 1),
    ("setp_ne", 1), ("setp_gt", 1), ("setp_ge", 1), ("setp_inv", 1),
    ("setp_pop", 1), ("setp_clr", 0), ("setp_rstr", 1), ("kills_eq", 1),
    ("kills_gt", 1), ("kills_ge", 1), ("kills_ne", 1), ("kills_one", 1),
    ("sqrt", 1), ("opcode_41", 0), ("mulsc", 2), ("mulsc", 2),
    ("addsc", 2), ("addsc", 2), ("subsc", 2), ("subsc", 2),
    ("sin", 1), ("cos", 1), ("retain_prev", 0), ("opcode_51", 0),
    ("opcode_52", 0), ("opcode_53", 0), ("opcode_54", 0), ("opcode_55", 0),
    ("opcode_56", 0), ("opcode_57", 0), ("opcode_58", 0), ("opcode_59", 0),
    ("opcode_60", 0), ("opcode_61", 0), ("opcode_62", 0), ("opcode_63", 0),
]

# Single-operand scalar ops that consume two components of that operand rather
# than one (xenia's `single_operand_is_two_component`).
SCALAR_TWO_COMPONENT = {
    "adds", "muls", "muls_prev2", "maxs", "mins", "maxas", "maxasf", "subs",
}

VECTOR_OPS = [
    ("add", 2), ("mul", 2), ("max", 2), ("min", 2),
    ("seq", 2), ("sgt", 2), ("sge", 2), ("sne", 2),
    ("frc", 1), ("trunc", 1), ("floor", 1), ("mad", 3),
    ("cndeq", 3), ("cndge", 3), ("cndgt", 3), ("dp4", 2),
    ("dp3", 2), ("dp2add", 3), ("cube", 2), ("max4", 1),
    ("setp_eq_push", 2), ("setp_ne_push", 2), ("setp_gt_push", 2),
    ("setp_ge_push", 2), ("kill_eq", 2), ("kill_gt", 2), ("kill_ge", 2),
    ("kill_ne", 2), ("dst", 2), ("maxa", 2), ("opcode_30", 0),
    ("opcode_31", 0),
]

FETCH_OPS = {
    0: "vfetch", 1: "tfetch", 16: "getBCF", 17: "getCompTexLOD",
    18: "getGradients", 19: "getWeights", 24: "setTexLOD",
    25: "setGradientH", 26: "setGradientV",
}

TEX_DIMENSIONS = {0: "1D", 1: "2D", 2: "3D", 3: "Cube"}

COMPONENTS = "xyzw"


def bits(value, start, count):
    return (value >> start) & ((1 << count) - 1)


def sbits(value, start, count):
    v = bits(value, start, count)
    if v & (1 << (count - 1)):
        v -= 1 << count
    return v


def swizzled_component(swizzle, index):
    """ALU source swizzles are component relative (ucode.h)."""
    return ((swizzle >> (2 * index)) + index) & 3


def write_mask_str(mask):
    return "".join(COMPONENTS[i] for i in range(4) if mask & (1 << i)) or "_"


def export_write_mask_str(vector_mask, scalar_mask, scalar_dest_rel):
    """Export destination mask, where the two masks also encode 0 and 1."""
    out = []
    for i in range(4):
        vector = (vector_mask >> i) & 1
        scalar = (scalar_mask >> i) & 1
        if vector and scalar:
            out.append("1")
        elif vector or scalar:
            out.append(COMPONENTS[i])
        elif scalar_dest_rel:
            out.append("0")
        else:
            out.append("_")
    return "".join(out)


class UcodeError(Exception):
    pass


class ControlFlow:
    def __init__(self, dword_0, dword_1):
        self.dword_0 = dword_0
        self.dword_1 = dword_1
        self.opcode = bits(dword_1, 12, 4)
        self.name = CF_OPCODES[self.opcode]

    @property
    def is_exec(self):
        return self.opcode in CF_EXEC

    # exec fields
    @property
    def address(self):
        return bits(self.dword_0, 0, 12)

    @property
    def count(self):
        return bits(self.dword_0, 12, 3)

    @property
    def is_yield(self):
        return bits(self.dword_0, 15, 1) == 1

    @property
    def sequence(self):
        return bits(self.dword_0, 16, 12)

    # Conditional fields. Word 1 is `vc_lo:2, bool_address:8, condition:1,
    # address_mode:1, opcode:4` for every conditional form, so the bool index
    # starts at bit 2 and the condition is bit 10. `cjmp` and `ccall` reuse the
    # low two bits for their own flags.
    @property
    def bool_address(self):
        return bits(self.dword_1, 2, 8)

    @property
    def condition(self):
        return bits(self.dword_1, 10, 1)

    # cjmp / ccall word 0: address:13, is_unconditional:1, is_predicated:1.
    @property
    def jump_address(self):
        return bits(self.dword_0, 0, 13)

    @property
    def is_unconditional(self):
        return bits(self.dword_0, 13, 1) == 1

    @property
    def is_predicated(self):
        return bits(self.dword_0, 14, 1) == 1

    # loop / endloop word 0: address:13, is_repeat:1 (loop) or
    # is_predicated_break:1 at bit 21 (endloop), loop_id:5 at bit 16.
    @property
    def loop_address(self):
        return bits(self.dword_0, 0, 13)

    @property
    def loop_id(self):
        return bits(self.dword_0, 16, 5)

    @property
    def is_repeat(self):
        return bits(self.dword_0, 13, 1) == 1

    @property
    def is_predicated_break(self):
        return bits(self.dword_0, 21, 1) == 1

    def _jump_cond(self):
        """cjmp/ccall condition: unconditional, predicate, or bool constant."""
        if self.is_unconditional:
            return "always"
        if self.is_predicated:
            return "(%sp0)" % ("" if self.condition else "!")
        return "(%sb%d)" % ("" if self.condition else "!", self.bool_address)

    def __str__(self):
        op = self.opcode
        if op == 0:
            return "nop"
        if self.is_exec:
            text = "%s addr(%d) cnt(%d)" % (self.name, self.address, self.count)
            if op in CF_COND_BOOL:
                text += " bool(b%d) cond(%d)" % (self.bool_address,
                                                 self.condition)
            elif op in CF_COND_PRED:
                text += " pred(%d)" % self.condition
            if self.is_yield:
                text += " yield"
            return text
        if op == 12:
            return "alloc %s size(%d)" % (
                ALLOC_TYPES[bits(self.dword_1, 9, 2)], bits(self.dword_0, 0, 3))
        if op == 7:
            return "loop i%d addr(%d)%s" % (self.loop_id, self.loop_address,
                                            " repeat" if self.is_repeat else "")
        if op == 8:
            return "endloop i%d addr(%d)%s" % (
                self.loop_id, self.loop_address,
                " break(p0==%d)" % self.condition
                if self.is_predicated_break else "")
        if op == 9:
            return "ccall addr(%d) %s" % (self.jump_address, self._jump_cond())
        if op == 10:
            return "ret"
        if op == 11:
            return "jmp addr(%d) %s" % (self.jump_address, self._jump_cond())
        return self.name


def decode_alu(w0, w1, w2):
    """AluInstruction, ucode.h."""
    vector_dest = bits(w0, 0, 6)
    vector_dest_rel = bits(w0, 6, 1)
    abs_constants = bits(w0, 7, 1)
    scalar_dest = bits(w0, 8, 6)
    scalar_dest_rel = bits(w0, 14, 1)
    export_data = bits(w0, 15, 1)
    vector_write_mask = bits(w0, 16, 4)
    scalar_write_mask = bits(w0, 20, 4)
    vector_clamp = bits(w0, 24, 1)
    scalar_clamp = bits(w0, 25, 1)
    scalar_opc = bits(w0, 26, 6)

    swiz = {3: bits(w1, 0, 8), 2: bits(w1, 8, 8), 1: bits(w1, 16, 8)}
    negate = {3: bits(w1, 24, 1), 2: bits(w1, 25, 1), 1: bits(w1, 26, 1)}
    pred_condition = bits(w1, 27, 1)
    is_predicated = bits(w1, 28, 1)

    reg = {3: bits(w2, 0, 8), 2: bits(w2, 8, 8), 1: bits(w2, 16, 8)}
    vector_opc = bits(w2, 24, 5)
    sel = {3: bits(w2, 29, 1), 2: bits(w2, 30, 1), 1: bits(w2, 31, 1)}

    vec_name, vec_operands = VECTOR_OPS[vector_opc]
    sca_name, sca_operands = SCALAR_OPS[scalar_opc]

    def operand(i, component_count=4):
        is_temp = sel[i] == 1
        text = ("r%d" if is_temp else "c%d") % (reg[i] & (0x3F if is_temp else 0xFF))
        if component_count == 1:
            comps = COMPONENTS[swizzled_component(swiz[i], 3)]
        elif component_count == -2:
            # The scalar half's two-component form reads swizzle slots 3 then 0.
            comps = "".join(COMPONENTS[swizzled_component(swiz[i], j)]
                            for j in (3, 0))
        else:
            comps = "".join(COMPONENTS[swizzled_component(swiz[i], j)]
                            for j in range(component_count))
        text += "." + comps
        if negate[i]:
            text = "-" + text
        if not is_temp and abs_constants:
            text = "|%s|" % text
        return text

    lines = []
    pred = ""
    if is_predicated:
        pred = "(%sp0) " % ("" if pred_condition else "!")

    if export_data:
        # Exports are written by both halves of the instruction into
        # vector_dest, and the two masks together also encode constant 0 and 1
        # per component (ucode.h, AluInstruction).
        export_mask = export_write_mask_str(vector_write_mask, scalar_write_mask,
                                            scalar_dest_rel)
    if vector_write_mask or export_data:
        dest = ("export%d" if export_data else "r%d") % vector_dest
        srcs = ", ".join(operand(i + 1) for i in range(max(vec_operands, 1)))
        lines.append("%s%s%s %s.%s, %s" % (
            pred, vec_name, "_sat" if vector_clamp else "",
            dest, export_mask if export_data else write_mask_str(vector_write_mask),
            srcs))
    if scalar_write_mask or (scalar_opc != 50 and sca_operands):
        dest = ("export%d" if export_data else "r%d") % (
            vector_dest if export_data else scalar_dest)
        if sca_operands == 2:
            const_reg = reg[3]
            temp_reg = (scalar_opc & 1) | (sel[3] << 1) | (swiz[3] & 0x3C)
            srcs = "c%d.%s, r%d.%s" % (
                const_reg, COMPONENTS[swizzled_component(swiz[3], 3)],
                temp_reg, COMPONENTS[swizzled_component(swiz[3], 0)])
            if negate[3]:
                srcs = "-" + srcs
        elif sca_operands == 1:
            srcs = operand(3, -2 if sca_name in SCALAR_TWO_COMPONENT else 1)
        else:
            srcs = ""
        lines.append("%s%s%s %s.%s%s" % (
            pred, sca_name, "_sat" if scalar_clamp else "",
            dest, export_mask if export_data else write_mask_str(scalar_write_mask),
            (", " + srcs) if srcs else ""))
    if not lines:
        lines.append("%s(nop)" % pred)

    return {
        "type": "alu",
        "vector_opcode": vector_opc,
        "vector_name": vec_name,
        "scalar_opcode": scalar_opc,
        "scalar_name": sca_name,
        "export": bool(export_data),
        "vector_dest": vector_dest,
        "scalar_dest": scalar_dest,
        "vector_write_mask": vector_write_mask,
        "scalar_write_mask": scalar_write_mask,
        "predicated": bool(is_predicated),
        "pred_condition": bool(pred_condition),
        "vector_dest_rel": bool(vector_dest_rel),
        "scalar_dest_rel": bool(scalar_dest_rel),
        "sources": [{"reg": reg[i], "is_temp": bool(sel[i]),
                     "swizzle": swiz[i], "negate": bool(negate[i])}
                    for i in (1, 2, 3)],
        "text": "; ".join(lines),
    }


def decode_fetch(w0, w1, w2):
    opcode = bits(w0, 0, 5)
    name = FETCH_OPS.get(opcode, "fetch_op_%d" % opcode)
    src_reg = bits(w0, 5, 6)
    dst_reg = bits(w0, 12, 6)
    dst_swiz = bits(w1, 0, 12)
    is_predicated = bits(w1, 31, 1)
    pred_condition = bits(w2, 31, 1)
    pred = "(%sp0) " % ("" if pred_condition else "!") if is_predicated else ""

    info = {
        "type": "fetch",
        "opcode": opcode,
        "name": name,
        "src_reg": src_reg,
        "dst_reg": dst_reg,
        "dst_swizzle": dst_swiz,
        "predicated": bool(is_predicated),
        "pred_condition": bool(pred_condition),
    }

    if opcode == 0:  # vertex fetch
        # Xenia: fetch_constant_index = const_index * 3 + const_index_sel.
        const_index = bits(w0, 20, 5) * 3 + bits(w0, 25, 2)
        info.update({
            "const_index": bits(w0, 20, 5),
            "const_index_sel": bits(w0, 25, 2),
            "fetch_slot": const_index,
            "format": bits(w1, 16, 6),
            "exp_adjust": sbits(w1, 24, 6),
            "is_mini_fetch": bool(bits(w1, 30, 1)),
            "stride": bits(w2, 0, 8),
            "offset": sbits(w2, 8, 23),
            "src_component": bits(w0, 30, 2),
        })
        info["text"] = ("%svfetch%s r%d.%s, r%d.%s, vf%d "
                        "(fmt %d, stride %d, offset %d)" % (
                            pred, "_mini" if info["is_mini_fetch"] else "_full",
                            dst_reg, swizzle_dst_str(dst_swiz),
                            src_reg, COMPONENTS[info["src_component"]],
                            const_index,
                            info["format"], info["stride"], info["offset"]))
    elif opcode == 1:  # texture fetch
        info.update({
            "const_index": bits(w0, 20, 5),
            "src_swizzle": bits(w0, 26, 6),
            "mag_filter": bits(w1, 12, 2),
            "min_filter": bits(w1, 14, 2),
            "mip_filter": bits(w1, 16, 2),
            "aniso_filter": bits(w1, 18, 3),
            "use_comp_lod": bool(bits(w1, 28, 1)),
            "use_reg_lod": bool(bits(w1, 29, 1)),
            "dimension": bits(w2, 14, 2),
            "lod_bias": sbits(w2, 2, 7),
            "offset_x": sbits(w2, 16, 5),
            "offset_y": sbits(w2, 21, 5),
            "offset_z": sbits(w2, 26, 5),
        })
        src_components = {0: 1, 1: 2, 2: 3, 3: 3}[info["dimension"]]
        src_swiz = "".join(
            COMPONENTS[bits(info["src_swizzle"], j * 2, 2)]
            for j in range(src_components))
        info["text"] = "%stfetch%s r%d.%s, r%d.%s, tf%d" % (
            pred, TEX_DIMENSIONS[info["dimension"]],
            dst_reg, swizzle_dst_str(dst_swiz), src_reg, src_swiz,
            info["const_index"])
    else:
        info["const_index"] = bits(w0, 20, 5)
        info["text"] = "%s%s r%d.%s, r%d, tf%d" % (
            pred, name, dst_reg, swizzle_dst_str(dst_swiz), src_reg,
            info["const_index"])
    return info


def swizzle_dst_str(dst_swiz):
    """Fetch destination swizzle: 3 bits per component, 4..6 are 0/1/keep.

    Positional, like xenia's disassembler: `_` marks a component the fetch does
    not write. Compacting those away instead loses which component each
    selector belongs to, which reads as the wrong destination.
    """
    out = []
    for i in range(4):
        v = bits(dst_swiz, i * 3, 3)
        if v < 4:
            out.append(COMPONENTS[v])
        elif v == 4:
            out.append("0")
        elif v == 5:
            out.append("1")
        else:  # 6 keeps the old value, 7 is unwritten
            out.append("_")
    return "".join(out)


def read_dwords(data):
    if len(data) % 4:
        raise UcodeError("microcode length %d is not dword aligned" % len(data))
    return list(struct.unpack(">%dI" % (len(data) // 4), data))


# Shaders that carry a patch table start their microcode payload with a 64 byte
# (16 dword) prefix before the control flow block; shaders without one start at
# zero. Verified against the SDK's own runtime shader dumps: for every dumped
# shader the dumped microcode is a byte exact suffix of the extracted payload,
# starting at 64 exactly when `patch_table_offset` is non zero and at 0
# otherwise (35 samples, and the split matches the 209 / 51 population counts).
#
# That prefix is the compiler's *literal constant pool*: four big endian float4s
# that occupy constant registers 252, 253, 254 and 255 of the shader's own bank.
# The game never writes them through either constant setter, so nothing reaches
# the register shadows and they read as zero unless they are taken from here.
# The correlation is exact over all 260 shaders: every shader with a prefix
# reads constants in 252..255 and no others above 240, and every shader without
# one reads none of them. Missing this is not subtle. Nearly every pixel shader
# spells saturate as `min(x, c253.w)` or clamps its alpha with `min(x, c255.x)`,
# so a zero pool collapses the alpha to 0 and the fixed function alpha test,
# which this title runs as GREATER on most draws, then discards the pixel.
PREFIX_CANDIDATES = (0, 64)


def find_prefix(data):
    """Return the byte offset of the control flow block within a payload."""
    best, best_count = None, 0
    for prefix in PREFIX_CANDIDATES:
        try:
            result = decode(data, prefix)
        except (UcodeError, IndexError, KeyError):
            continue
        # A wrong prefix makes the control flow bound heuristic latch onto
        # garbage and yields no instructions at all, so the candidate that
        # decodes the most instructions is the right one.
        if len(result["instructions"]) > best_count:
            best, best_count = prefix, len(result["instructions"])
    if best is None:
        raise UcodeError("no control flow block found at any known prefix")
    return best


def literal_constants(data, prefix=None):
    """Return the shader's literal pool as 16 floats, or None if it has none.

    The four float4s land in constant registers 252..255 of the shader's own
    bank, in order. See the note above PREFIX_CANDIDATES.
    """
    if prefix is None:
        prefix = find_prefix(data)
    if prefix == 0:
        return None
    return list(struct.unpack(">16f", data[:64]))


def decode(data, prefix=None):
    """Decode a microcode blob into control flow plus instruction slots."""
    if prefix is None:
        prefix = find_prefix(data)
    data = data[prefix:]
    dwords = read_dwords(data)
    n = len(dwords)
    if n < 3:
        raise UcodeError("microcode too short (%d dwords)" % n)

    # The control flow block sits at the top of the microcode, one pair of
    # control flow instructions per three dwords, which is also the size of one
    # instruction slot. Its length is not stored, so bound it by the lowest
    # address any exec refers to, scanning the whole blob first the way Xenia's
    # Shader::AnalyzeUcode does (the bound only ever shrinks, so a partial scan
    # can leave garbage pairs decoded from instruction data).
    def unpack_pair(offset):
        d0, d1, d2 = dwords[offset], dwords[offset + 1], dwords[offset + 2]
        return [ControlFlow(d0, d1 & 0xFFFF),
                ControlFlow(((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFF, d2 >> 16)]

    pair_bound = n // 3
    pair_index = 0
    while pair_index < pair_bound:
        for cf in unpack_pair(pair_index * 3):
            if cf.is_exec:
                pair_bound = min(pair_bound, cf.address)
        pair_index += 1

    cf_list = []
    for pair_index in range(pair_bound):
        cf_list.extend(unpack_pair(pair_index * 3))
    cf_dwords = pair_bound * 3
    instr_base = pair_bound

    instructions = {}
    for cf in cf_list:
        if not cf.is_exec:
            continue
        sequence = cf.sequence
        for slot in range(cf.count):
            addr = cf.address + slot
            off = addr * 3
            if off + 3 > n:
                raise UcodeError(
                    "exec at %d slot %d reads dwords %d..%d past end (%d)"
                    % (cf.address, slot, off, off + 3, n))
            if off < cf_dwords:
                raise UcodeError(
                    "exec at %d slot %d overlaps the control flow block"
                    % (cf.address, slot))
            is_fetch = (sequence >> (slot * 2)) & 1
            w0, w1, w2 = dwords[off], dwords[off + 1], dwords[off + 2]
            decoded = decode_fetch(w0, w1, w2) if is_fetch else decode_alu(w0, w1, w2)
            decoded["address"] = addr
            decoded["serialize"] = bool((sequence >> (slot * 2 + 1)) & 1)
            prev = instructions.get(addr)
            if prev is not None and prev["text"] != decoded["text"]:
                raise UcodeError("instruction %d decoded two different ways" % addr)
            instructions[addr] = decoded

    return {
        "prefix": prefix,
        "dword_count": n,
        "cf_dwords": cf_dwords,
        "control_flow": cf_list,
        "instructions": instructions,
        "instruction_base": instr_base,
    }


def disassemble(data):
    result = decode(data)
    lines = []
    for index, cf in enumerate(result["control_flow"]):
        lines.append("%3d  %s" % (index, cf))
        if cf.is_exec:
            for slot in range(cf.count):
                instr = result["instructions"][cf.address + slot]
                lines.append("       %3d  %s" % (instr["address"], instr["text"]))
    return "\n".join(lines)


def validate_all(directory, json_path=None):
    names = sorted(f for f in os.listdir(directory) if f.endswith(".ucode"))
    ok, failed = 0, []
    vec_hist, sca_hist, fetch_hist = {}, {}, {}
    summary = {}
    for name in names:
        path = os.path.join(directory, name)
        with open(path, "rb") as handle:
            data = handle.read()
        try:
            result = decode(data)
        except (UcodeError, IndexError, KeyError) as exc:
            failed.append((name, str(exc)))
            continue
        ok += 1
        for instr in result["instructions"].values():
            if instr["type"] == "alu":
                vec_hist[instr["vector_name"]] = vec_hist.get(instr["vector_name"], 0) + 1
                sca_hist[instr["scalar_name"]] = sca_hist.get(instr["scalar_name"], 0) + 1
            else:
                fetch_hist[instr["name"]] = fetch_hist.get(instr["name"], 0) + 1
        summary[name] = {
            "dwords": result["dword_count"],
            "cf_dwords": result["cf_dwords"],
            "cf_count": len(result["control_flow"]),
            "instruction_count": len(result["instructions"]),
            "instructions": [result["instructions"][a]["text"]
                             for a in sorted(result["instructions"])],
        }

    print("decoded %d/%d shaders" % (ok, len(names)))
    for name, message in failed:
        print("  FAIL %s: %s" % (name, message))
    print("\nvector opcodes: %s" % ", ".join(
        "%s=%d" % kv for kv in sorted(vec_hist.items(), key=lambda kv: -kv[1])))
    print("\nscalar opcodes: %s" % ", ".join(
        "%s=%d" % kv for kv in sorted(sca_hist.items(), key=lambda kv: -kv[1])))
    print("\nfetch opcodes: %s" % ", ".join(
        "%s=%d" % kv for kv in sorted(fetch_hist.items(), key=lambda kv: -kv[1])))

    if json_path:
        with open(json_path, "w") as handle:
            json.dump(summary, handle, indent=1)
        print("\nwrote %s" % json_path)
    return 0 if not failed else 1


def report_features(directory):
    """Report the features a translator would have to implement, not just the
    opcodes it would have to spell.

    `--all` already gives the opcode histogram, and it is small. That is not
    what decides whether these shaders can be compiled ahead of time into a
    structured high level language. Four things do, and none of them is an
    opcode count:

      * control flow. `loop`/`endloop`, `ccall`/`ret` and `cjmp` are a jump
        graph, and HLSL/GLSL want structured loops and ifs. A shader that only
        uses `exec`/`exece`/`alloc` is straight line code and translates
        trivially; one with a `cjmp` needs the graph restructured.
      * predication. `setp_*` writes p0 and later instructions execute under
        it, which becomes either an `if` or a `select` per instruction.
      * relative addressing on the destination register, which needs an
        indexable temp array rather than plain locals.
      * how many temp and constant registers are live, which sizes the
        declarations the emitted shader needs.

    Reported per shader type, since vertex and pixel shaders differ sharply.

    One gap stated rather than glossed: relative addressing of *constants*
    (indexing c[] by the address register) is not decoded by decode_alu, so it
    is not counted here. Absence from this report is not evidence of absence.
    """
    names = sorted(f for f in os.listdir(directory) if f.endswith(".ucode"))
    if not names:
        print("no .ucode files in %s" % directory)
        return 1

    # Three tiers, because "not straight line" lumps together two very
    # different costs. A conditional or predicated exec is an `if` around a
    # block and a loop/endloop pair is a `for`: both map onto structured high
    # level control flow directly. `cjmp`, `ccall` and `ret` are a jump graph,
    # and those are what force a restructuring pass.
    structured_cf = {"cexec", "cexece", "cexecp", "cexecpe", "cexecpc", "cexecpce",
                     "loop", "endloop"}
    unstructured_cf = {"cjmp", "ccall", "ret"}

    groups = {}
    for name in names:
        kind = "vertex" if name.startswith("vs") else "pixel"
        stats = groups.setdefault(kind, {
            "shaders": 0, "cf_hist": {}, "alloc_hist": {}, "tex_dim_hist": {},
            "export_hist": {}, "unstructured": [], "with_predication": 0,
            "with_dest_rel": 0, "max_temp": 0, "max_const": 0,
            "max_instructions": 0, "straight_line": 0, "structured": 0,
            "backward_jumps": 0, "nested_pairs": 0, "crossing_pairs": 0,
        })

        with open(os.path.join(directory, name), "rb") as handle:
            data = handle.read()
        try:
            result = decode(data)
        except (UcodeError, IndexError, KeyError) as exc:
            print("  FAIL %s: %s" % (name, exc))
            continue

        stats["shaders"] += 1
        # `cjmp` is the only thing here that is a jump rather than a block
        # structure, so whether it can be emitted as a plain `if` is what
        # decides if a restructuring pass is needed at all. Two properties make
        # it structurable: the jump must go forward, and its guarded body must
        # nest with every other one rather than partially overlap. The guarded
        # body of `cjmp at i -> t` is blocks i+1..t-1; the cjmp slot itself is
        # not inside it, which is why adjacent if/else-if chains look like
        # overlaps unless the interval is taken exclusively.
        jumps = [(index + 1, bits(cf.dword_0, 0, 13) - 1)
                 for index, cf in enumerate(result["control_flow"]) if cf.opcode == 11]
        for start, end in jumps:
            if end < start - 1:
                stats["backward_jumps"] += 1
        for a in range(len(jumps)):
            for b in range(a + 1, len(jumps)):
                s1, e1 = jumps[a]
                s2, e2 = jumps[b]
                disjoint = e1 < s2 or e2 < s1
                nested = (s1 <= s2 and e2 <= e1) or (s2 <= s1 and e1 <= e2)
                if disjoint or nested:
                    stats["nested_pairs"] += 1
                else:
                    stats["crossing_pairs"] += 1

        shader_structured = set()
        shader_unstructured = set()
        for cf in result["control_flow"]:
            # nop pads the block out to a whole pair; counting it says nothing.
            if cf.opcode == 0:
                continue
            stats["cf_hist"][cf.name] = stats["cf_hist"].get(cf.name, 0) + 1
            if cf.name in structured_cf:
                shader_structured.add(cf.name)
            if cf.name in unstructured_cf:
                shader_unstructured.add(cf.name)
            if cf.opcode == 12:
                alloc = ALLOC_TYPES[bits(cf.dword_1, 9, 2)]
                stats["alloc_hist"][alloc] = stats["alloc_hist"].get(alloc, 0) + 1
        if shader_unstructured:
            stats["unstructured"].append((name, sorted(shader_unstructured)))
        elif shader_structured:
            stats["structured"] += 1
        else:
            stats["straight_line"] += 1

        predicated = False
        dest_rel = False
        for instr in result["instructions"].values():
            if instr["predicated"]:
                predicated = True
            if instr["type"] == "alu":
                if instr["vector_dest_rel"]:
                    dest_rel = True
                # scalar_dest_rel is reused as an export mask bit, so it only
                # means relative addressing on a non-export instruction.
                if instr["scalar_dest_rel"] and not instr["export"]:
                    dest_rel = True
                if instr["export"]:
                    index = instr["vector_dest"]
                    stats["export_hist"][index] = stats["export_hist"].get(index, 0) + 1
                else:
                    stats["max_temp"] = max(stats["max_temp"], instr["vector_dest"],
                                            instr["scalar_dest"])
                for src in instr["sources"]:
                    if src["is_temp"]:
                        stats["max_temp"] = max(stats["max_temp"], src["reg"] & 0x3F)
                    else:
                        stats["max_const"] = max(stats["max_const"], src["reg"])
            else:
                stats["max_temp"] = max(stats["max_temp"], instr["dst_reg"],
                                        instr["src_reg"])
                if instr["opcode"] == 1:
                    dim = TEX_DIMENSIONS[instr["dimension"]]
                    stats["tex_dim_hist"][dim] = stats["tex_dim_hist"].get(dim, 0) + 1
        stats["with_predication"] += 1 if predicated else 0
        stats["with_dest_rel"] += 1 if dest_rel else 0
        stats["max_instructions"] = max(stats["max_instructions"],
                                        len(result["instructions"]))

    for kind in ("vertex", "pixel"):
        stats = groups.get(kind)
        if not stats:
            continue
        print("\n=== %s shaders (%d) ===" % (kind, stats["shaders"]))
        print("control flow: %s" % ", ".join(
            "%s=%d" % kv for kv in sorted(stats["cf_hist"].items(), key=lambda kv: -kv[1])))
        print("alloc: %s" % ", ".join(
            "%s=%d" % kv for kv in sorted(stats["alloc_hist"].items(), key=lambda kv: -kv[1])))
        if stats["tex_dim_hist"]:
            print("tfetch dimensions: %s" % ", ".join(
                "%s=%d" % kv for kv in sorted(stats["tex_dim_hist"].items(),
                                              key=lambda kv: -kv[1])))
        print("exports written: %s" % ", ".join(
            "e%d=%d" % kv for kv in sorted(stats["export_hist"].items())))
        print("shape: straight line=%d, structured (if/loop)=%d, "
              "unstructured (jump graph)=%d, of %d"
              % (stats["straight_line"], stats["structured"],
                 len(stats["unstructured"]), stats["shaders"]))
        print("using predication: %d" % stats["with_predication"])
        print("using relative destination addressing: %d" % stats["with_dest_rel"])
        print("max temp register r%d, max constant register c%d, "
              "longest shader %d instructions"
              % (stats["max_temp"], stats["max_const"], stats["max_instructions"]))
        jump_ops = {}
        for _, ops in stats["unstructured"]:
            for op in ops:
                jump_ops[op] = jump_ops.get(op, 0) + 1
        if jump_ops:
            print("using jumps, by opcode: %s" % ", ".join(
                "%s=%d" % kv for kv in sorted(jump_ops.items(), key=lambda kv: -kv[1])))
        if stats["nested_pairs"] or stats["crossing_pairs"] or stats["backward_jumps"]:
            # If both of these are zero the jump graph is a properly nested
            # forward if-structure, and every one of these shaders emits as
            # structured code with no restructuring pass.
            print("cjmp structurability: backward jumps=%d, partially overlapping "
                  "guarded bodies=%d (of %d pairs) -> %s"
                  % (stats["backward_jumps"], stats["crossing_pairs"],
                     stats["nested_pairs"] + stats["crossing_pairs"],
                     "emits as nested ifs"
                     if not stats["backward_jumps"] and not stats["crossing_pairs"]
                     else "NEEDS RESTRUCTURING"))
    return 0


# D3DDECLUSAGE, as it appears in the vertex fetch patch table below.
DECL_USAGES = {
    0: "POSITION", 1: "BLENDWEIGHT", 2: "BLENDINDICES", 3: "NORMAL",
    4: "PSIZE", 5: "TEXCOORD", 6: "TANGENT", 7: "BINORMAL",
    8: "TESSFACTOR", 9: "POSITIONT", 10: "COLOR", 11: "FOG",
    12: "DEPTH", 13: "SAMPLE",
}


def vertex_fetch_table(header):
    """Read a vertex shader's vertex fetch patch table out of its container.

    At draw time `sub_82267218` walks this table and rewrites every vfetch
    instruction in a scratch copy of the microcode from the bound vertex
    declaration, matching each entry to a declaration element by usage and
    usage index. Offline it is the shader's vertex input signature.

    Layout, derived from that function: header dword 6 is the byte offset of a
    program sub header S; S[6] is a dword offset and S[7] the entry count, and
    the table starts at S[9 + S[6]]. Each entry packs the instruction index in
    bits 0..11, the usage in bits 12..15 and the usage index in bits 16..19.
    """
    dwords = struct.unpack(">%dI" % (len(header) // 4), header)
    sub_header = dwords[dwords[6] // 4:]
    start = 9 + sub_header[6]
    entries = []
    for entry in sub_header[start:start + sub_header[7]]:
        usage = (entry >> 12) & 0xF
        entries.append({
            "instruction": entry & 0xFFF,
            "usage": usage,
            "usage_name": DECL_USAGES.get(usage, "usage_%d" % usage),
            "usage_index": (entry >> 16) & 0xF,
            "raw": entry,
        })
    return entries


def interpolator_signature(header):
    """Read a shader's interpolator signature table out of its container.

    This is the table `sub_822679A8` walks at draw time to reconcile a pixel
    shader's inputs against the bound vertex shader's outputs: both lists are
    sorted by a semantic key, and where the same key sits in different
    registers in the two shaders it rewrites the pixel shader's source register
    index. Offline it is the interpolator signature of either shader type.

    The entry count is bits 5..9 of sub header S[5]. Vertex shaders carry the
    vertex fetch table first, so their entries start at `9 + S[6] + S[7]`, past
    it; pixel shaders have no fetch table and start at S[8]. Each entry packs
    the semantic key in bits 0..7 and the interpolator register in bits 8..11.

    Checked over all 260 containers: every table is sorted by key, the register
    index is always the entry's own position, the registers form a dense 0..n-1
    range that matches the shader's interpolator exports (vertex) or its
    read-before-write registers (pixel), and the 12 keys in the title are all
    used by both shader types.
    """
    dwords = struct.unpack(">%dI" % (len(header) // 4), header)
    sub_header = dwords[dwords[6] // 4:]
    count = (sub_header[5] >> 5) & 0x1F
    is_vertex = bool(dwords[0] & 1)
    start = 9 + sub_header[6] + sub_header[7] if is_vertex else 8
    entries = []
    for position, entry in enumerate(sub_header[start:start + count]):
        register = (entry >> 8) & 0xF
        if register != position:
            raise UcodeError("interpolator %d is register %d, not its position"
                             % (position, register))
        entries.append({"key": entry & 0xFF, "register": register,
                        "raw": entry})
    if len(entries) != count:
        raise UcodeError("interpolator table truncated: %d of %d"
                         % (len(entries), count))
    return entries


def report_vertex_inputs(directory, json_path=None):
    """Print each vertex shader's input signature, checked against its ucode."""
    signatures, mismatches = {}, []
    names = sorted(name[:-len(".header")] for name in os.listdir(directory)
                   if name.startswith("vs_") and name.endswith(".header"))
    for name in names:
        with open(os.path.join(directory, name + ".header"), "rb") as handle:
            entries = vertex_fetch_table(handle.read())
        with open(os.path.join(directory, name + ".ucode"), "rb") as handle:
            result = decode(handle.read())
        fetches = sorted(address for address, instruction
                         in result["instructions"].items()
                         if instruction["type"] == "fetch")
        if sorted(entry["instruction"] for entry in entries) != fetches:
            mismatches.append(name)
        signatures[name] = entries
        print("%s  %s" % (name, ", ".join(
            "%s%d@%d" % (entry["usage_name"], entry["usage_index"],
                         entry["instruction"]) for entry in entries)))

    total = sum(len(entries) for entries in signatures.values())
    print("\n%d vertex shaders, %d fetch entries" % (len(signatures), total))
    print("entries not matching the decoded vfetch instructions: %d"
          % len(mismatches))
    for name in mismatches:
        print("  %s" % name)
    if json_path:
        with open(json_path, "w") as handle:
            json.dump(signatures, handle, indent=1)
        print("wrote %s" % json_path)
    return 0 if not mismatches else 1


def verify_against_dumps(directory, dumps_dir):
    """Cross-check the offline decode against the SDK's runtime shader dumps.

    The dumps hold the microcode exactly as the GPU saw it (little endian
    dwords) next to the disassembly the SDK produced for it, so they are an
    independent check on both the payload prefix and the instruction decode.
    """
    payloads = {}
    for name in sorted(os.listdir(directory)):
        if name.endswith(".ucode"):
            with open(os.path.join(directory, name), "rb") as handle:
                payloads[name[:-len(".ucode")]] = handle.read()

    matched, unmatched, mismatches = 0, [], []
    for dump in sorted(os.listdir(dumps_dir)):
        if ".ucode.bin." not in dump:
            continue
        with open(os.path.join(dumps_dir, dump), "rb") as handle:
            raw = handle.read()
        swapped = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
        hit = None
        for name, payload in payloads.items():
            offset = payload.find(swapped)
            if offset >= 0:
                hit = (name, offset)
                break
        if hit is None:
            unmatched.append(dump)
            continue
        name, offset = hit
        matched += 1

        result = decode(payloads[name])
        if result["prefix"] != offset:
            mismatches.append("%s (%s): prefix %d, dump starts at %d"
                              % (dump, name, result["prefix"], offset))
            continue

        # Compare the opcode found at each instruction address against the
        # SDK's own disassembly text.
        text_path = os.path.join(dumps_dir, dump.replace(".ucode.bin.", ".ucode."))
        if not os.path.exists(text_path):
            continue
        reference = {}
        with open(text_path) as handle:
            for line in handle:
                head, _, rest = line.partition("*/")
                if not head.startswith("/*") or not rest.strip():
                    continue
                try:
                    address = int(head[2:].strip())
                except ValueError:
                    continue
                tokens = rest.split()
                if tokens[0].startswith("("):  # predicate prefix
                    tokens = tokens[1:]
                if tokens:
                    reference[address] = tokens[0]
        if set(reference) != set(result["instructions"]):
            mismatches.append("%s (%s): instruction addresses differ" % (dump, name))
            continue
        for address, opcode in reference.items():
            if opcode == "serialize":
                continue  # a modifier the SDK prints on its own line
            instruction = result["instructions"][address]
            if instruction["type"] == "alu":
                # Both halves are compared, including a half the disassembly
                # here suppresses because its write mask is empty.
                names = [instruction["vector_name"], instruction["scalar_name"]]
            else:
                names = [instruction["name"]]
                if instruction["opcode"] == 1:
                    names = ["tfetch" + TEX_DIMENSIONS[instruction["dimension"]]]
            if not any(opcode.lower().startswith(n.lower()) for n in names):
                mismatches.append("%s (%s): address %d is %s, dump says %s"
                                  % (dump, name, address,
                                     instruction["text"], opcode))

    print("dumps matched to an extracted payload: %d" % matched)
    if unmatched:
        print("dumps with no matching payload: %d" % len(unmatched))
        for dump in unmatched[:10]:
            print("  %s" % dump)
    print("decode mismatches: %d" % len(mismatches))
    for message in mismatches[:20]:
        print("  %s" % message)
    return 0 if not mismatches else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help=".ucode file, or directory with --all")
    parser.add_argument("--all", action="store_true",
                        help="decode every .ucode in the directory and report")
    parser.add_argument("--json", help="with --all, write a decode summary here")
    parser.add_argument("--vertex-inputs", action="store_true",
                        help="report every vertex shader's input signature")
    parser.add_argument("--verify-dumps", metavar="DIR",
                        help="cross-check the decode against SDK shader dumps")
    parser.add_argument("--features", action="store_true",
                        help="report the control flow, predication and register "
                             "features a translator would have to support")
    args = parser.parse_args()

    if args.features:
        return report_features(args.path)
    if args.vertex_inputs:
        return report_vertex_inputs(args.path, args.json)
    if args.verify_dumps:
        return verify_against_dumps(args.path, args.verify_dumps)
    if args.all:
        return validate_all(args.path, args.json)

    with open(args.path, "rb") as handle:
        print(disassemble(handle.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
