"""PS3 audio -> 360 game directory, for ps3_convert.py.

PS3 sound banks hold mono ATRAC3 clips (192 byte frames) where the 360's hold
XMA2, and no XMA encoder exists. Every clip gets a stub payload that starts
with an RXPcmSub tag, and its frames go to pcm/<token>.wav in the game
directory, which the host decodes and substitutes when the XMA decoder meets
the tag.

Music: kind 0 .cps is big endian PCM and becomes the plain .wav the scripts
ask for; PS-ADPCM tracks become a .cxs whose payload carries the tag, with the
ADPCM frames in the sidecar. See docs/ps3-assets.md.
"""
import os
import struct

u32 = struct.Struct('>I')

TAG = b'RXPcmSub'
PACKET = 0x800
ATRAC3_FRAME = 192
ATRAC3_SAMPLES = 1024
# Sidecar WAV format tags: ATRAC3's registered one, and a private one for
# PS-ADPCM, channels interleaved per 16 byte frame of 28 samples.
WAVE_ATRAC3 = 0x0270
WAVE_PSX_ADPCM = 0x5053


def rd32(d, o):
    return u32.unpack_from(d, o)[0]


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def token(key):
    """8 byte FNV-1a of a stable key; the sidecar's file name and the tag's tail."""
    h = 0xCBF29CE484222325
    for c in key.encode('latin1'):
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h.to_bytes(8, 'little')


