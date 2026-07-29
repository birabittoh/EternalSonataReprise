#!/usr/bin/env python3
"""Eternal Sonata (Xbox 360) asset *re*packer - inverse of unpack_e.py.

The game reads each asset as a loose file under `assets/`, named by its
`index.vmtoc` record, and decodes it according to that record:

    [0:32]  lowercase path
    [32:36] BE u32 decoded size
    [36]    codec flag   bit0 = LZSS, bit1 = range coder

Because flag 0 means "stored, no transform" (136 shipped files already use it),
repacking does NOT require writing an encoder: emit the bytes verbatim and set
the flag to 0.  That is this script's default and it is exact by construction.

Modes:
  stored  (default)  flag 0.  Instant, always byte-exact, file gets bigger.
  lzss               flag 1.  Real compression, ~40-60% of stored on script
                     data.  Pure Python and therefore slow (~1-2 MB/min), so
                     it is best for the handful of files you actually edit.

The range-coder layers (flags 2 and 3) are deliberately NOT implemented - see
the note at the bottom of this file.

Every encode is verified by decoding it again with scripts/unpack_e.py and
comparing to the input, so a file is only written if it round-trips exactly.

usage:
  python scripts/repack_e.py <src_dir> [--assets DIR] [--mode stored|lzss]
                             [--only SUBSTR] [--in-place] [--out DIR]

  <src_dir>   tree of modified *decoded* files, laid out like the TOC paths
              (e.g. extracted/e/, after editing files in it)
  --in-place  write into --assets and patch assets/index.vmtoc directly
              (index.vmtoc is backed up to index.vmtoc.bak first)
  --out DIR   otherwise, write the new files + index.vmtoc into DIR
  --add-new   also add a TOC record for any file under <src_dir> that has no
              existing TOC entry, inserted in sorted order (see note below).

Note: a genuinely new asset path does NOT strictly need a TOC record - the
loader (sub_8210C9D8) falls back to raw/stored when the TOC lookup misses,
sizing the read off the file itself. --add-new exists for the case where you
want it to carry real size/flag bookkeeping like every other asset.
"""
import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unpack_e  # noqa: E402

RECORD = 48
MIN_MATCH = 3
MAX_MATCH = 18          # (v & 0xF) + 3
WINDOW = 4096
RING_START = 4078       # decoder's initial ring position
MAX_CANDIDATES = 64     # search cap; raise for smaller output, lower for speed


# ---------------------------------------------------------------------------
# encoders
# ---------------------------------------------------------------------------

def encode_stored(data):
    """flag 0: the decoder's `not (flag & 1)` path over a RawReader."""
    return bytes(data), 0


