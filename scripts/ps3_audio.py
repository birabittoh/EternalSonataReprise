"""PS3 audio -> 360 game directory, for ps3_convert.py.

PS3 sound banks hold mono ATRAC3 clips (192 byte frames) where the 360's hold
XMA2, and no XMA encoder exists. A clip whose 360 twin has the same length keeps
the 360 XMA; any other clip gets a stub payload that starts with an RXPcmSub tag,
and its PCM goes to pcm/<token>.wav in the game directory, which the host reads
and substitutes when the XMA decoder meets the tag.

PS3 scripts ask for MPxxx.cps; the shared tracks are sample identical to the
360's .cxs, which PS3 mode serves under that name. New tracks get a donor .cxs
with a tagged payload, or a plain big endian WAV where the scripts ask for .wav. See docs/ps3-assets.md.
"""
import array
import os
import shutil
import struct
import subprocess
import sys

u32 = struct.Struct('>I')

TAG = b'RXPcmSub'
PACKET = 0x800
ATRAC3_FRAME = 192
ATRAC3_SAMPLES = 1024
# A duration within this many samples counts as the same clip: ATRAC3 and XMA
# pad a clip to different frame sizes.
SAME_CLIP_SLACK = 3000

ADPCM_COEF = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


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


def pcm_wav(pcm, channels, rate, loop=None):
    """Little endian 16 bit WAV; loop is (start, end) in frames, end exclusive."""
    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    if loop:
        smpl = struct.pack('<9I', 0, 0, 1000000000 // rate, 60, 0, 0, 0, 1, 0)
        smpl += struct.pack('<6I', 0, 0, loop[0], loop[1] - 1, 0, 0)
        body += b'smpl' + struct.pack('<I', len(smpl)) + smpl
    body += b'data' + struct.pack('<I', len(pcm)) + pcm
    return b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body


def guest_wav(pcm_be, channels, rate):
    """The 360's own sound/cxs/*.wav layout: RIFF with big endian fields and samples."""
    fmt = struct.pack('>HHIIHH', 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b'fmt ' + u32.pack(len(fmt)) + fmt + b'data' + u32.pack(len(pcm_be)) + pcm_be
    return b'RIFF' + u32.pack(4 + len(body)) + b'WAVE' + body


def decode_atrac3(frames, rate):
    """Mono ATRAC3 -> little endian PCM, through ffmpeg."""
    ext = struct.pack('<HIHHHH', 1, 0, 0, 0, 1, 0)
    fmt = struct.pack('<HHIIHHH', 0x270, 1, rate, ATRAC3_FRAME * rate // ATRAC3_SAMPLES,
                      ATRAC3_FRAME, 0, len(ext)) + ext
    body = (b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt +
            b'data' + struct.pack('<I', len(frames)) + frames)
    wav = b'RIFF' + struct.pack('<I', len(body)) + body
    out = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'wav', '-i', 'pipe:0',
                          '-f', 's16le', '-ac', '1', 'pipe:1'],
                         input=wav, capture_output=True, check=True)
    return out.stdout


def decode_psx_adpcm(data, channels):
    """PS-ADPCM, channels interleaved per 16 byte frame -> little endian PCM."""
    frames = len(data) // (16 * channels)
    out = [array.array('h') for _ in range(channels)]
    for c in range(channels):
        h1 = h2 = 0
        s = out[c]
        for f in range(frames):
            o = (f * channels + c) * 16
            head = data[o]
            shift, filt = head & 15, head >> 4
            c1, c2 = ADPCM_COEF[filt] if filt < 5 else (0, 0)
            for i in range(28):
                n = data[o + 2 + (i >> 1)] >> ((i & 1) * 4) & 15
                v = ((n - 16 if n > 7 else n) << 12 >> shift) + ((h1 * c1 + h2 * c2 + 32) >> 6)
                v = -32768 if v < -32768 else 32767 if v > 32767 else v
                s.append(v)
                h2, h1 = h1, v
    mixed = array.array('h', bytes(2 * frames * 28 * channels))
    for c in range(channels):
        mixed[c::channels] = out[c]
    if sys.byteorder == 'big':
        mixed.byteswap()
    return mixed.tobytes()


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
        if self.xma_off:
            # ep171 and ep186 each hold a 20 byte header without a sample
            # count; a length no clip can be within the slack of never pairs.
            if self.xma_len < 0x18:
                return -1 << 40
            return rd32(self.raw, self.xma_off + 0x14)
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


