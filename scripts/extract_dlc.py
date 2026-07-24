#!/usr/bin/env python3
"""Extract Xbox 360 DLC content packages (STFS) in the emulator save-data
directory into per-package directories, matching the layout the game expects.

Each raw file under .../00000002/ is an STFS package (CON/LIVE/PIRS).
This script replaces each one with a directory of the same name containing the
extracted files (.mdl models, KameoDLCList.txt, etc.).

Usage:
    python scripts/extract-dlc.py [--data-dir DIR]

Without --data-dir it uses the platform default:
    Linux:   $XDG_DATA_HOME/<project_name>  (or ~/.local/share/<project_name>)
    Windows: %USERPROFILE%/Documents/<project_name>
"""
import glob
import os
import platform
import struct
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_tu

STFS_MAGICS = {b"CON ", b"LIVE", b"PIRS"}

# STFS header offsets for content metadata.
OFF_DISPLAY_NAME = 0x411  # UTF-16 BE, 128 chars (256 bytes)
OFF_TITLE_ID = 0x360      # be u32
OFF_CONTENT_TYPE = 0x344  # be u32


def load_project_name():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    manifests = glob.glob(os.path.join(root, "*_manifest.toml"))
    if len(manifests) != 1:
        print(f"error: expected exactly one *_manifest.toml, found: {manifests or 'none'}", file=sys.stderr)
        sys.exit(1)
    with open(manifests[0], "rb") as f:
        return tomllib.load(f)["project"]["name"]


def default_data_dir(project_name):
    if platform.system() == "Windows":
        return os.path.join(os.environ["USERPROFILE"], "Documents", project_name)
    xdg = os.environ.get("XDG_DATA_HOME")
    if xdg:
        return os.path.join(xdg, project_name)
    return os.path.join(os.path.expanduser("~"), ".local", "share", project_name)


def is_stfs_package(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) in STFS_MAGICS
    except (OSError, IOError):
        return False


def extract_package(pkg_path, out_dir):
    with open(pkg_path, "rb") as f:
        pkg_data = f.read()
    stfs = extract_tu.Stfs(pkg_data)
    entries = stfs.list_files()

    def full_path(i):
        parts, p = [entries[i]["name"]], entries[i]["parent"]
        while p != 0xFFFF:
            parts.append(entries[p]["name"])
            p = entries[p]["parent"]
        return os.path.join(*reversed(parts))

    count = 0
    for i, e in enumerate(entries):
        if e["is_dir"]:
            continue
        dest = os.path.join(out_dir, full_path(i))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        data = stfs.read_file(e)
        with open(dest, "wb") as f:
            f.write(data)
        count += 1

    return pkg_data, count


def write_content_header(pkg_data, pkg_name, data_dir, xuid, title_id, content_type):
    """Generate the .header file the SDK's ContentManager needs to enumerate DLC.

    The header is an XCONTENT_AGGREGATE_DATA struct (0x148 bytes) followed by a
    4-byte license_mask. We extract the display name, title ID, and content type
    from the STFS package header and set license_mask to 0x80000000 (unlocked).
    """
    header = bytearray(0x148 + 4)

    # device_id = 1 (HDD)
    struct.pack_into(">I", header, 0x000, 1)

    # content_type from STFS header
    ct = struct.unpack_from(">I", pkg_data, OFF_CONTENT_TYPE)[0]
    struct.pack_into(">I", header, 0x004, ct)

    # display_name from STFS header (already BE UTF-16)
    header[0x008:0x008 + 256] = pkg_data[OFF_DISPLAY_NAME:OFF_DISPLAY_NAME + 256]

    # file_name = the package's hex name
    fn_bytes = pkg_name.encode("ascii")[:42]
    header[0x108:0x108 + len(fn_bytes)] = fn_bytes

    # title_id from STFS header
    tid = struct.unpack_from(">I", pkg_data, OFF_TITLE_ID)[0]
    struct.pack_into(">I", header, 0x13C, tid)

    # license_mask = 0x80000000 (unlocked for all profiles)
    struct.pack_into("<I", header, 0x148, 0x80000000)

    header_dir = os.path.join(data_dir, xuid, title_id, "Headers", content_type)
    os.makedirs(header_dir, exist_ok=True)
    header_path = os.path.join(header_dir, pkg_name + ".header")
    with open(header_path, "wb") as f:
        f.write(header)
    return header_path


def main():
    import argparse
    p = argparse.ArgumentParser(description="Extract DLC STFS packages into per-package directories.")
    p.add_argument("--data-dir", metavar="DIR", help="Override the data directory (default: platform default)")
    p.add_argument("--xuid", default="0000000000000000", help="XUID directory name (default: all zeros)")
    p.add_argument("--content-type", default="00000002", help="Content type directory name (default: 00000002)")
    args = p.parse_args()

    project_name = load_project_name()
    data_dir = args.data_dir or default_data_dir(project_name)

    # Read title ID from the STFS packages themselves; use the manifest's
    # entrypoint title as a directory-scan hint.
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    manifests = glob.glob(os.path.join(root, "*_manifest.toml"))
    with open(manifests[0], "rb") as f:
        manifest = tomllib.load(f)
    xex_path = os.path.join(root, manifest["entrypoint"]["file_path"])

    # Derive title_id from the XEX if available, else fall back to scanning.
    title_id = None
    if os.path.isfile(xex_path):
        with open(xex_path, "rb") as f:
            magic = f.read(4)
            if magic == b"XEX2":
                f.seek(0x10)
                sec_off = struct.unpack(">I", f.read(4))[0]
                f.seek(sec_off)
                hdr_count = struct.unpack(">I", f.read(4))[0]
                for _ in range(hdr_count):
                    key, val = struct.unpack(">II", f.read(8))
                    if key == 0x40006:
                        title_id = f"{val:08X}"
                        break

    if not title_id:
        # Fall back: scan data_dir for title ID directories
        xuid_dir = os.path.join(data_dir, args.xuid)
        if os.path.isdir(xuid_dir):
            for d in os.listdir(xuid_dir):
                if len(d) == 8 and all(c in "0123456789ABCDEFabcdef" for c in d):
                    title_id = d.upper()
                    break

    if not title_id:
        print("error: could not determine title ID", file=sys.stderr)
        sys.exit(1)

    xuid = args.xuid
    content_type = args.content_type
    save_dir = os.path.join(data_dir, xuid, title_id, content_type)

    if not os.path.isdir(save_dir):
        print(f"error: save directory not found: {save_dir}", file=sys.stderr)
        sys.exit(1)

    packages = []
    for name in sorted(os.listdir(save_dir)):
        path = os.path.join(save_dir, name)
        if os.path.isfile(path) and is_stfs_package(path):
            packages.append((name, path))

    if not packages:
        print(f"no STFS packages found in {save_dir}")
        return

    for name, path in packages:
        out_dir = path
        tmp = path + ".extracting"
        os.rename(path, tmp)
        try:
            os.makedirs(out_dir, exist_ok=True)
            pkg_data, n = extract_package(tmp, out_dir)
            write_content_header(pkg_data, name, data_dir, xuid, title_id, content_type)
            os.remove(tmp)
            print(f"{name}: extracted {n} file(s)")
        except Exception:
            if os.path.isdir(out_dir):
                import shutil
                shutil.rmtree(out_dir)
            os.rename(tmp, path)
            raise


if __name__ == "__main__":
    main()
