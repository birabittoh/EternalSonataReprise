# Dumping and replacing assets

This covers the texture and shader dump/replace workflow in detail: the
cvars, the dump file layout, and how to turn a dumped file into a mod. For
the mod folder layout itself (`mods/<name>/textures/`, `enabled_mods`,
`mod.toml`, etc.), see [making-mods.md](making-mods.md) first; this doc
assumes you already know how asset-only mods are structured.

Texture replacement is implemented by the native renderer
(`src/gpu/native_renderer_texture.cpp`) on top of the SDK's hash index and
loader, using the same hash and file names as the SDK's own path. Shader
dumping is SDK functionality.

## Cvars

These go in `eternalsonata.toml`, or on the command line as `--name=value`:

```toml
texture_dump_enabled = false
texture_dump_skip_sizes = "640x360,1280x720"
shader_dump_enabled = false
```

| cvar | default | purpose |
|---|---|---|
| `texture_dump_enabled` | `false` | dump every texture the game draws to `dumps/textures/` |
| `texture_dump_skip_sizes` | `"640x360,1280x720"` | comma separated `WxH` list not to dump; empty dumps everything |
| `shader_dump_enabled` | `false` | dump every shader the game compiles/uses to `dumps/shaders/` |
| `shader_load_enabled` | `true` | whether mod shader overrides (DXBC/SPIR-V) are applied |
| `mods_dump_root` | unset | override where dumps are written (default: `<exe folder>/dumps`) |

## Dumping textures

With `texture_dump_enabled = true`, every colour texture the native renderer
decodes is written to `dumps/textures/` as:

```
<hash16>_<width>x<height>_<format>.png
```

`<hash16>` is a 16 hex digit XXH3 of the texture's level 0 exactly as it sits
in guest memory (tiled, big endian), so it does not depend on where the game
loaded it or on the run. It is the name a replacement takes. The PNG is the
decoded RGBA image, whatever the guest format (`DXT1`, `DXT2_3`, `DXT4_5`,
`8_8_8_8`, `5_6_5`, `1_5_5_5`).

A file is written once per hash, and a texture whose contents keep changing
(the font atlas, a readback target) stops dumping after a few versions.
Two channel normal maps are not dumped or replaced.

## Replacing textures

Put a replacement in a mod's `textures/` folder, named by the hash from the
dump:

```
mods/<name>/textures/<hash16>.png
mods/<name>/textures/<hash16>.dds    (uncompressed RGBA8 only)
```

- Any size, up to 16384 on a side, and any aspect ratio. The game keeps
  sampling with the same coordinates, so a 4x upscale is drawn at four times
  the detail with nothing else to change.
- The host builds a full mip chain from the image; mips in the file are
  ignored.
- Replacements are stored as uncompressed RGBA8: a 2048x2048 image costs
  about 21 MiB of video memory with its mips, against 4 MiB for the DXT5
  original at 512x512.
- If several enabled mods ship the same hash, the one earliest in
  `enabled_mods` wins.
- The image is read the first time its texture is drawn, which can hitch for
  a very large PNG.

## Dumping shaders

With `shader_dump_enabled = true`, `dumps/shaders/` fills up with, per
shader:

- `shader_<ucode_hash>.ucode.vert` / `.frag`: disassembled Xenos microcode,
  human-readable.
- `shader_<ucode_hash>.ucode.bin.vert` / `.frag`: the raw microcode binary.
- `shader_<hash>_<backend>.vk.bin.vert` / `.d3d.bin.vert`: the
  backend-translated shader binary actually bound to the pipeline (SPIR-V or
  DXBC depending on which graphics backend is active).

`dumps/shaders.toml` accumulates one entry per shader you've seen, keyed by
the ucode hash:

```toml
[<ucode_hash_hex>]
name = "<name>"
disabled = false
```

You can hand-edit `name` to label shaders as you identify them, and flip
`disabled = true` to force a shader off (useful for isolating which draw
call a visual effect comes from).

### Shader Debugger overlay

If you're doing this interactively rather than by hand-editing
`shaders.toml`, the in-game Shader Debugger overlay lists shaders live,
lets you toggle `disabled` per-shader, and can hot-patch a shader's
translated binary in place (recompiling from edited HLSL via D3DCompile, or
loading a raw translated binary) without restarting; useful for iterating
on a replacement before you commit it to a mod's `shaders/` folder.

## Replacing shaders

Drop the backend-translated binary into a mod's `shaders/` folder, named by
the shader hash used in the dump filename:

```
mods/<name>/shaders/<hash>_<backend>.dxbc   (D3D12)
mods/<name>/shaders/<hash>_<backend>.spv    (Vulkan)
```

The first mod root (in `enabled_mods` order) with a matching file wins.
`shader_load_enabled` must be `true` (default) for overrides to be picked
up.

Because the replacement format is the *compiled* shader binary (DXBC/SPIR-V),
not source, the practical workflow is: dump, edit the disassembly or patch
via the Shader Debugger overlay to get something working, then export/copy
the resulting binary into your mod's `shaders/` folder under the original
hash name.

## Other assets (files, sound, etc.)

Anything that isn't a texture or shader, such as game files, DLC content, or audio,
is replaced by mirroring the guest path under a mod's `game/`, `update/`,
or `dlc/<name>/` folder, as covered in
[making-mods.md](making-mods.md#asset-only-mods). There's no separate
"dump" step for these: the guest path is already whatever's on the game
disc/partition, so you get the original file straight from the extracted
game data rather than from a runtime dump.

## Packaging a dump into a mod

None of `scripts/make_mods.py` or `build.py` touch asset-only
mods; asset replacement is just files on disk read directly by the SDK's
runtime. To turn a dump into a shareable mod:

1. Copy the dumped/edited file into `mods/<name>/textures/` or
   `mods/<name>/shaders/`, keeping the hash-based filename.
2. Add a `mod.toml` (see making-mods.md) and optional `icon.png`.
3. Add `<name>` to `enabled_mods` in `eternalsonata.toml`.
4. `./eternalsonata.exe`, then press **F1** to confirm the mod loaded.

If you want to distribute it, zip the `mods/<name>/` folder the same way
`make_mods.py --package` does for code mods; asset mods just don't need
building first.
