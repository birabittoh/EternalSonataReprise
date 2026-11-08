# RecompLauncher integration

The desktop release includes a format-1 manifest next to the executable.
RecompLauncher uses it instead of its built-in compatibility profile. This
includes direct startup, settings, a headless preparation step, known-disc
checksums, save locations and folder actions. Android uses its existing flow.

## Preparing a copy

Select an Xbox 360 disc image, a PS3 disc image, or an extracted game folder.
Europe, North America and Japan use the same identification and installation
pipeline as the game's own start screen. PS3 archives are unpacked and converted;
Xbox 360 regional data is patched when needed. The pipeline identifies the
executable against the supported releases before installing. A random folder
or disc is rejected. The manifest deliberately leaves `game.check` empty: the
launcher cannot express Xbox 360 **or** PS3 format checks in one profile.

An encrypted PS3 image needs its matching `.dkey` or `.key` beside the image,
unless the installer already knows its key. Headless setup never opens a key
picker. A folder converted by an older version must be replaced from the
original dump rather than silently accepted.

The launcher invokes:

```text
eternalsonata.exe --prepare-content --source <disc-or-folder> --destination <install>/assets/recomplauncher
```

On Linux/macOS the executable is `eternalsonata`. This runs before SDL or the
graphics runtime initializes. It writes flushed `progress <0..1> <message>`
lines to stdout, diagnostics to stderr, and exits with 0 on success, 1 on an
installation failure, or 2 on invalid arguments. Normal game arguments still
use the SDK's original entry point.

Setup needs room for the complete new copy while the previous copy remains
available. Extracted sources are copied first, so conversion and patching
never change the player's files. Installation runs in a sibling `.preparing`
directory, then replaces only a destination carrying this tool's ownership
marker. Other existing folders are refused. Failed installs preserve the old
copy and remove only the transaction created by that invocation.

Setup holds a lock on its `.preparing` directory, which the OS releases if the
launcher cancels it or it times out. The next setup then removes that directory,
first moving its `old` copy back to the destination when the destination is
absent. A `.preparing` directory still locked by a running setup, or one without
the ownership marker, is refused and left alone. A failed rollback prints the
location of the previous files. Do not run setup while the game is using the
prepared directory.

Play passes `--game_version=<prepared-folder> --show_start_screen=false`.
Command line cvars outrank `settings.toml` and are never saved, so this selects
the prepared version for that session without replacing the standalone library
selection.
Fullscreen and button prompts follow the game's existing settings and input
handling. Explicit in-game requests to revisit the start screen or resolve
blocking mods still work.

## Settings and saves

The launcher edits the existing friendly `settings.toml`, preserving unrelated
settings. Controls cover fullscreen, draw scale, frame rate, V-Sync, MSAA, FXAA,
texture filtering and intro logos. Windows release manifests also expose the
Direct3D 12/Vulkan backend choice. No unsupported Xenia graphics options are
advertised for the native renderer.

User data uses Documents/eternalsonata on Windows and the SDK's data-home
directory on Linux/macOS (`XDG_DATA_HOME/eternalsonata`, default
`~/.local/share/eternalsonata`). `portable.txt` uses `<install>/user`.
`scripts/recomplauncher.py` stages the appropriate paths when packaging.
The manifest lists these user directories for backups; RecompLauncher's save
filter excludes caches, mods, logs and other generated assets. Custom root cvars
remain supported by the game, but require matching paths in a custom manifest.
macOS bundles carry the manifest in `Contents/MacOS`, alongside the executable;
installing DMGs through an external launcher depends on that launcher's support.

## Disc verification

`game.dumps` records full-image size and CRC32 values from Redump metadata,
retrieved through the [Xbox 360 DAT](https://github.com/libretro/libretro-database/blob/master/metadat/redump/Microsoft%20-%20Xbox%20360.dat)
and [PS3 DAT](https://github.com/libretro/libretro-database/blob/master/metadat/redump/Sony%20-%20PlayStation%203.dat)
on 2026-11-05. The PS3 entries refer to encrypted Redump images; a decrypted
image or extracted folder can still install but does not receive a matching
whole-disc badge. The installer independently identifies supported releases
using the executable SHA-256 values in `src/installer/release_id.cpp`.

| Disc | Bytes | CRC32 |
| --- | ---: | --- |
| Xbox 360 Europe | 7835492352 | 2cec7d69 |
| Xbox 360 North America | 7835492352 | 59e012cd |
| Xbox 360 Japan | 7835492352 | 6f0ebc6f |
| PS3 Europe | 11110907904 | aa3753a7 |
| PS3 North America | 11108483072 | 3ade94a2 |
| PS3 Japan | 10885267456 | fe205b52 |

## Development checks

The filesystem transaction tests run without the SDK or copyrighted game data:

```sh
cmake -S tests -B out/launcher-tests
cmake --build out/launcher-tests --config Release
ctest --test-dir out/launcher-tests -C Release --output-on-failure
```

They cover source preservation, replacement, installer failure, unfinished
phases, unowned destinations, interrupted transactions, overlapping paths,
invalid arguments and UTF-8 paths. Real disc extraction, PS3 conversion and
normal game startup additionally require the player's own copy and a game build.