def convert_csf(rel, ps3, x360, write_pcm, report):
    """-> 360 bank bytes. x360 is the decoded 360 bank of the same name, or None."""
    pre, pghd, progs, head = parse_csf(ps3)
    if any(ps3[len(pre) + rd32(pghd, 4):head].strip(b'\0')):
        report.warn(f'{rel}: data after PGHD in the bank header is dropped')
    twins = []
    if x360:
        for _, tims in parse_csf(x360)[2]:
            twins += tims
    ordinal = 0
    prog_blobs, payload = [], bytearray()
    for prog, tims in progs:
        blob = bytearray(prog)
        for tim in tims:
            twin = twins[ordinal] if ordinal < len(twins) else None
            payload += bytes(align(len(payload), 0x1000) - len(payload))
            offset = len(payload)
            if (twin and (0xFF if tim.flags == 1 else tim.flags) == twin.flags and
                    abs(tim.samples() - twin.samples()) <= SAME_CLIP_SLACK):
                x_head = rd32(x360, 8)
                payload += x360[x_head + twin.offset:x_head + twin.offset + twin.size]
                t = bytearray(twin.raw)
                t[0x10:0x14] = u32.pack(offset)
                blob += t
                report.counts['audio clips kept from the 360'] += 1
            else:
                frames = ps3[head + tim.offset:head + tim.offset + tim.size]
                if tim.size % ATRAC3_FRAME or frames[:1] != b'\xa2':
                    report.warn(f'{rel}: clip {ordinal} is not mono 192 byte ATRAC3')
                pcm = decode_atrac3(frames, tim.rate)
                n = len(pcm) // 2
                loops = tim.flags == 1
                loop = None
                if loops:
                    start = tim.loop_start // ATRAC3_FRAME * ATRAC3_SAMPLES
                    end = min(n, tim.loop_end // ATRAC3_FRAME * ATRAC3_SAMPLES or n)
                    loop = (start, end) if start < end else (0, n)
                tok = token(f'{rel.lower()}#{ordinal}')
                write_pcm(tok, pcm_wav(pcm, 1, tim.rate, loop))
                payload += TAG + tok + bytes(PACKET - 16)
                blob += new_tim(tim, offset, PACKET, n, loops)
                report.counts['audio clips decoded to PCM'] += 1
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


def clip_shapes(bank):
    return [(0xFF if t.flags == 1 else t.flags, t.samples())
            for _, tims in parse_csf(bank)[2] for t in tims]


def best_twin(bank, candidates, index):
    """The 360 bank whose clips line up with the most of this one's, by
    ordinal as convert_csf reuses them; ties go to the same bank index."""
    shapes = clip_shapes(bank)
    best, best_score = None, 0
    for i, x in enumerate(candidates):
        score = sum(1 for (f, n), (xf, xn) in zip(shapes, clip_shapes(x))
                    if f == xf and abs(n - xn) <= SAME_CLIP_SLACK)
        if score > best_score or (score == best_score and score and i == index):
            best, best_score = x, score
    return best


def best_group(group, candidates):
    """The 360 bank directory whose banks line up with the most of this
    one's clips, position by position; ties go to the closest durations."""
    shapes = [clip_shapes(b) for b in group]
    best, best_key = None, (0, 0)
    for x in candidates:
        score = distance = 0
        for mine, theirs in zip(shapes, (clip_shapes(b) for b in x)):
            for (f, n), (xf, xn) in zip(mine, theirs):
                if f == xf and abs(n - xn) <= SAME_CLIP_SLACK:
                    score += 1
                    distance += abs(n - xn)
        if score and (score, -distance) > best_key:
            best, best_key = x, (score, -distance)
    return best


def decoded_360_banks(base, scratch):
    """Decodes every 360 .csf with unpack_e.exe; -> lowercase rel -> path."""
    return decoded_360_files(base, scratch, '.csf')


def decoded_360_files(base, scratch, suffix):
    """Decodes every 360 file ending in suffix with unpack_e.exe; -> lowercase
    rel -> path."""
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unpack_e.exe')
    subprocess.run([exe, base, scratch, suffix], check=True, capture_output=True)
    out = {}
    for root, _, files in os.walk(scratch):
        for f in files:
            p = os.path.join(root, f)
            out[os.path.relpath(p, scratch).replace(os.sep, '/').lower()] = p
    return out


# ---------------------------------------------------------------------------
# Music
# ---------------------------------------------------------------------------
def cxs_info(d):
    return rd32(d, 0x10), rd32(d, 0x18) != 0


def convert_cps(rel, d, base, cxs_donors, write_pcm, report):
    """-> [(360 relative path, bytes)] to add; empty when the 360 track serves."""
    header, channels, size, rate, loop_start, loop_end, kind = struct.unpack_from('>7I', d, 4)
    data = d[header:header + size]
    stem = os.path.splitext(os.path.basename(rel))[0].upper()
    if kind == 0:
        # Plain PCM, already big endian: the scripts ask for these as .wav.
        target = f'sound/cxs/{stem}.wav'
        if os.path.exists(os.path.join(base, target)):
            return []
        report.counts['music tracks added as WAV'] += 1
        return [(target, guest_wav(data, channels, rate))]
    target = f'sound/cxs/{stem}.cxs'
    if os.path.exists(os.path.join(base, target)):
        return []
    pcm = decode_psx_adpcm(data, channels)
    frames = len(pcm) // (2 * channels)
    loops = loop_end != 0
    loop = None
    if loops:
        loop = (loop_start // (16 * channels) * 28, frames)
    # Any 360 track can carry the tag; the one closest in shape keeps whatever
    # the guest derives from its header nearest the truth.
    donor = min(cxs_donors, key=lambda p: (cxs_info(cxs_donors[p])[1] != loops,
                                           abs(cxs_info(cxs_donors[p])[0] - frames)))
    cxs = bytearray(cxs_donors[donor])
    tok = token(target.lower())
    at = rd32(cxs, 0x20)
    cxs[at:at + 16] = TAG + tok
    write_pcm(tok, pcm_wav(pcm, channels, rate, loop))
    report.counts['music tracks added as PCM'] += 1
    return [(target, bytes(cxs))]


def load_cxs_donors(base):
    out = {}
    folder = os.path.join(base, 'sound', 'cxs')
    for f in os.listdir(folder):
        if f.lower().endswith('.cxs'):
            with open(os.path.join(folder, f), 'rb') as fh:
                d = fh.read()
            if d[:4] == b'CXS ':
                out[f] = d
    return out


def require_ffmpeg():
    if not shutil.which('ffmpeg'):
        raise SystemExit('ffmpeg is needed to decode the PS3 ATRAC3 sound banks')