def encode_lzss(data):
    """flag 1: the decoder's LZSS path (sub_8210E260) over a RawReader.

    Mirrors the decoder's ring exactly: 4096 bytes, zero-filled, write cursor
    starting at 4078.  Control bytes carry 8 flags LSB-first; a set bit is a
    literal, a clear bit is a 2-byte match {off & 0xFF, ((off >> 4) & 0xF0) |
    (len - 3)}.
    """
    ring = bytearray(WINDOW)
    pos = RING_START
    out = bytearray()
    group = []          # up to 8 (is_literal, bytes) items
    flags = 0
    nbits = 0
    heads = {}          # 3-byte key -> recent output indices, newest last
    i = 0
    n = len(data)

    def flush():
        nonlocal flags, nbits, group
        if not nbits:
            return
        out.append(flags)
        for _, payload in group:
            out.extend(payload)
        flags = 0
        nbits = 0
        group = []

    def emit(is_literal, payload):
        nonlocal flags, nbits
        if is_literal:
            flags |= 1 << nbits
        group.append((is_literal, payload))
        nbits += 1
        if nbits == 8:
            flush()

    while i < n:
        best_len = 0
        best_off = 0
        if i + MIN_MATCH <= n:
            key = bytes(data[i:i + MIN_MATCH])
            cands = heads.get(key)
            if cands:
                for j in reversed(cands[-MAX_CANDIDATES:]):
                    if i - j >= WINDOW - MAX_MATCH:
                        continue
                    off = (RING_START + j) & 0xFFF
                    # Simulate the copy: the ring is written while it is read,
                    # so a match may legitimately overlap its own output.
                    ovr = {}
                    ro, rp = off, pos
                    k = 0
                    limit = min(MAX_MATCH, n - i)
                    while k < limit:
                        c = ovr.get(ro, ring[ro])
                        if c != data[i + k]:
                            break
                        ovr[rp] = c
                        ro = (ro + 1) & 0xFFF
                        rp = (rp + 1) & 0xFFF
                        k += 1
                    if k > best_len:
                        best_len, best_off = k, off
                        if k == MAX_MATCH:
                            break

        if best_len >= MIN_MATCH:
            emit(False, bytes((best_off & 0xFF,
                               ((best_off >> 4) & 0xF0) | (best_len - 3))))
            run = best_len
        else:
            emit(True, bytes((data[i],)))
            run = 1

        for k in range(run):
            idx = i + k
            if idx + MIN_MATCH <= n:
                heads.setdefault(bytes(data[idx:idx + MIN_MATCH]), []).append(idx)
            ring[pos] = data[idx]
            pos = (pos + 1) & 0xFFF
        i += run

    flush()
    return bytes(out), 1


ENCODERS = {'stored': encode_stored, 'lzss': encode_lzss}


# ---------------------------------------------------------------------------
# TOC
# ---------------------------------------------------------------------------

