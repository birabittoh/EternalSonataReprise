<img width="829" height="375" alt="logo" src="https://github.com/user-attachments/assets/202b83f7-4be5-4add-87c0-bce06c2699e8" />

Static recompilation of **Eternal Sonata** (Xbox 360) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk/tree/thedarkness).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any copyrighted code, data, or assets. You provide your own legally dumped game.

# Get the game on [Goopie](https://goopie.xyz/#/library/eternalsonata)!

Join the community on [Discord](https://discord.gg/DJe2pXMH7S).

## Using a pre-built release

Get the latest stable build from the [Releases](../../releases/latest) page.

Nightly builds are available from [CI artifacts](https://nightly.link/birabittoh/EternalSonataReprise/workflows/ci/main?preview).

Just extract the archive, run the executable and it will prompt you to extract the game.

**This project can load assets from the PAL, USA and JAP releases of the game. However, building the actual executable requires the PAL version of `default.xex`.**

## Making it portable

By default saves, options and caches live in your platform user folder. To keep
them next to the executable instead, create an empty `portable.txt` beside it.

The root cvars in `eternalsonata.toml` (or `--name=value` on the command line)
override each location. Any root left empty falls back to its default.

```toml
game_data_root   = "assets"    # extracted game files (default: assets)
user_data_root   = "user"      # saves and profiles (default: Documents/eternalsonata)
cache_root       = "cache"     # shader cache (default: <user_data_root>/cache)
mods_data_root   = "mods"      # mod folders (default: mods)
mods_dump_root   = "dumps"     # dumped textures and shaders (default: dumps)
update_data_root = "update"    # extracted title update files (default: update)
```

## Troubleshooting

### Black screen before the window title appears

On some Windows systems, SDL's DirectInput device scan can block while Windows
queries an unresponsive HID device. The window stays black and unresponsive
for a while, then starts normally after the HID request times out.

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
paru -S clang20 cmake ninja vulkan-headers wayland libxcb
```

#### Linux (Debian/Ubuntu)
```bash
sudo apt install clang cmake ninja-build libvulkan-dev libwayland-dev libxcb1-dev
```

#### Windows
```powershell
scoop install llvm cmake ninja extract-xiso
```

### 1. Clone

```bash
git clone --recursive https://github.com/birabittoh/EternalSonataReprise
cd EternalSonataReprise
```

If you already cloned without `--recursive`, fetch the `plume` submodule with:

```bash
git submodule update --init --recursive
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py --pinned # Fetches the Release version
```

### 3. Provide your game

Extract your legally dumped ISO directly into `assets/`:

```bash
extract-xiso -d assets "Eternal Sonata.iso"
```

`assets/default.xex` must exist before running codegen.

### 4. Build

For an optimized release build:

```bash
python scripts/build.py --release
```

Without `--release` the script builds RelWithDebInfo.

## Contribution Policy

AI-assisted contributions will be judged exactly like human-written ones.
Basically, if I like the code you submitted, and I feel like the PR is in
line with the project's goals, I will gladly accept it, no matter how that
code was produced.

Things that make me not like your code are:

* emdashes;
* comments with useless narrative ("this used to do this, now it does this");
* Claude/Codex/Jules/whatever as a co-author in commit messages.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [Eternal Sonata Studio](https://github.com/mimofixe/eternal-sonata-studio)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
