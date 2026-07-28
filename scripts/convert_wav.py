#!/usr/bin/env python3
"""
Convert Xbox 360 (big-endian) WAV files to standard little-endian WAV.
These WAV files have BE headers and BE 16-bit PCM samples.
"""
import struct
import os
from pathlib import Path

SRC_DIR = Path(__file__).parent.parent / "assets" / "sound" / "cxs"
OUT_DIR = Path(__file__).parent / "wav_le"
OUT_DIR.mkdir(exist_ok=True)


def swap16(data: bytes) -> bytes:
    """Byte-swap every 16-bit sample."""
    out = bytearray(len(data))
    for i in range(0, len(data), 2):
        out[i] = data[i + 1]
        out[i + 1] = data[i]
    return bytes(out)


def convert_wav(src: Path, dst: Path):
    data = src.read_bytes()
    if len(data) < 44:
        print(f"  SKIP {src.name}: too small ({len(data)} bytes)")
        return False

    # Validate BE magic
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        print(f"  SKIP {src.name}: not a WAV file")
        return False

    riff_size = struct.unpack_from('>I', data, 4)[0]
    channels = struct.unpack_from('>H', data, 22)[0]
    sample_rate = struct.unpack_from('>I', data, 24)[0]
    byte_rate = struct.unpack_from('>I', data, 28)[0]
    block_align = struct.unpack_from('>H', data, 32)[0]
    bits = struct.unpack_from('>H', data, 34)[0]
    data_size = struct.unpack_from('>I', data, 40)[0]

    print(f"  {src.name}: {channels}ch {sample_rate}Hz {bits}bit, "
          f"data={data_size:,} bytes ({data_size/sample_rate/channels/(bits//8):.1f}s)")

    # Build LE WAV
    out = bytearray()

    # RIFF header (LE)
    # Standard WAV: RIFF size = total file size - 8
    # BE WAV: RIFF size = total file size (off by 8)
    le_riff_size = len(data) - 8
    out += b'RIFF'
    out += struct.pack('<I', le_riff_size)
    out += b'WAVE'

    # Find and convert chunks
    offset = 12
    while offset + 8 <= len(data):
        chunk_id = data[offset:offset + 4]
        chunk_size = struct.unpack_from('>I', data, offset + 4)[0]

        if chunk_id == b'fmt ':
            # Convert fmt chunk
            fmt_data = data[offset + 8:offset + 8 + chunk_size]
            if len(fmt_data) >= 16:
                out += b'fmt '
                out += struct.pack('<I', chunk_size)  # fmt size stays same
                audio_fmt = struct.unpack_from('>H', fmt_data, 0)[0]
                out += struct.pack('<H', audio_fmt)
                out += struct.pack('<H', channels)
                out += struct.pack('<I', sample_rate)
                out += struct.pack('<I', byte_rate)
                out += struct.pack('<H', block_align)
                out += struct.pack('<H', bits)
                # Extra format bytes
                if len(fmt_data) > 16:
                    out += fmt_data[16:]
            else:
                out += data[offset + 8:offset + 8 + 8 + chunk_size]
        elif chunk_id == b'data':
            # Convert PCM data (byte-swap every 16-bit sample)
            pcm = data[offset + 8:offset + 8 + chunk_size]
            le_pcm = swap16(pcm)
            out += b'data'
            out += struct.pack('<I', chunk_size)
            out += le_pcm
        else:
            # Pass through other chunks with LE size
            out += chunk_id
            out += struct.pack('<I', chunk_size)
            out += data[offset + 8:offset + 8 + chunk_size]

        if chunk_size == 0:
            break
        offset += 8 + chunk_size
        # Align to 2 bytes (WAV chunk alignment)
        if chunk_size % 2 != 0:
            offset += 1

    dst.write_bytes(bytes(out))
    print(f"    -> {dst.name} ({len(out):,} bytes)")
    return True


def main():
    wav_files = sorted(SRC_DIR.glob("*.wav"))
    print(f"Converting {len(wav_files)} BE WAV files to LE...\n")

    converted = 0
    for src in wav_files:
        dst = OUT_DIR / src.name
        if convert_wav(src, dst):
            converted += 1

    print(f"\nDone: {converted}/{len(wav_files)} files converted to {OUT_DIR}")


if __name__ == '__main__':
    main()
