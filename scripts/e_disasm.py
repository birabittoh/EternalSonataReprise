#!/usr/bin/env python3
"""Disassemble the bytecode of a decoded .e image (docs/script-vm-notes.md §8).

    python scripts/e_disasm.py extracted/e/cfdata/bel01.e 0x186f 0x1899
    python scripts/e_disasm.py extracted/e/cfdata/bel01.e --imports

Native calls (7d) and pointer operands whose patch offset appears in the
block2 import tables are shown as symN; everything else as an image offset.
"""
import argparse
import struct
import sys

# opcode: (mnemonic, operand width, operand kind)
# kinds: '' none, 'u' unsigned, 's' signed, 'p' relocated pointer, 'f' float
# bits, 'j' relative jump, 'd' f64
OPS = {
    0x00: ('halt', 0, ''), 0x01: ('acc=u8', 1, 'u'), 0x02: ('acc=u16', 2, 'u'),
    0x03: ('acc=u32', 4, 'f'), 0x04: ('acc=f64', 8, 'd'), 0x05: ('acc=-u8', 1, 'u'),
    0x06: ('acc=-u16', 2, 'u'), 0x07: ('acc=ptr', 4, 'p'),
    0x08: ('acc=&L[s32]', 4, 's'), 0x09: ('acc=&L[s8]', 1, 's'),
    0x0a: ('acc=*u8 ptr', 4, 'p'), 0x0b: ('acc=*u16 ptr', 4, 'p'),
    0x0c: ('acc=*u32 ptr', 4, 'p'), 0x0d: ('acc=*f64 ptr', 4, 'p'),
    0x0e: ('acc=*s8 ptr', 4, 'p'), 0x0f: ('acc=*s16 ptr', 4, 'p'),
    0x10: ('acc=L.u8[s32]', 4, 's'), 0x11: ('acc=L.u16[s32]', 4, 's'),
    0x12: ('acc=L.u32[s32]', 4, 's'), 0x13: ('acc=L.f64[s32]', 4, 's'),
    0x14: ('acc=L.s8[s32]', 4, 's'), 0x15: ('acc=L.s16[s32]', 4, 's'),
    0x16: ('acc=L.u8[s8]', 1, 's'), 0x17: ('acc=L.u16[s8]', 1, 's'),
    0x18: ('acc=L.u32[s8]', 1, 's'), 0x19: ('acc=L.f64[s8]', 1, 's'),
    0x1a: ('acc=L.s8[s8]', 1, 's'), 0x1b: ('acc=L.s16[s8]', 1, 's'),
    0x1c: ('acc=*u8 acc', 0, ''), 0x1d: ('acc=*u16 acc', 0, ''),
    0x1e: ('acc=*u32 acc', 0, ''), 0x1f: ('acc=*f64 acc', 0, ''),
    0x20: ('acc=*s8 acc', 0, ''), 0x21: ('acc=*s16 acc', 0, ''),
    0x22: ('*u8 pop=acc', 0, ''), 0x23: ('*u16 pop=acc', 0, ''),
    0x24: ('*u32 pop=acc', 0, ''), 0x25: ('*f64 pop=acc', 0, ''),
    0x26: ('memcpy(pop,acc,u32)', 4, 'u'), 0x27: ('memcpy(pop,acc,u8)', 1, 'u'),
    0x28: ('acc=f32(int)', 0, ''), 0x29: ('acc=f64(int)', 0, ''),
    0x2a: ('acc=f32(uint)', 0, ''), 0x2b: ('acc=f64(uint)', 0, ''),
    0x2c: ('acc=int(f32)', 0, ''), 0x2d: ('acc=f64(f32)', 0, ''),
    0x2e: ('acc=int(f64)', 0, ''), 0x2f: ('acc=f32(f64)', 0, ''),
    0x30: ('acc=!acc', 0, ''), 0x31: ('acc=(f64acc==0)', 0, ''),
    0x32: ('acc=pop+acc', 0, ''), 0x33: ('acc=pop+acc f32', 0, ''), 0x34: ('acc=pop+acc f64', 0, ''),
    0x35: ('acc=pop-acc', 0, ''), 0x36: ('acc=pop-acc f32', 0, ''), 0x37: ('acc=pop-acc f64', 0, ''),
    0x38: ('acc=pop*acc', 0, ''), 0x39: ('acc=pop*acc', 0, ''),
    0x3a: ('acc=pop*acc f32', 0, ''), 0x3b: ('acc=pop*acc f64', 0, ''),
    0x3c: ('acc=pop/acc u', 0, ''), 0x3d: ('acc=pop/acc', 0, ''),
    0x3e: ('acc=pop/acc f32', 0, ''), 0x3f: ('acc=pop/acc f64', 0, ''),
    0x40: ('acc=pop%acc u', 0, ''), 0x41: ('acc=pop%acc', 0, ''),
    0x42: ('acc=pop>>acc', 0, ''), 0x43: ('acc=pop>>acc s', 0, ''), 0x44: ('acc=pop<<acc', 0, ''),
    0x45: ('acc=pop&acc', 0, ''), 0x46: ('acc=pop^acc', 0, ''), 0x47: ('acc=pop|acc', 0, ''),
    0x48: ('acc=-acc', 0, ''), 0x49: ('acc=-acc f32', 0, ''), 0x4a: ('acc=-acc f64', 0, ''),
    0x4b: ('acc=~acc', 0, ''), 0x4c: ('*u8acc |= u8', 1, 'u'),
    0x4d: ('*u8acc+=u8 ->new', 1, 'u'), 0x4e: ('*u16acc+=s8 ->new', 1, 's'),
    0x4f: ('*u32acc+=s8 ->new', 1, 's'), 0x50: ('*f32acc+=s8 ->new', 1, 's'),
    0x51: ('*f64acc+=s8 ->new', 1, 's'), 0x52: ('*s8acc+=u8 ->new', 1, 'u'),
    0x53: ('*s16acc+=s8 ->new', 1, 's'), 0x54: ('*u32acc+=s32 ->new', 4, 's'),
    0x55: ('*u8acc+=u8 ->old', 1, 'u'), 0x56: ('*u16acc+=s8 ->old', 1, 's'),
    0x57: ('*u32acc+=s8 ->old', 1, 's'), 0x58: ('*f32acc+=s8 ->old', 1, 's'),
    0x59: ('*f64acc+=s8 ->old', 1, 's'), 0x5a: ('*s8acc+=u8 ->old', 1, 'u'),
    0x5b: ('*s16acc+=s8 ->old', 1, 's'), 0x5c: ('*u32acc+=s32 ->old', 4, 's'),
    0x5d: ('acc=pop==acc', 0, ''), 0x5e: ('acc=pop==acc f32', 0, ''), 0x5f: ('acc=pop==acc f64', 0, ''),
    0x60: ('acc=pop!=acc', 0, ''), 0x61: ('acc=pop!=acc f32', 0, ''), 0x62: ('acc=pop!=acc f64', 0, ''),
    0x63: ('acc=pop<acc', 0, ''), 0x64: ('acc=pop<acc f32', 0, ''), 0x65: ('acc=pop<acc f64', 0, ''),
    0x66: ('acc=pop<=acc', 0, ''), 0x67: ('acc=pop<=acc f32', 0, ''), 0x68: ('acc=pop<=acc f64', 0, ''),
    0x69: ('acc=pop<acc u', 0, ''), 0x6a: ('acc=pop<=acc u', 0, ''),
    0x6b: ('acc=pop>acc', 0, ''), 0x6c: ('acc=pop>acc f32', 0, ''), 0x6d: ('acc=pop>acc f64', 0, ''),
    0x6e: ('acc=pop>=acc', 0, ''), 0x6f: ('acc=pop>=acc f32', 0, ''), 0x70: ('acc=pop>=acc f64', 0, ''),
    0x71: ('acc=pop>acc u', 0, ''), 0x72: ('acc=pop>=acc u', 0, ''), 0x73: ('acc=(acc==0)', 0, ''),
    0x74: ('jmp', 4, 'j'), 0x75: ('jz', 4, 'j'), 0x76: ('jnz', 4, 'j'),
    0x77: ('jmp', 1, 'j'), 0x78: ('jz', 1, 'j'), 0x79: ('jnz', 1, 'j'),
    0x7a: ('call', 4, 'p'), 0x7b: ('calltab', 4, 'u'), 0x7c: ('ret', 0, ''),
    0x7d: ('native', 4, 'p'), 0x7e: ('sleep acc', 0, ''),
    0x7f: ('frame u32', 4, 'u'), 0x80: ('frame u8', 1, 'u'),
    0x81: ('push', 0, ''), 0x82: ('push f64', 0, ''),
    0x83: ('push struct u32', 4, 'u'), 0x84: ('push struct u8', 1, 'u'),
    0x85: ('pop4', 0, ''), 0x86: ('pop8', 0, ''), 0x87: ('pop u32', 4, 'u'), 0x88: ('pop u8', 1, 'u'),
    0x89: ('switch', 4, 'p'),
}


