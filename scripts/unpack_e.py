"""Eternal Sonata (Xbox 360) asset decoder.

Reverse-engineered from default.xex:
  sub_8210DFA8  - init (reads 256-byte frequency table + 4 code bytes)
  sub_8210E0F8  - range-coder symbol decode  (flag bit 1)
  sub_8210E260  - LZSS layer                 (flag bit 0)

index.vmtoc: 48-byte records, [0:32]=lowercase name, [32:36]=BE u32 decoded
size, [36]=flag (bit0 = LZSS, bit1 = range-coded).
"""
import os
import struct
import sys

M32 = 0xFFFFFFFF
ROOT = r'C:\Users\m.andronaco\dev\EternalSonataReprise\assets'


def load_toc(root=ROOT):
    d = open(os.path.join(root, 'index.vmtoc'), 'rb').read()
    toc = {}
    for i in range(len(d) // 48):
        e = d[i * 48:(i + 1) * 48]
        nm = e[0:32].split(b'\x00')[0].decode('latin1')
        if nm:
            toc[nm.lower().replace('\\', '/')] = (struct.unpack('>I', e[32:36])[0], e[36])
    return toc


class RangeDecoder:
    def __init__(self, data):
        self.d = data
        self.p = 0
        self.freq = list(data[:256])
        self.p = 256
        self.cum = [0] * 257
        for i in range(256):
            self.cum[i + 1] = self.cum[i] + self.freq[i]
        self.total = self.cum[256] & 0xFFFF
        self.lut = bytearray(self.total)
        v7 = 0
        for sym in range(256):
            while v7 < self.cum[sym + 1]:
                self.lut[v7] = sym
                v7 += 1
        self.low = 0
        self.range = M32
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & M32

    def _byte(self):
        if self.p >= len(self.d):
            return None
        b = self.d[self.p]
        self.p += 1
        return b

    def get(self):
        # loop 1: renormalize while top byte of (low, low+range) matches
        while (((self.low + self.range) & M32) ^ self.low) < 0x1000000:
            b = self._byte()
            if b is None:
                return -1
            self.low = (self.low << 8) & M32
            self.range = (self.range << 8) & M32
            self.code = ((self.code << 8) | b) & M32
        # loop 2: underflow
        while self.range < 0x2000:
            b = self._byte()
            if b is None:
                return -1
            old = self.low
            self.low = (old << 8) & M32
            self.range = (-(old * 256)) & 0x1FFF00
            self.code = ((self.code << 8) | b) & M32
        r = self.range // self.total
        sym = self.lut[((self.code - self.low) & M32) // r]
        self.low = (self.low + self.cum[sym] * r) & M32
        self.range = (self.freq[sym] * r) & M32
        return sym


class RawReader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def get(self):
        if self.p >= len(self.d):
            return -1
        b = self.d[self.p]
        self.p += 1
        return b


def unpack(data, out_size, flag):
    src = RangeDecoder(data) if (flag & 2) else RawReader(data)
    out = bytearray()
    if not (flag & 1):
        while len(out) < out_size:
            v = src.get()
            if v == -1:
                break
            out.append(v)
        return bytes(out)

    ring = bytearray(4096)
    pos = 4078
    state = 0        # 0=need flag byte, 1=literal, 2=match lo byte, 3=match hi byte
    flagbyte = 0
    mask = 0
    matchlo = 0
    while len(out) < out_size:
        v = src.get()
        if v == -1:
            break
        if state == 0:
            flagbyte = v
            mask = 1
            state = 2 if (v & 1) == 0 else 1
            continue
        if state == 1:
            out.append(v)
            ring[pos] = v
            pos = (pos + 1) & 0xFFF
        elif state == 2:
            matchlo = v
            state = 3
            continue
        else:  # state 3
            n = (v & 0xF) + 3
            off = (((v << 4) & 0xF00) | matchlo) & 0xFFF
            for _ in range(n):
                c = ring[off]
                off = (off + 1) & 0xFFF
                out.append(c)
                ring[pos] = c
                pos = (pos + 1) & 0xFFF
        mask = (mask << 1) & 0xFF
        if mask == 0:
            state = 0
        else:
            state = 2 if (flagbyte & mask) == 0 else 1
    return bytes(out)


def unpack_file(relpath, root=ROOT, toc=None):
    toc = toc or load_toc(root)
    key = relpath.lower().replace('\\', '/')
    size, flag = toc[key]
    data = open(os.path.join(root, relpath), 'rb').read()
    return unpack(data, size, flag), size, flag


if __name__ == '__main__':
    rel = sys.argv[1] if len(sys.argv) > 1 else r'btldata\script\tutorial\t0001.e'
    out, size, flag = unpack_file(rel)
    print('%s flag=%d expected=%d got=%d' % (rel, flag, size, len(out)))
    dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out',
                       os.path.basename(rel) + '.bin')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(out)
    print('wrote', dst)
    print(out[:64].hex())