def sidecar_wav(fmt, data, loop=None, rate=48000):
    """Little endian WAV around encoded frames; loop is (start, end) in
    samples, end exclusive."""
    body = b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    if loop:
        smpl = struct.pack('<9I', 0, 0, 1000000000 // rate, 60, 0, 0, 0, 1, 0)
        smpl += struct.pack('<6I', 0, 0, loop[0], loop[1] - 1, 0, 0)
        body += b'smpl' + struct.pack('<I', len(smpl)) + smpl
    body += b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body


def atrac3_wav(frames, rate, loop):
    ext = struct.pack('<HIHHHH', 1, 0, 0, 0, 1, 0)
    fmt = struct.pack('<HHIIHHH', WAVE_ATRAC3, 1, rate, ATRAC3_FRAME * rate // ATRAC3_SAMPLES,
                      ATRAC3_FRAME, 0, len(ext)) + ext
    return sidecar_wav(fmt, frames, loop, rate)


def psx_adpcm_wav(data, channels, rate, loop):
    fmt = struct.pack('<HHIIHH', WAVE_PSX_ADPCM, channels, rate, rate * channels * 16 // 28,
                      16 * channels, 4)
    return sidecar_wav(fmt, data, loop, rate)


def guest_wav(pcm_be, channels, rate):
    """The 360's own sound/cxs/*.wav layout: RIFF with big endian fields and samples."""
    fmt = struct.pack('>HHIIHH', 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b'fmt ' + u32.pack(len(fmt)) + fmt + b'data' + u32.pack(len(pcm_be)) + pcm_be
    return b'RIFF' + u32.pack(4 + len(body)) + b'WAVE' + body


# ---------------------------------------------------------------------------
# Sound banks
# ---------------------------------------------------------------------------
class Tim:
    def __init__(self, d, at):
        self.raw = d[at:at + rd32(d, at + 4)]
        (self.flags, self.rate, self.offset, self.size, self.loop_start, self.loop_end,
         self.lip_off, self.lip_len, self.xma_off, self.xma_len, self.seek_off,
         self.seek_len) = struct.unpack_from('>12I', self.raw, 8)

    def samples(self):
        return self.size // ATRAC3_FRAME * ATRAC3_SAMPLES


def parse_csf(d):
    """-> (bytes before PGHD, PGHD header, [(PROG header, [Tim])], header size)."""
    head = rd32(d, 8)
    at = 0x10
    while d[at:at + 4] != b'PGHD':
        at += rd32(d, at + 4)
        if at >= head:
            raise ValueError('no PGHD in the bank header')
    pre, pghd = d[:at], d[at:at + 16]
    end = at + rd32(d, at + 4)
    progs = []
    at += 16
    while at < end:
        if d[at:at + 4] != b'PROG':
            raise ValueError(f'expected PROG at {at:#x}')
        prog_end = at + rd32(d, at + 4)
        tims = []
        t = at + 16
        while t < prog_end:
            tims.append(Tim(d, t))
            t += rd32(d, t + 4)
        progs.append((d[at:at + 16], tims))
        at = prog_end
    return pre, pghd, progs, head


def new_tim(ps3, offset, size, frames, loops):
    """A 360 TIM for a substituted clip: the PS3 clip's parameters, LIP and
    rate, with one XMA block of stub payload described as frames long."""
    raw = ps3.raw
    subs = [o for o in (ps3.lip_off, ps3.xma_off, ps3.seek_off) if o]
    params = bytearray(raw[0x38:(min(subs) if subs else len(raw)) & ~3])
    # The low half of the first parameter word is 6 on every PS3 clip, 0 on the 360.
    params[2:4] = b'\0\0'
    lip = raw[ps3.lip_off:ps3.lip_off + ps3.lip_len] if ps3.lip_len else b''
    xma = struct.pack('>9I', 0x030100FF if loops else 0x03010000, 0, frames if loops else 0,
                      ps3.rate, size, frames, frames, 1, 0x01000001)
    seek = u32.pack(frames)
    lip_off = 0x38 + len(params) if lip else 0
    xma_off = align(0x38 + len(params) + len(lip), 4)
    body = struct.pack('>12I', 0xFF if ps3.flags == 1 else ps3.flags, ps3.rate, offset, size,
                       0, 0, lip_off, len(lip), xma_off, len(xma), xma_off + len(xma), len(seek))
    out = bytearray(b'TIM \0\0\0\0' + body + params + lip)
    out += bytes(xma_off - len(out)) + xma + seek
    out[4:8] = u32.pack(len(out))
    return bytes(out)


def convert_csf(rel, ps3, write_pcm, report):
    """-> 360 bank bytes, every clip a tagged stub with its frames in a sidecar."""
    pre, pghd, progs, head = parse_csf(ps3)
    if any(ps3[len(pre) + rd32(pghd, 4):head].strip(b'\0')):
        report.warn(f'{rel}: data after PGHD in the bank header is dropped')
    ordinal = 0
    prog_blobs, payload = [], bytearray()
    for prog, tims in progs:
        blob = bytearray(prog)
        for tim in tims:
            payload += bytes(align(len(payload), 0x1000) - len(payload))
            offset = len(payload)
            frames = ps3[head + tim.offset:head + tim.offset + tim.size]
            if tim.xma_off:
                # ep121 and ep146 each kept a 360 clip, XMA header and all.
                payload += frames
                t = bytearray(tim.raw)
                t[0x10:0x14] = u32.pack(offset)
                blob += t
                report.counts['audio clips already XMA'] += 1
                ordinal += 1
                continue
            if tim.size % ATRAC3_FRAME or frames[:1] != b'\xa2':
                report.warn(f'{rel}: clip {ordinal} is not mono 192 byte ATRAC3')
            n = tim.samples()
            loops = tim.flags == 1
            loop = None
            if loops:
                start = tim.loop_start // ATRAC3_FRAME * ATRAC3_SAMPLES
                end = min(n, tim.loop_end // ATRAC3_FRAME * ATRAC3_SAMPLES or n)
                loop = (start, end) if start < end else (0, n)
            tok = token(f'{rel.lower()}#{ordinal}')
            write_pcm(tok, atrac3_wav(frames, tim.rate, loop))
            payload += TAG + tok + bytes(PACKET - 16)
            blob += new_tim(tim, offset, PACKET, n, loops)
            report.counts['audio clips'] += 1
            ordinal += 1
        blob[4:8] = u32.pack(len(blob))
        prog_blobs.append(bytes(blob))
    pg = bytearray(pghd) + b''.join(prog_blobs)
    pg[4:8] = u32.pack(len(pg))
    header = bytearray(pre) + pg
    header += bytes(align(len(header), 0x1000) - len(header))
    payload += bytes(align(len(payload), 0x1000) - len(payload))
    struct.pack_into('>III', header, 4, len(header) + len(payload), len(header), len(payload))
    return bytes(header) + bytes(payload)


# ---------------------------------------------------------------------------
# Music
# ---------------------------------------------------------------------------
CXS_HEADER = 0x800
CXS_PAYLOAD = 0x1000
CXS_BLOCK = 0x10000
# Payload bytes per sample and channel, about what the 360's XMA tracks take
# (0.11 to 0.28): the guest streams the payload while the track plays.
CXS_BYTES_PER_SAMPLE = 0.25


def new_cxs(tok, channels, rate, frames, loop):
    """A .cxs header as every 360 track has it, and a payload whose first
    packet carries the tag."""
    size = align(int(frames * channels * CXS_BYTES_PER_SAMPLE), PACKET)
    blocks = (size + CXS_BLOCK - 1) // CXS_BLOCK
    loop_start, loop_end = (loop[0], align(loop[1], 512)) if loop else (0, 0)
    out = bytearray(struct.pack('>4s11I', b'CXS ', CXS_HEADER, rate, channels, frames, loop_start,
                                loop_end, blocks, CXS_PAYLOAD, size, PACKET, PACKET))
    out += bytes(CXS_HEADER - len(out))
    # The samples played by the end of each block, in whole XMA frames, the
    # last one frame past the end as on the 360. The guest does not start a
    # track whose loop end falls outside the table.
    table = [align((i + 1) * frames // blocks, 512) for i in range(blocks)]
    table[-1] = align(frames, 512) + 512
    out += b''.join(u32.pack(v) for v in table)
    out += bytes(CXS_PAYLOAD - len(out))
    out += TAG + tok + bytes(size - 16)
    return bytes(out)


def convert_cps(rel, d, write_pcm, report):
    """-> [(360 relative path, bytes)]."""
    header, channels, size, rate, loop_start, loop_end, kind = struct.unpack_from('>7I', d, 4)
    data = d[header:header + size]
    stem = os.path.splitext(os.path.basename(rel))[0].upper()
    if kind == 0:
        # Plain PCM, already big endian: the scripts ask for these as .wav.
        report.counts['music tracks as WAV'] += 1
        return [(f'sound/cxs/{stem}.wav', guest_wav(data, channels, rate))]
    target = f'sound/cxs/{stem}.cxs'
    frames = len(data) // (16 * channels) * 28
    loop = (loop_start // (16 * channels) * 28, frames) if loop_end else None
    tok = token(target.lower())
    write_pcm(tok, psx_adpcm_wav(data, channels, rate, loop))
    report.counts['music tracks as PS-ADPCM'] += 1
    return [(target, new_cxs(tok, channels, rate, frames, loop))]
