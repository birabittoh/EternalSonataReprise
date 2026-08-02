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

**This project is built and tested against the PAL version of the game.**

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

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