def read_toc(path):
    d = open(path, 'rb').read()
    if len(d) % RECORD:
        raise SystemExit('%s is not a multiple of %d bytes' % (path, RECORD))
    recs = []
    for i in range(len(d) // RECORD):
        e = bytearray(d[i * RECORD:(i + 1) * RECORD])
        name = e[0:32].split(b'\x00')[0].decode('latin1')
        recs.append([name, e])
    return recs


def check_e_header(name, data):
    """.e files carry their own total size at +0x0C; keep it consistent."""
    if not name.lower().endswith('.e') or len(data) < 16:
        return None
    magic, = struct.unpack_from('>I', data, 0)
    if magic not in (0x180, 0x181):
        return 'magic is 0x%x, expected 0x180/0x181' % magic
    total, = struct.unpack_from('>I', data, 12)
    if total != len(data):
        return ('header total size 0x%x != file length 0x%x '
                '(patch offset 0x0C after editing)' % (total, len(data)))
    return None


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('src_dir', help='tree of modified decoded files')
    ap.add_argument('--assets', default='assets', help='original assets dir')
    ap.add_argument('--mode', default='stored', choices=sorted(ENCODERS))
    ap.add_argument('--only', default='', help='only paths containing this substring')
    ap.add_argument('--in-place', action='store_true',
                    help='write into --assets and patch its index.vmtoc')
    ap.add_argument('--out', default='repacked', help='output dir (without --in-place)')
    ap.add_argument('--add-new', action='store_true',
                    help='add TOC records for files in src_dir with no existing entry')
    args = ap.parse_args(argv)

    toc_path = os.path.join(args.assets, 'index.vmtoc')
    recs = read_toc(toc_path)
    dest_root = args.assets if args.in_place else args.out
    encode = ENCODERS[args.mode]

    changed = 0
    skipped = 0
    total_in = 0
    total_out = 0

    for name, rec in recs:
        if not name:
            continue
        norm = name.lower().replace('\\', '/')
        if args.only and args.only.lower().replace('\\', '/') not in norm:
            continue
        src = os.path.join(args.src_dir, name.replace('\\', '/'))
        if not os.path.isfile(src):
            continue

        data = open(src, 'rb').read()
        warn = check_e_header(name, data)
        if warn:
            print('WARN     %s: %s' % (name, warn))

        blob, flag = encode(data)

        # Round-trip through the decoder before writing anything.
        back = unpack_e.unpack(blob, len(data), flag)
        if back != data:
            print('FAIL     %s: round-trip mismatch (%d in, %d back)'
                  % (name, len(data), len(back)))
            skipped += 1
            continue

        dst = os.path.join(dest_root, name.replace('\\', '/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(blob)

        struct.pack_into('>I', rec, 32, len(data))
        rec[36] = flag
        changed += 1
        total_in += len(data)
        total_out += len(blob)
        print('ok       %-44s %8d -> %8d  flag=%d' % (name, len(data), len(blob), flag))

    if args.add_new:
        existing = {n.lower().replace('\\', '/') for n, _ in recs if n}
        for root, _, files in os.walk(args.src_dir):
            for fn in files:
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, args.src_dir).replace(os.sep, '\\')
                norm = rel.lower().replace('\\', '/')
                if args.only and args.only.lower().replace('\\', '/') not in norm:
                    continue
                if norm in existing:
                    continue

                name = rel.lower()
                if len(name) > 31:
                    print('SKIP     %s: path too long for the 32-byte TOC field' % rel)
                    skipped += 1
                    continue

                data = open(full, 'rb').read()
                warn = check_e_header(name, data)
                if warn:
                    print('WARN     %s: %s' % (name, warn))

                blob, flag = encode(data)
                back = unpack_e.unpack(blob, len(data), flag)
                if back != data:
                    print('FAIL     %s: round-trip mismatch (new entry)' % name)
                    skipped += 1
                    continue

                dst = os.path.join(dest_root, name.replace('\\', '/'))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, 'wb') as f:
                    f.write(blob)

                rec = bytearray(RECORD)
                rec[0:len(name)] = name.encode('latin1')
                struct.pack_into('>I', rec, 32, len(data))
                rec[36] = flag
                recs.append([name, rec])
                existing.add(norm)
                changed += 1
                total_in += len(data)
                total_out += len(blob)
                print('new      %-44s %8d -> %8d  flag=%d' % (name, len(data), len(blob), flag))

    if not changed:
        print('nothing to do: no TOC entry had a matching file under %s' % args.src_dir)
        return 1

    if args.in_place:
        backup = toc_path + '.bak'
        if not os.path.exists(backup):
            shutil.copy2(toc_path, backup)
            print('backed up %s -> %s' % (toc_path, backup))
        new_toc_path = toc_path
    else:
        new_toc_path = os.path.join(dest_root, 'index.vmtoc')
        os.makedirs(dest_root, exist_ok=True)

    # The device does a binary search over this array by lowercased path, so
    # inserted records must keep the whole table sorted.
    recs.sort(key=lambda nr: nr[0].lower())

    with open(new_toc_path, 'wb') as f:
        for _, rec in recs:
            f.write(bytes(rec))

    print('\n%d file(s) repacked (%s), %d skipped, %d -> %d bytes'
          % (changed, args.mode, skipped, total_in, total_out))
    print('wrote %s' % new_toc_path)
    if not args.in_place:
        print('copy %s over %s to apply' % (dest_root, args.assets))
    return 0 if not skipped else 1


# ---------------------------------------------------------------------------
# Why flags 2 and 3 (range coder) are not implemented
# ---------------------------------------------------------------------------
# Nothing needs them: the flag is per-file in the TOC, so a repacked file can
# use flag 0 or 1 even though it shipped as flag 3.  Writing a matching encoder
# would mean inverting sub_8210E0F8's carry-less range coder, whose two
# normalisation loops (`((low + range) ^ low) < 0x1000000` and the
# `range < 0x2000` underflow rescale) have to be mirrored exactly or the output
# desynchronises.  On top of that the frequency table is one *byte* per symbol
# with the total bounded by the decoder's LUT budget (ctx+816..ctx+9020 = 8204
# entries), so real symbol counts must be requantised into 1..255 without
# zeroing any symbol that occurs.  That is a genuine piece of work for no
# benefit beyond distribution size.

if __name__ == '__main__':
    sys.exit(main())
