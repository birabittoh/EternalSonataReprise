<img width="829" height="375" alt="logo" src="https://github.com/user-attachments/assets/202b83f7-4be5-4add-87c0-bce06c2699e8" />

Static recompilation of **Eternal Sonata** (Xbox 360) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk/tree/thedarkness).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any copyrighted code, data, or assets. You provide your own legally dumped game.

# Get the game on [Goopie](https://goopie.xyz/#/library/eternalsonata)!

## Using a pre-built release

Get the latest stable build from the [Releases](../../releases/latest) page.

Nightly builds are available from [CI artifacts](https://nightly.link/birabittoh/EternalSonataReprise/workflows/ci/main?preview).

Just extract the archive, run the executable and it will prompt you to extract the game.

**This project can load assets from the PAL, USA and JAP releases of the game. However, building the actual executable requires the PAL version of `default.xex`.**

## Making it portable

By default saves, options and caches live in your platform user folder. To keep
everything next to the executable instead, set the root cvars in
`eternalsonata.toml` (or pass them as `--name=value`). Any root left empty falls
back to its default.

```toml
game_data_root   = "assets"    # extracted game files (default: assets)
user_data_root   = "saves"     # saves and profiles (default: Documents/<app name>)
cache_root       = "cache"     # shader cache (default: <user_data_root>/cache)
mods_data_root   = "mods"      # mod folders (default: <exe folder>/mods)
mods_dump_root   = "dumps"     # dumped textures and shaders (default: <exe folder>/dumps)
update_data_root = "update"    # extracted title update files (default: none, set by the wizard if needed)
metadata_root    = "metadata"  # game metadata (default: searched for in <game_data_root>/metadata)
```

Use absolute paths if you launch the game from a different working directory,
and launch it from the folder containing the executable when using relative
ones.

## Troubleshooting

### Black screen before the window title appears

On some Windows systems, SDL's DirectInput device scan can block while Windows
queries an unresponsive HID device. The window stays black and unresponsive
for a while and then starts normally after the HID request times out. Any HID
input device, including a mouse, can be queried during controller discovery
and delay the entire startup process.

To bypass the DirectInput scan for one launch, start the game from PowerShell:

```powershell
$env:SDL_JOYSTICK_DIRECTINPUT = "0"
.\eternalsonata.exe
```

This setting disables support for controllers that require DirectInput.

## Building from scratch

### 0. Install dependencies

#### Linux (Arch/CachyOS)
```bash
paru -S clang20 cmake ninja vulkan-headers
```

#### Windows
```powershell
scoop install llvm cmake ninja extract-xiso
```

### 1. Clone

```bash
git clone https://github.com/birabittoh/EternalSonataReprise
cd EternalSonataReprise
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py --pinned
```

### 3. Provide your game

Extract your legally dumped ISO directly into `assets/`:

```bash
extract-xiso -d assets "Eternal Sonata.iso"
```

`assets/default.xex` must exist before running codegen.

### 4. Build

Use this script:

```bash
python scripts/build.py
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [Eternal Sonata Studio](https://github.com/mimofixe/eternal-sonata-studio)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
