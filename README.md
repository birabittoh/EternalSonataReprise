# Eternal Sonata: Reprise

Static recompilation of **Eternal Sonata** (Xbox 360) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any Eternal Sonata code, data, or assets. You provide your own legally dumped ISO.

## Using a pre-built release

Get the latest stable build from the [Releases](../../releases/latest) page.

Nightly builds are available from [CI artifacts](https://nightly.link/birabittoh/EternalSonataReprise/workflows/ci/main?preview).

Just place the downloaded executable next to the extracted `assets` directory and run it.

## Building from scratch

### 0. Install dependencies

#### Linux (Arch/CachyOS)
```bash
paru -S clang20 cmake ninja vulkan-headers extract-xiso
```

#### Windows
```powershell
scoop install llvm cmake ninja
```

### 1. Clone

```bash
git clone https://github.com/birabittoh/EternalSonataReprise
cd EternalSonataReprise
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py
```

This downloads the latest nightly and installs it into `sdk`.

### 3. Provide your game

Extract your legally dumped ISO directly into `assets/`:

```bash
extract-xiso -d assets "Eternal Sonata.iso"
```

`assets/default.xex` must exist before running codegen.

### 4. Run codegen

```bash
sdk/bin/rexglue codegen eternalsonata_manifest.toml
```

### 5. Build

```bash
# Linux
cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH="sdk"
cmake --build --preset linux-amd64-release -- -j$(nproc)
```

```powershell
# Windows
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH="sdk"
cmake --build --preset win-amd64-release -- -j $env:NUMBER_OF_PROCESSORS
```

Symlink assets into the build output so the binary can find them:

```bash
# Linux
ln -sf "$PWD/assets" out/build/linux-amd64-release/assets
```

```powershell
# Windows
New-Item -ItemType Junction -Path out/build/win-amd64-release/assets -Target "$PWD/assets"
```

## Options

Options can be persisted by adding them to `eternalsonata.toml` next to the game executable, for example:

```toml
vulkan_device = 1 # NVIDIA GPU
user_language = 1 # English
```

### Keyboard & mouse

Keyboard and mouse controls are enabled by default. Default mapping:

| Input | Xbox 360 button |
|-------|----------------|
| WASD | Left stick |
| Mouse | Right stick (camera) |
| `1` / `2` / `3` | X / Y / B |
| Space | A |
| Left click | LT |
| Right click | RT |
| Q / E | LB / RB |
| Enter | Start |
| Backspace | Back |
| Arrow keys | D-Pad |

All bindings are overridable via `eternalsonata.toml` (or CLI flags). For example:

```toml
keybind_a = "F"
keybind_left_trigger = "LControl"
mnk_sensitivity = 0.5
```

Mouse sensitivity is controlled by `mnk_sensitivity` (default `1.0`).

### GPU selection

If you have multiple GPUs, you can force a specific one:

```bash
./eternalsonata --vulkan_device 1
```

List available devices by running the game without the flag.

### Logging

The game writes logs into the `logs` directory by default, but you can configure it.

```bash
./eternalsonata --log_file eternal.log --log_level debug
```

## Adding a hook

1. Find the guest address in `default.xex`.
2. Add to `eternalsonata_config.toml`:

   ```toml
   [functions]
   0x8XXXXXXX = {name = "MyFunction"}
   ```

3. Implement in `src/eternalsonata_hooks.cpp` (create if it doesn't exist, and add it to `CMakeLists.txt`):

   ```cpp
   void MyFunction(PPCContext& ctx, uint8_t* base) {
       // your logic
   }
   ```

4. Re-run codegen and rebuild.

## Adding a midasm hook (inline patch)

```toml
[[midasm_hook]]
address = 0x8XXXXXXX
name = "MyHook"
registers = ["r3"]
return = true
```

Implement in `src/eternalsonata_hooks.cpp`:

```cpp
void MyHook(PPCRegister& r3) {
    r3.u32 = 1;
}
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.

The recompiled game code produced at build time contains symbols and logic
from Eternal Sonata and is **not** redistributable. Do not share
`default.xex`, the `generated/` directory, or any built binary that links
against them.