def parse(data):
    """Return (image_size, block2 tables) for a decoded .e file."""
    _magic, _hid, _ts, _total, imsz, reloc = struct.unpack('>6I', data[:24])
    o = 0x18 + imsz + reloc
    for _ in range(2):  # list A, list B
        n = struct.unpack('>I', data[o:o + 4])[0]
        o += 4 + 4 * n
    tables = []
    for _ in range(4):
        n = struct.unpack('>I', data[o:o + 4])[0]
        o += 4
        tables.append([struct.unpack('>II', data[o + 8 * i:o + 8 * i + 8]) for i in range(n)])
        o += 8 * n
    return imsz + 0x18, tables


def disassemble(data, imports, start, end, out=sys.stdout):
    o = start
    while o < end:
        op = data[o]
        name, width, kind = OPS.get(op, ('??', 0, ''))
        a = o + 1
        arg = ''
        if kind == 'u':
            arg = str(int.from_bytes(data[a:a + width], 'big'))
        elif kind == 's':
            arg = str(int.from_bytes(data[a:a + width], 'big', signed=True))
        elif kind == 'p':
            if a in imports:
                arg = 'sym%d' % imports[a]
            else:
                arg = 'img+%#x' % struct.unpack('>I', data[a:a + 4])[0]
        elif kind == 'f':
            arg = '%#x (%g)' % (struct.unpack('>I', data[a:a + 4])[0], struct.unpack('>f', data[a:a + 4])[0])
        elif kind == 'd':
            arg = str(struct.unpack('>d', data[a:a + 8])[0])
        elif kind == 'j':
            rel = int.from_bytes(data[a:a + width], 'big', signed=True)
            arg = '-> %#x' % (a + rel)
        print('%04x: %-26s %s' % (o, name, arg), file=out)
        o = a + width


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file')
    ap.add_argument('start', nargs='?', help='image offset (hex ok)')
    ap.add_argument('end', nargs='?', help='image offset (hex ok); defaults to image end')
    ap.add_argument('--imports', action='store_true', help='list block2 import tables and exit')
    args = ap.parse_args()
    data = open(args.file, 'rb').read()
    image_end, tables = parse(data)
    if args.imports:
        for t, ents in enumerate(tables):
            for sym, off in ents:
                print('table%d %6d @ %#x (op %02x)' % (t, sym, off, data[off - 1]))
        return
    imports = {off: sym for ents in tables for sym, off in ents}
    start = int(args.start, 0) if args.start else 0x18
    end = int(args.end, 0) if args.end else image_end
    disassemble(data, imports, start, end)


if __name__ == '__main__':
    main()
