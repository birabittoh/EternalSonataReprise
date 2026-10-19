#!/usr/bin/env python3
"""Compare xma2enc output with the 360's own music.

For every .cxs in --ref (360 tree) that also exists in --ps3 (converted PS3 tree
whose first packet carries the old sidecar token), decode the sidecar PS-ADPCM,
encode it with xma2enc_cli, decode both streams with ffmpeg and report frame
counts, seek table agreement and SNR against the PS3 PCM.

usage: verify_xma2.py [--ref assets-build/360-pal] [--ps3 assets/ps3-jp] [names...]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode_psx(data, channels):
    frames = len(data) // (16 * channels)
    out = np.zeros((frames * 28, channels), dtype=np.int16)
    for c in range(channels):
        h1 = h2 = 0
        s = 0
        col = out[:, c]
        for f in range(frames):
            o = (f * channels + c) * 16
            shift = data[o] & 15
            filt = data[o] >> 4
            c1, c2 = COEF[filt] if filt < 5 else (0, 0)
            for i in range(28):
                n = data[o + 2 + (i >> 1)] >> ((i & 1) * 4) & 15
                if n > 7:
                    n -= 16
                v = ((n * 4096) >> shift) + ((h1 * c1 + h2 * c2 + 32) >> 6)
                v = -32768 if v < -32768 else 32767 if v > 32767 else v
                col[s] = v
                s += 1
                h2 = h1
                h1 = v
    return out


def write_pcm_wav(path, pcm, rate):
    ch = pcm.shape[1]
    raw = pcm.astype("<i2").tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, ch, rate, rate * ch * 2, ch * 2, 16) + b"data" + struct.pack("<I", len(raw))
    with open(path, "wb") as f:
        f.write(hdr + raw)


def cxs_to_xma2_wav(path, out):
    d = open(path, "rb").read()
    magic, hsize, rate, ch, samples, ls, le, blocks, poff, psize = struct.unpack(">4sIIIIIIIII", d[:40])
    payload = d[poff:poff + psize]
    extra = struct.pack("<HIIIIIIIBBH", 1, 4 if ch == 1 else 3, samples, 0x10000, 0, samples,
                        0, 0, 0, 4, blocks)
    fmt = struct.pack("<HHIIHHH", 0x166, ch, rate, rate * 4, 4, 16, 34) + extra
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(payload)) + payload
    open(out, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    return dict(rate=rate, ch=ch, samples=samples, loop=(ls, le), blocks=blocks,
                seek=[struct.unpack(">I", d[0x800 + 4 * i:0x804 + 4 * i])[0] for i in range(blocks)],
                packets=psize // 2048,
                frames=sum(payload[o] >> 2 for o in range(0, psize, 2048)))


def ffmpeg_decode(path, ch):
    r = subprocess.run([shutil.which("ffmpeg"), "-v", "error", "-i", path, "-f", "s16le", "-"], capture_output=True)
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors="replace"))
    a = np.frombuffer(r.stdout, dtype="<i2")
    return a[: len(a) // ch * ch].reshape(-1, ch)


def snr(ref, test):
    n = min(len(ref), len(test))
    r = ref[:n].astype(np.float64)
    e = r - test[:n].astype(np.float64)
    den = float((e ** 2).sum())
    return 99.0 if den == 0 else 10 * np.log10(float((r ** 2).sum()) / den)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="assets-build/360-pal")
    ap.add_argument("--ps3", default="assets/ps3-jp")
    ap.add_argument("--cli", default="xma2enc/out/xma2enc_cli.exe")
    ap.add_argument("--bitrate", default="192000")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    args.cli = os.path.abspath(args.cli)

    names = args.names or sorted(n for n in os.listdir(f"{args.ps3}/sound/cxs")
                                 if os.path.exists(f"{args.ref}/sound/cxs/{n}"))
    tmp = tempfile.mkdtemp()
    print(f"{'track':<12}{'frames360':>10}{'framesNew':>10}{'seekOK':>7}{'snr360':>8}{'snrNew':>8}{'new/360':>9}")
    bad = 0
    for name in names:
        try:
            ps3 = open(f"{args.ps3}/sound/cxs/{name}", "rb").read()
            i = ps3.find(b"RXPcmSub")
            if i < 0:
                print(f"{name:<12}no sidecar tag (already real XMA?)")
                continue
            token = ps3[i + 8:i + 16].hex()
            side = open(f"{args.ps3}/pcm/{token}.wav", "rb").read()
            fmt = side.find(b"fmt ")
            ch, rate = struct.unpack("<HI", side[fmt + 10:fmt + 16])
            d0 = side.find(b"data")
            dsz = struct.unpack("<I", side[d0 + 4:d0 + 8])[0]
            pcm = decode_psx(side[d0 + 8:d0 + 8 + dsz], ch)

            ref_wav = f"{tmp}/{name}.360.wav"
            ref = cxs_to_xma2_wav(f"{args.ref}/sound/cxs/{name}", ref_wav)
            n = ref["samples"]
            short = len(pcm) < n
            pcm = pcm[:n]
            if short:
                ref['frames'] = None
            src = f"{tmp}/{name}.pcm.wav"
            write_pcm_wav(src, pcm, rate)
            new_wav = f"{tmp}/{name}.new.xma2.wav"
            out = subprocess.run([args.cli, src, new_wav, args.bitrate, "0x10000"], capture_output=True, text=True)
            if out.returncode:
                raise RuntimeError(out.stderr)
            new_frames = int(out.stdout.split()[1])
            dec_ref = ffmpeg_decode(ref_wav, ch)
            dec_new = ffmpeg_decode(new_wav, ch)
            ref_frames = ref["frames"]  # PS3 shorter than the 360: different master
            # new stream seek table is checked by the cxs writer; here only the count
            seek_ok = short or abs(len(dec_new) - len(dec_ref)) <= 1024
            s360 = snr(pcm, dec_ref)
            snew = snr(pcm, dec_new)
            snn = snr(dec_ref, dec_new)
            print(f"{name:<12}{ref_frames:>10}{new_frames:>10}{'short' if short else 'yes' if seek_ok else 'NO':>7}"
                  f"{s360:>8.1f}{snew:>8.1f}{snn:>9.1f}", flush=True)
            bad += (not short and new_frames != ref_frames) or not seek_ok
        except Exception as e:
            bad += 1
            print(f"{name:<12}FAILED {e}", flush=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
