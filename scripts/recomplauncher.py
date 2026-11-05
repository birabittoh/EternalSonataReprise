"""Stage the manifest with the SDK's platform-specific user-data paths."""
import json
from pathlib import Path


def stage_manifest(destination, is_windows, is_macos=False):
    source = Path(__file__).resolve().parent.parent / "recomplauncher.json"
    text = source.read_text(encoding="utf-8")
    if not is_windows:
        # GetUserFolder() uses XDG_DATA_HOME, falling back to ~/.local/share.
        root = "{home}/.local/share" if is_macos else "{local}"
        text = text.replace("{documents}/eternalsonata", root + "/eternalsonata")
    manifest = json.loads(text)
    if is_macos:
        manifest["folders"]["logs"] = [
            {"path": "{home}/Library/Application Support/eternalsonata/logs"}]
    if is_windows:
        manifest["settings"].append({
            "id": "graphics_api", "label": "Graphics API",
            "choices": [
                {"label": "Automatic", "set": {"gpu_backend": '"any"'}},
                {"label": "Direct3D 12", "set": {"gpu_backend": '"d3d12"'}},
                {"label": "Vulkan", "set": {"gpu_backend": '"vulkan"'}},
            ],
            "defaults": {"gpu_backend": '"any"'},
        })
    else:
        manifest["looks"] = {"graphics": "vulkan"}
    Path(destination, "recomplauncher.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
