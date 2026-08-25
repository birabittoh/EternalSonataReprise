#!/usr/bin/env python3
"""Decrypt and decompress an Xbox 360 XEX2 into a flat, virtual-address addressable image.

`default.xex` is stored encrypted (AES-128-CBC under a title key that is itself
wrapped with the retail key) and "basic" compressed, which is not a real codec:
it is a run of (raw_size, zero_size) pairs describing where the linker elided
zero pages. That means no LZX decoder is needed.

Used by extract_shaders.py. Import `XexImage` and read by guest address:

    img = XexImage.load("assets/default.xex")
    img.read(0x8238EC50, 16)
"""
import struct
import sys

# Wrapping keys for the title key held in the security info block.
RETAIL_KEY = bytes(
    [0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
     0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91]
)
DEVKIT_KEY = bytes(16)

XEX_FILE_FORMAT_INFO = 0x000003FF

# Offsets into the xex2_security_info struct.
SEC_LOAD_ADDRESS = 0x110
SEC_FILE_KEY = 0x150


def _aes_ecb_decrypt(key, data):
    try:
        from Crypto.Cipher import AES
        return AES.new(key, AES.MODE_ECB).decrypt(data)
    except ImportError:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        dec = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
        return dec.update(data) + dec.finalize()


def _aes_cbc_decryptor(key):
    """Return a stateful `update(bytes) -> bytes` CBC decryptor with a zero IV.

    The CBC chain runs continuously across the whole compressed stream, not per
    block, so the caller must feed it in order and keep one instance.
    """
    iv = bytes(16)
    try:
        from Crypto.Cipher import AES
        return AES.new(key, AES.MODE_CBC, iv).decrypt
    except ImportError:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        return dec.update


class XexImage:
    def __init__(self, data, base):
        self.data = data
        self.base = base

    @property
    def end(self):
        return self.base + len(self.data)

    def contains(self, addr, size=1):
        return self.base <= addr and addr + size <= self.end

    def read(self, addr, size):
        if not self.contains(addr, size):
            raise ValueError(
                f"0x{addr:08X}+{size} outside image "
                f"0x{self.base:08X}..0x{self.end:08X}"
            )
        off = addr - self.base
        return self.data[off:off + size]

    def u32(self, addr):
        return struct.unpack_from(">I", self.read(addr, 4))[0]

    @classmethod
    def load(cls, path):
        raw = open(path, "rb").read()
        if raw[:4] != b"XEX2":
            raise ValueError(f"{path}: not a XEX2 file")

        pe_data_offset, = struct.unpack_from(">I", raw, 8)
        security_offset, = struct.unpack_from(">I", raw, 16)
        header_count, = struct.unpack_from(">I", raw, 20)

        fmt_off = None
        for i in range(header_count):
            key, value = struct.unpack_from(">II", raw, 24 + 8 * i)
            if key == XEX_FILE_FORMAT_INFO:
                fmt_off = value
        if fmt_off is None:
            raise ValueError("no XEX_FILE_FORMAT_INFO header")

        info_size, encryption, compression = struct.unpack_from(">IHH", raw, fmt_off)
        if compression not in (0, 1):
            raise ValueError(
                f"compression type {compression} unsupported "
                "(only none/basic; LZX would need a real decoder)"
            )

        load_address, = struct.unpack_from(">I", raw, security_offset + SEC_LOAD_ADDRESS)
        wrapped_key = raw[security_offset + SEC_FILE_KEY:
                          security_offset + SEC_FILE_KEY + 16]

        body = raw[pe_data_offset:]

        # Basic compression: (raw_size, zero_size) pairs following the 8 byte
        # info header. Anything else is a single implicit whole-body block.
        if compression == 1:
            pair_count = (info_size - 8) // 8
            blocks = [
                struct.unpack_from(">II", raw, fmt_off + 8 + 8 * i)
                for i in range(pair_count)
            ]
        else:
            blocks = [(len(body), 0)]

        last_error = None
        for key_name, wrapping_key in (("retail", RETAIL_KEY), ("devkit", DEVKIT_KEY)):
            try:
                image = cls._build(body, blocks, encryption, wrapped_key, wrapping_key)
            except Exception as exc:  # pragma: no cover - defensive
                last_error = exc
                continue
            if image[:2] == b"MZ":
                return cls(image, load_address)
            last_error = ValueError(f"{key_name} key produced no MZ header")
        raise ValueError(f"could not decrypt {path}: {last_error}")

    @staticmethod
    def _build(body, blocks, encryption, wrapped_key, wrapping_key):
        if encryption:
            title_key = _aes_ecb_decrypt(wrapping_key, wrapped_key)
            decrypt = _aes_cbc_decryptor(title_key)
        else:
            decrypt = lambda b: b  # noqa: E731

        out = bytearray()
        pos = 0
        for raw_size, zero_size in blocks:
            chunk = body[pos:pos + raw_size]
            pos += raw_size
            if encryption:
                # CBC needs whole blocks; the final chunk can be short.
                trimmed = len(chunk) & ~0xF
                chunk = decrypt(chunk[:trimmed]) + chunk[trimmed:]
            out += chunk
            out += bytes(zero_size)
        return bytes(out)


def main():
    if len(sys.argv) < 2:
        print("usage: xex_image.py <default.xex> [out.bin]", file=sys.stderr)
        return 2
    img = XexImage.load(sys.argv[1])
    print(f"image base 0x{img.base:08X} size 0x{len(img.data):X} "
          f"end 0x{img.end:08X}")
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(img.data)
        print(f"wrote {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
