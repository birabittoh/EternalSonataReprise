#!/usr/bin/env python3
"""Build the guest shader pack: default.xex -> HLSL -> DXIL/SPIR-V, offline.

This is the build-time driver that ties together the three pieces that already
existed as separate manual steps:

    extract_shaders.parse_blob   the 260 shader containers inside default.xex
    xenos_hlsl                   microcode -> HLSL
    dxc                          HLSL -> DXIL and SPIR-V

Nothing here runs at game time. That is the whole point of the native renderer:
the guest's shader inventory is a closed set compiled into the executable, so
translating it is a finite offline problem rather than a runtime one.

Output is a single pack file, `guest_shaders.bin`, deployed next to the exe and
read once at startup by src/guest_shaders.cpp. It is a file rather than an
embedded byte array because the compiled blobs come to a few megabytes per
format, and turning that into C++ array initialisers costs far more build time
than reading it back does at runtime.

The pack is indexed by *guest table slot*, not by position: the game binds
shaders out of the 256 entry tables at dword_824BBEE8 and dword_824BC2E8, and
the device mirror resolves a bound shader object back to its slot. So a lookup
at runtime is a direct index, with an empty entry meaning "the game never had a
shader there".

Alongside the blobs the pack carries the two signature tables the host needs to
build pipelines without inspecting DXIL:

  * per vertex shader, the vertex input signature (D3DDECLUSAGE, usage index)
    read from the container's own vertex fetch patch table. This is what a host
    input layout is built from, and it is why vfetch does not have to be
    translated as an instruction.
  * per shader, the interpolator semantic keys. Both sides declare
    `TEXCOORD<key>`, so the host linker does the register renumbering that
    sub_822679A8 does at draw time. D3D12 requires a pixel shader's inputs to be
    a subset of the bound vertex shader's outputs, so a pipeline builder needs
    the key sets to check a (VS, PS) pair before trying to create it.

Usage:
    python scripts/gen-guest-shaders.py --dxc <path/to/dxc> --out <dir>
    python scripts/gen-guest-shaders.py --dxc <path> --out <dir> --formats dxil
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_shaders as X  # noqa: E402
import xenos_hlsl as H  # noqa: E402

PACK_MAGIC = b"ESGS"
PACK_VERSION = 5

# Both guest tables are 256 entries; the pack holds vertex slots first, then
# pixel slots, so an entry index is `kind_base + slot`.
SLOTS = X.MAX_INDEX + 1

FLAG_POINT_SIZE = 1 << 0
FLAG_HAS_CUBE = 1 << 1

# Shader model 6.0 is the floor Plume's D3D12 and Vulkan backends both accept,
# and nothing emitted here needs anything newer.
PROFILE = {"vs": "vs_6_0", "ps": "ps_6_0"}

# Matches Plume's own helper (examples/cmake/modules/PlumeDXC.cmake) so the
# guest shaders and the overlay shaders are compiled the same way. -fvk-invert-y
# on vertex shaders lets the HLSL be written once for the D3D clip space
# convention, with the Vulkan flip handled at compile time.
DXIL_OPTS = ["-Wno-ignored-attributes"]
SPIRV_OPTS = ["-spirv", "-fspv-target-env=vulkan1.0", "-fvk-use-dx-layout"]

# The reflection and debug sections are dead weight here: the pack carries the
# signature tables the host needs, decoded from the guest container rather than
# recovered from the compiled blob.
STRIP_OPTS = ["-Qstrip_reflect", "-Qstrip_debug"]


def write_if_different(path, data):
    """Avoid touching an unchanged file, so DXC can be skipped on rebuild."""
    if os.path.exists(path):
        with open(path, "rb") as handle:
            if handle.read() == data:
                return False
    with open(path, "wb") as handle:
        handle.write(data)
    return True


class Translated:
    """One guest shader, translated but not yet compiled."""

    def __init__(self, kind, slot, name, emitted):
        self.kind = kind
        self.slot = slot
        self.name = name
        self.inputs = [(e["usage"], e["usage_index"]) for e in emitted.inputs] \
            if kind == "vs" else []
        self.keys = sorted(e["key"] for e in emitted.interpolators)
        self.texture_mask = 0
        self.flags = 0
        # 16 floats for constants 252..255, or None. See xenos_ucode's note
        # above PREFIX_CANDIDATES for why this has to travel with the shader.
        self.literals = emitted.literals
        if kind == "ps":
            for texture_slot, kind_name in emitted.textures.items():
                self.texture_mask |= 1 << texture_slot
                if kind_name == "TextureCube":
                    self.flags |= FLAG_HAS_CUBE
        elif H.EXPORT_POINT_SIZE in emitted.exports:
            self.flags |= FLAG_POINT_SIZE
        self.blobs = {}  # format -> bytes


def translate(xex, hlsl_dir):
    """Walk the blob, translate every container, write the HLSL sources."""
    image = X.XexImage.load(xex)
    shaders, _ = X.parse_blob(image)
    os.makedirs(hlsl_dir, exist_ok=True)

    # Decode everything first, because the interpolator layout is a property of
    # the title rather than of a shader: a pixel shader has to declare its
    # inputs in the same registers the vertex shader it is paired with writes,
    # and any (VS, PS) pair the game forms has to link. One ascending list of
    # every semantic key in the blob gives every shader the same register
    # assignment. See Shader.interpolator_layout for what goes wrong without it.
    decoded = []
    keys = set()
    for shader in shaders:
        kind = H.PixelShader if shader.kind == "ps" else H.VertexShader
        emitted = kind(shader.name, shader.microcode, shader.header)
        if emitted.unsupported:
            raise SystemExit(
                "%s does not translate: %s"
                % (shader.name, "; ".join(emitted.unsupported)))
        keys.update(entry["key"] for entry in emitted.interpolators)
        decoded.append((shader, emitted))

    layout = sorted(keys)
    out = []
    for shader, emitted in decoded:
        emitted.interpolator_layout = layout
        text = emitted.emit(shader.name).encode("utf-8")
        path = os.path.join(hlsl_dir, shader.name + ".hlsl")
        write_if_different(path, text)
        entry = Translated(shader.kind, shader.index, shader.name, emitted)
        entry.source = path
        out.append(entry)
    return out


def compile_one(dxc_command, entry, fmt, out_dir):
    source = entry.source
    output = os.path.join(out_dir, "%s.%s" % (entry.name, fmt))
    # The HLSL source is only rewritten when it changes, so an output that is
    # newer than its source is still valid and DXC can be skipped entirely.
    if os.path.exists(output) and \
            os.path.getmtime(output) >= os.path.getmtime(source):
        with open(output, "rb") as handle:
            return handle.read()

    options = list(DXIL_OPTS if fmt == "dxil" else SPIRV_OPTS)
    if fmt == "dxil":
        options += STRIP_OPTS
    if fmt == "spirv" and entry.kind == "vs":
        options.append("-fvk-invert-y")

    command = list(dxc_command) + options + [
        "-E", "main", "-T", PROFILE[entry.kind], "-Fo", output, source]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(output):
        raise SystemExit("dxc failed on %s (%s):\n%s%s"
                         % (entry.name, fmt, result.stdout, result.stderr))
    with open(output, "rb") as handle:
        return handle.read()


def compile_all(dxc_command, entries, formats, out_root, jobs):
    work = []
    for fmt in formats:
        out_dir = os.path.join(out_root, fmt)
        os.makedirs(out_dir, exist_ok=True)
        for entry in entries:
            work.append((entry, fmt, out_dir))

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {
            pool.submit(compile_one, dxc_command, entry, fmt, out_dir):
                (entry, fmt)
            for entry, fmt, out_dir in work
        }
        for future in concurrent.futures.as_completed(futures):
            entry, fmt = futures[future]
            entry.blobs[fmt] = future.result()


def pack(entries, formats):
    """Lay the pack out: header, entry table, signature tables, then the blobs.

    Blob offsets are relative to the start of the blob section, so the loader
    can keep one pointer to it and index from there.
    """
    blob = bytearray()
    inputs = bytearray()
    keys = bytearray()

    # The literal constant pools, deduplicated: 64 bytes each, host order, and
    # a shader without one indexes the sentinel below. Entry 0 is reserved for
    # "no pool" so the per entry field can stay a plain index.
    literals = bytearray(64)
    literal_index = {}

    def place_literals(values):
        if values is None:
            return 0
        data = struct.pack("<16f", *values)
        existing = literal_index.get(data)
        if existing is not None:
            return existing
        where = len(literals) // 64
        literals.extend(data)
        literal_index[data] = where
        return where

    # Deduplicating identical blobs is free here and worth doing: the same
    # microcode compiled for two slots is byte identical.
    seen = {}

    def place(data):
        if not data:
            return (0, 0)
        existing = seen.get(data)
        if existing is not None:
            return existing
        # 4 byte alignment keeps every blob usable as a dword stream, which is
        # what both DXIL and SPIR-V are.
        while len(blob) % 4:
            blob.append(0)
        where = (len(blob), len(data))
        blob.extend(data)
        seen[data] = where
        return where

    table = [None] * (SLOTS * 2)
    for entry in entries:
        index = (0 if entry.kind == "vs" else SLOTS) + entry.slot
        if table[index] is not None:
            raise SystemExit("two shaders claim %s slot %d"
                             % (entry.kind, entry.slot))
        dxil = place(entry.blobs.get("dxil", b""))
        spirv = place(entry.blobs.get("spirv", b""))

        input_offset = len(inputs) // 2
        for usage, usage_index in entry.inputs:
            inputs.extend((usage, usage_index))
        key_offset = len(keys)
        keys.extend(entry.keys)

        table[index] = struct.pack(
            "<IIIIIHBBHBBH",
            dxil[0], dxil[1], spirv[0], spirv[1], entry.texture_mask,
            input_offset, len(entry.inputs), entry.flags,
            key_offset, len(entry.keys), 0, place_literals(entry.literals))

    empty = struct.pack("<IIIIIHBBHBBH", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    body = b"".join(slot if slot is not None else empty for slot in table)

    header = struct.pack("<4sIIIIII", PACK_MAGIC, PACK_VERSION, SLOTS,
                         len(inputs) // 2, len(keys), len(literals) // 64,
                         len(blob))
    return (header + body + bytes(inputs) + bytes(keys) + bytes(literals)
            + bytes(blob)), len(blob)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--xex", default=os.path.join(root, "assets/default.xex"))
    parser.add_argument("--out", default=os.path.join(root, "out/guest_shaders"),
                        help="working directory for the .hlsl and compiled blobs")
    parser.add_argument("--pack", help="pack file to write "
                                       "(default <out>/guest_shaders.bin)")
    parser.add_argument("--dxc", required=True, help="dxc executable")
    parser.add_argument("--dxc-lib-dir",
                        help="directory holding libdxcompiler, if it is not "
                             "next to the executable (Linux and macOS)")
    parser.add_argument("--formats", default="dxil,spirv",
                        help="comma separated: dxil, spirv")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    formats = [f.strip() for f in args.formats.split(",") if f.strip()]
    for fmt in formats:
        if fmt not in ("dxil", "spirv"):
            parser.error("unknown format %r" % fmt)

    # DXC ships as a thin executable next to a shared library it dlopens by
    # plain name, so on Linux and macOS the loader has to be pointed at it.
    environment_prefix = []
    if args.dxc_lib_dir:
        variable = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" \
            else "LD_LIBRARY_PATH"
        os.environ[variable] = args.dxc_lib_dir + os.pathsep + \
            os.environ.get(variable, "")
    # Normalised because CreateProcess on Windows does not accept a relative
    # path with forward slashes, which is exactly what a shell hands over.
    dxc_command = environment_prefix + [os.path.normpath(os.path.abspath(args.dxc))]

    entries = translate(args.xex, os.path.join(args.out, "hlsl"))
    compile_all(dxc_command, entries, formats, args.out, args.jobs)
    data, blob_bytes = pack(entries, formats)

    pack_path = args.pack or os.path.join(args.out, "guest_shaders.bin")
    os.makedirs(os.path.dirname(os.path.abspath(pack_path)), exist_ok=True)
    write_if_different(pack_path, data)

    vertex = sum(1 for e in entries if e.kind == "vs")
    print("guest shaders: %d vertex, %d pixel, formats %s, %d blob bytes, "
          "pack %d bytes -> %s"
          % (vertex, len(entries) - vertex, "+".join(formats), blob_bytes,
             len(data), pack_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
