#!/usr/bin/env python3
import sys
import os
import json
import shutil
import platform
import tempfile
import zipfile
import argparse
from urllib.request import urlopen, Request

REPO = "rexglue/rexglue-sdk"


def fetch_json(url):
    req = Request(url, headers={"User-Agent": "python"})
    with urlopen(req) as resp:
        return json.load(resp)


def detect_platform():
    os_name = platform.system()
    arch = platform.machine().lower()

    if os_name == "Linux":
        if arch in ("x86_64", "amd64"):
            return "linux-amd64"
        elif arch in ("aarch64", "arm64"):
            return "linux-arm64"
        else:
            raise RuntimeError(f"Unsupported architecture: {arch}")

    elif os_name == "Windows":
        if arch in ("x86_64", "amd64"):
            return "win-amd64"
        else:
            raise RuntimeError(f"Unsupported architecture: {arch}")

    else:
        raise RuntimeError(f"Unsupported OS: {os_name}")


def read_file_trim(path):
    with open(path, "r") as f:
        return f.read().strip()


def write_file(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)


def download_file(url, dest):
    req = Request(url, headers={"User-Agent": "python"})
    with urlopen(req) as resp, open(dest, "wb") as out:
        shutil.copyfileobj(resp, out)


def main():
    parser = argparse.ArgumentParser(
        description="Download rexglue SDK"
    )
    parser.add_argument(
        "out_dir",
        nargs="?",
        default="sdk",
        help="Output directory (default: sdk)"
    )
    parser.add_argument(
        "--pinned",
        action="store_true",
        help="Use pinned version from .sdk-version"
    )
    parser.add_argument(
        "--stable",
        action="store_true",
        help="Use latest stable (non-nightly) release"
    )

    args = parser.parse_args()

    out_dir = args.out_dir
    pinned_mode = args.pinned
    stable_mode = args.stable

    script_dir = os.path.dirname(os.path.abspath(__file__))
    version_file = os.path.normpath(os.path.join(script_dir, "../.sdk-version"))

    platform_id = detect_platform()

    if pinned_mode:
        target_tag = read_file_trim(version_file)
        print(f"Fetching pinned release {target_tag} for {platform_id}...")

        data = fetch_json(
            f"https://api.github.com/repos/{REPO}/releases/tags/{target_tag}"
        )

        asset_url = next(
            a["browser_download_url"]
            for a in data["assets"]
            if platform_id in a["name"]
        )

    elif stable_mode:
        print(f"Fetching latest stable release for {platform_id}...")

        releases = fetch_json(
            f"https://api.github.com/repos/{REPO}/releases?per_page=20"
        )

        stable = next(
            r for r in releases if not r["tag_name"].startswith("nightly-") and not r["prerelease"]
        )

        target_tag = stable["tag_name"]

        asset_url = next(
            a["browser_download_url"]
            for a in stable["assets"]
            if platform_id in a["name"]
        )

    else:
        print(f"Fetching latest nightly release for {platform_id}...")

        releases = fetch_json(
            f"https://api.github.com/repos/{REPO}/releases?per_page=20"
        )

        nightly = next(
            r for r in releases if r["tag_name"].startswith("nightly-")
        )

        target_tag = nightly["tag_name"]

        asset_url = next(
            a["browser_download_url"]
            for a in nightly["assets"]
            if platform_id in a["name"]
        )

    installed_version_file = os.path.join(out_dir, ".sdk-version")

    if os.path.exists(installed_version_file):
        installed_version = read_file_trim(installed_version_file)
        if installed_version == target_tag:
            print(f"SDK already at {target_tag}. Skipping download.")
            return

    print(f"Downloading {target_tag}...")

    with tempfile.TemporaryDirectory() as tmpdir:
        zip_path = os.path.join(tmpdir, "rexglue-sdk.zip")
        extract_dir = os.path.join(tmpdir, "extract")

        download_file(asset_url, zip_path)

        print("Extracting...")
        with zipfile.ZipFile(zip_path, "r") as z:
            z.extractall(extract_dir)

        inner_dirs = [
            d for d in os.listdir(extract_dir)
            if os.path.isdir(os.path.join(extract_dir, d))
        ]

        if not inner_dirs:
            raise RuntimeError("Unexpected archive structure")

        dest_dir = out_dir
        if os.path.exists(dest_dir):
            shutil.rmtree(dest_dir)

        shutil.move(
            os.path.join(extract_dir, inner_dirs[0]),
            dest_dir
        )

    write_file(installed_version_file, target_tag)
    print(f"SDK installed to {dest_dir} ({target_tag})")

    if platform.system() == "Linux":
        bin_path = os.path.join(dest_dir, "bin", "rexglue")
        if os.path.exists(bin_path):
            os.chmod(bin_path, os.stat(bin_path).st_mode | 0o111)
            print(f"Marked {bin_path} executable")

    if not pinned_mode:
        write_file(version_file, target_tag)
        print(f"Pinned version updated to {target_tag}")


if __name__ == "__main__":
    main()
