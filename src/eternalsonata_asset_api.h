// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for granular asset replacement: one string, one texture, or one
// mesh at a time, without shipping the package it lives in.
//
// Why this exists. The SDK's asset overlay (mods/<name>/game/...) replaces a
// whole guest file. Eternal Sonata ships almost everything inside .e / .bmd
// containers that hold hundreds of unrelated assets at once, so replacing one
// line of dialogue that way means redistributing every texture, mesh and script
// that shares the container: large, and other people's copyrighted data. This
// API instead patches the container in memory as the game loads it, so a mod
// ships only the bytes it authored.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options and party APIs are used:
//
//     auto set_text = reinterpret_cast<EternalSonataSetTextFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSetText"));
//     if (set_text) {
//       set_text("cfdata/adg01.e#text:ITA/17", "Nuova battuta", 0);
//     }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataAssetAbiVersion() before using anything added
// after version 1.
//
// Most mods do not need this header at all. Dropping files into
// mods/<name>/assets/ does the same thing declaratively and is the documented
// default; see docs/making-mods.md. This ABI is for mods that decide what to
// replace at runtime (a translation that follows the language setting, a
// texture generated from the player's own files, a randomiser).
//
// ---------------------------------------------------------------------------
// Asset references
// ---------------------------------------------------------------------------
// Everything here is addressed by a reference string:
//
//     <guest path>#<kind>:<selector>
//
//     cfdata/adg01.e#text:USA/17        string id 17, USA block, first BTX blob
//     cfdata/adg01.e#text:1/USA/17      same, in the file's second BTX blob
//     e0020_020.e#tex:face_alg.tga      texture chunk by its embedded name
//     e0020_020.e#tex:3                 fourth texture chunk, in file order
//     map/nyaza.e#mesh:2                third NSHP chunk
//     map/nyaza.e#mesh:head             NSHP chunk by name
//     sound/cxs/bgm042.cxs#music        the whole track (one .cxs is one track)
//     sound/spc001.csf#sfx:7            clip 7 of a sound bank / voice bank
//     sound/vo/field01.wav              a big-endian PCM .wav, replaced whole
//
// The guest path is the path as it appears in index.vmtoc: lowercase,
// '/'-separated (a '\' is accepted and normalised), no "game:\" prefix.
//
// Names are preferred over ordinals wherever a name exists: ordinals shift if
// the container ever changes, names do not. EternalSonataEnumerateAssets()
// lists the valid references for a container, which is also what the
// asset browser overlay and scripts/es_asset.py print.
//
// ---------------------------------------------------------------------------
// When patches are applied
// ---------------------------------------------------------------------------
// The host serves patched containers through the VFS: the first time the game
// opens a patched file, the host decodes it (see docs/asset-formats.md §2),
// splices in every registered patch, and serves the result as an uncompressed
// file. Nothing is written to disk and the game's own loader is untouched.
//
// INVARIANT: a patched container is ALWAYS served together with a patched
// index.vmtoc record for it, with the codec flag set to 0 (stored) and the
// size field set to the new decoded length. The loader sizes its allocation
// from that record (docs/asset-formats.md §1), so serving patched bytes under
// the shipped record hands the game the wrong length and fails in a way that
// looks like anything but the real cause. There is no path here that touches a
// container without touching its record; if a code change makes one possible,
// that is the bug.
//
// Consequences worth knowing:
//
//   - Register patches before the container is first opened. OnModuleLaunched()
//     is early enough for everything except boot-time files; a patch registered
//     later applies the next time the container is loaded, which for field data
//     is the next area transition. EternalSonataInvalidateAsset() forces the
//     rebuild for a file already in the cache.
//   - Patching costs one decode plus one rebuild per container, once, cached
//     until invalidated. Patching a file the game streams every frame is fine;
//     re-registering a patch every frame is not.
//   - Every mutation here is thread-safe and takes effect at load time, so no
//     call runs guest code and none of them can be refused for game state.
//
// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------
// There is one patched image per container, built from every enabled mod at
// once. The host does not choose a winning mod and serve its version of the
// file: it collects the patches from all of them, keyed by reference, and
// splices them into a single rebuild. Two mods each changing a different
// string in the same .e both take effect and neither needs to know the other
// exists.
//
//   - Distinct references always compose, in any mod order.
//   - The same reference is resolved by mod priority (earlier in mods.toml
//     wins). The loser's patch is dropped rather than merged (a texture or a
//     mesh is a whole-chunk replacement; there is no way to blend two), logged
//     at WARN naming both mods, and reported to the losing caller as
//     ETERNALSONATA_ASSET_CONFLICT.
//   - ETERNALSONATA_ASSET_FORCE beats an earlier mod's patch for the same
//     reference. Two forcing mods fall back to priority between themselves.
//   - A whole-file overlay of the container (a mod's game/ folder) becomes the
//     base image, and every mod's granular patches apply on top of it. A
//     reference that no longer resolves against it reports
//     ETERNALSONATA_ASSET_NOT_FOUND naming the mod that registered it.
//   - Surviving patches are spliced in ascending file offset, not in mod
//     order, so several resizing patches in one container compose and offsets
//     are recomputed once. Mod order decides which patch applies, never where
//     the bytes land.
//
// The one shared resource is the preserve-size budget: free room in a BTX
// language block comes from deduplicating identical strings within that block,
// so several mods growing strings in the same block spend the same pool. When
// it runs out the patches that did not fit are individually reported as
// ETERNALSONATA_ASSET_TOO_LARGE, with the mod and reference named, rather than
// one of them corrupting the block. ETERNALSONATA_ASSET_ALLOW_RESIZE removes
// the limit per patch, so one mod opting into a rebuild does not impose it on
// the others.
//
// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
// Published on the shared mod registry bus (rex::system::ModRegistry, via
// runtime->mod_registry()), so subscribing needs neither this header nor a
// linked symbol:
//
//     ETERNALSONATA_ASSET_EVENT_LOADING  "eternalsonata.asset.loading"
//     ETERNALSONATA_ASSET_EVENT_PATCHED  "eternalsonata.asset.patched"
//
// `bytes` is the container's guest path in both. On `.loading` the payload's
// `u64` is 0 and the host is about to build the patched image: this is the last
// moment a patch for that container can be registered, and it is the intended
// hook for a mod that wants to decide lazily (see the provider callback below,
// which is the same point with the path handed to you directly). On `.patched`
// the `u64` is the number of patches applied and `f64` the resulting size.
//
// Both are published from whichever guest thread is loading the container, not
// from the frame tick or the UI thread. A subscriber must be thread-safe and
// must not touch ImGui.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_ASSET_ABI_VERSION 1u

#define ETERNALSONATA_ASSET_EVENT_LOADING "eternalsonata.asset.loading"
#define ETERNALSONATA_ASSET_EVENT_PATCHED "eternalsonata.asset.patched"

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------
typedef enum EternalSonataAssetResult {
  ETERNALSONATA_ASSET_OK = 0,
  // The reference did not parse, or named a kind the selector cannot address.
  ETERNALSONATA_ASSET_BAD_REF = 1,
  // The reference parsed but nothing in the container matches it. Distinct
  // from BAD_REF on purpose: this is the "the file moved on you" case.
  ETERNALSONATA_ASSET_NOT_FOUND = 2,
  // The replacement is well-formed but cannot be spliced under the current
  // flags, e.g. it is larger than the original and RESIZE was not passed.
  ETERNALSONATA_ASSET_TOO_LARGE = 3,
  // The replacement data itself is malformed (bad PNG, mesh with no vertices,
  // a texture whose dimensions are not a multiple of four).
  ETERNALSONATA_ASSET_BAD_DATA = 4,
  // Another mod already owns this exact reference and won on mod order (see
  // Composition above). Only this one patch was dropped; every other patch
  // this mod registered for the same container still applies. Reported, not
  // silently swallowed.
  ETERNALSONATA_ASSET_CONFLICT = 5,
  // The host could not read a file path handed to it.
  ETERNALSONATA_ASSET_IO_ERROR = 6,
  ETERNALSONATA_ASSET_UNSUPPORTED = 7,
} EternalSonataAssetResult;

// ---------------------------------------------------------------------------
// Flags (bitmask, shared by every Replace/Set call)
// ---------------------------------------------------------------------------

// Default. The patch must fit the space the original occupies; the remainder is
// padded. Nothing after the patched item moves, so nothing else in the file can
// be disturbed. Text gets extra room for free here because identical strings in
// a language block are deduplicated to share one copy (entry offsets are
// arbitrary, so the game's reader cannot tell).
#define ETERNALSONATA_ASSET_PRESERVE_SIZE 0u

// Allow the patched item to grow. The host rebuilds the container and fixes up
// the .e relocation tables for the shift (docs/asset-formats.md §3.4.2). This
// is well tested for text and is what a translation needs; for textures and
// meshes prefer to stay within the original footprint where you can.
#define ETERNALSONATA_ASSET_ALLOW_RESIZE (1u << 0)

// Apply to every language block rather than the one the reference names. Only
// meaningful for text; useful for a mod that ships one translation and wants it
// visible whatever the player's language setting is.
#define ETERNALSONATA_ASSET_ALL_LANGUAGES (1u << 1)

// Take this patch even if a higher-priority mod already registered the same
// reference. Off by default: mod order decides, and the loser is told via
// ETERNALSONATA_ASSET_CONFLICT rather than being overwritten in silence.
#define ETERNALSONATA_ASSET_FORCE (1u << 2)

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------
typedef enum EternalSonataAssetKind {
  ETERNALSONATA_ASSET_KIND_TEXT = 0,     // a BTX string
  ETERNALSONATA_ASSET_KIND_TEXTURE = 1,  // an NTX2 / NTEX / NTX3 chunk
  ETERNALSONATA_ASSET_KIND_MESH = 2,     // an NSHP chunk
  ETERNALSONATA_ASSET_KIND_RAW = 3,      // a whole guest file
  ETERNALSONATA_ASSET_KIND_MUSIC = 4,    // a .cxs streaming track
  ETERNALSONATA_ASSET_KIND_SFX = 5,      // one clip of a .csf bank
} EternalSonataAssetKind;

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
// Text is the game's own single-byte encoding, not UTF-8: the font draws one
// glyph per byte, so "e" with an acute accent is the single byte "\xE9". A
// newline is the literal two-character sequence '\' 'n', not 0x0A. Markup tags
// (<w>, <w1500>, <c Allegretto>, ...) are passed through untouched; see
// docs/asset-formats.md §3.5 for the set.
//
// `ref` names the language by the game's fourcc, without the trailing space:
// JPN, USA, GBR, FRA, ITA, DEU, ESP.

typedef EternalSonataAssetResult (*EternalSonataSetTextFn)(const char* ref, const char* text,
                                                           uint32_t flags);

// Reads the shipped string, so a mod can translate from the original rather
// than hardcoding what it thinks is there. Writes up to `capacity` bytes
// including the terminator and returns the full byte length through
// `out_length` (so a caller can size a buffer and call again). `buffer` may be
// NULL when `capacity` is 0.
typedef EternalSonataAssetResult (*EternalSonataGetTextFn)(const char* ref, char* buffer,
                                                           uint32_t capacity,
                                                           uint32_t* out_length);

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
// Pixels are RGBA8, row-major, tightly packed, top row first. The host does the
// rest: BC encoding to whatever format the original chunk used, mip generation,
// Xbox 360 tiling, and the endian and pitch fields of the chunk's fetch
// constant.
//
// Dimensions must be multiples of four. Matching the original's dimensions
// keeps the patch inside its original footprint; anything larger needs
// ETERNALSONATA_ASSET_ALLOW_RESIZE.
//
// This is not the only way to replace a texture, and often not the best one.
// The SDK's content-hash path (mods/<name>/textures/<hash16>.png) already
// replaces any texture the game uploads to the GPU, without knowing which
// container it came from. Use this API when you want to address a texture by
// the name it carries in the container rather than by a hash you had to dump
// first, or when the texture is one the game reads but never uploads.

typedef struct EternalSonataImage {
  const uint8_t* pixels;  // width * height * 4 bytes, RGBA8
  uint32_t width;
  uint32_t height;
  // 1 = generate the mip chain from `pixels`. Otherwise `pixels` holds that
  // many levels back to back, each half the previous, down from width/height.
  uint32_t mip_levels;
} EternalSonataImage;

typedef EternalSonataAssetResult (*EternalSonataReplaceTextureFn)(const char* ref,
                                                                  const EternalSonataImage* image,
                                                                  uint32_t flags);

// Same, from a .png or .dds on disk. A DDS whose format already matches the
// original chunk is spliced with no re-encode, which is both faster and
// lossless; anything else is decoded to RGBA8 first and takes the path above.
typedef EternalSonataAssetResult (*EternalSonataReplaceTextureFromFileFn)(const char* ref,
                                                                          const char* host_path,
                                                                          uint32_t flags);

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------
// Vertex layout mirrors what the NSHP chunk stores (see the studio's
// NSHPParser, and docs/asset-formats.md): position, normal, one UV set, and
// four bone weights and indices for skinned meshes. The host packs it into
// whatever stride the original chunk declared, converting to the half-float and
// 10/10/10 packed-normal forms the game expects, and rebuilds the index buffer
// as the triangle list or strip the original used.
//
// Constraints, which are the game's and not the API's:
//   - The bone indices must be slots that exist in the original chunk's bone
//     list. A replacement cannot introduce a new bone: the skeleton lives in a
//     separate NBN2 chunk the animations are authored against.
//   - Every face section's material id must be one the original container
//     declares. A replacement cannot introduce a new material or texture slot.
//   - Vertex and index counts are otherwise free, subject to the size rules:
//     a mesh almost never re-encodes to exactly its original size, so mesh
//     replacement effectively requires ETERNALSONATA_ASSET_ALLOW_RESIZE.

typedef struct EternalSonataVertex {
  float position[3];
  float normal[3];
  float uv[2];
  float bone_weights[4];
  uint8_t bone_ids[4];
} EternalSonataVertex;

typedef struct EternalSonataFaceSection {
  uint16_t material_id;
  uint32_t index_start;
  uint32_t index_count;
} EternalSonataFaceSection;

typedef struct EternalSonataMesh {
  const EternalSonataVertex* vertices;
  uint32_t vertex_count;
  const uint32_t* indices;  // triangle list
  uint32_t index_count;
  const EternalSonataFaceSection* sections;
  uint32_t section_count;
} EternalSonataMesh;

typedef EternalSonataAssetResult (*EternalSonataReplaceMeshFn)(const char* ref,
                                                               const EternalSonataMesh* mesh,
                                                               uint32_t flags);

// Same, from a .gltf/.glb on disk: the format the studio's exporter writes, so
// export, edit, import is a closed loop. A file with several meshes uses the
// one whose node name matches the reference's selector, or its first mesh when
// the reference selects by ordinal.
typedef EternalSonataAssetResult (*EternalSonataReplaceMeshFromFileFn)(const char* ref,
                                                                       const char* host_path,
                                                                       uint32_t flags);

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
// Music (.cxs, one file = one track) and sound/voice banks (.csf, many clips
// per file) both hold raw XMA2 packet streams. There is no open XMA2 encoder,
// so a replacement is NOT re-encoded and spliced into the container the way a
// texture is. Instead the host substitutes decoded PCM at the point where it
// already decodes XMA for the guest: the container keeps its shipped bytes and
// its timing metadata, and the audio that comes out of the decoder is yours.
//
// What this means in practice:
//
//   - Ship ordinary audio. WAV, FLAC and OGG are accepted, any sample rate and
//     channel count; the host resamples and downmixes to what the stream the
//     clip belongs to expects. You never touch XMA.
//   - Nothing about the container changes, so audio replacement never resizes
//     anything, never rebuilds an .e, and cannot conflict with a text or
//     texture patch in the same file.
//   - Length is yours for music and is NOT for anything the game times against.
//     A .cxs track loops on the host's own loop points and can be any length.
//     A voice clip whose duration the game uses to advance a text box (see the
//     <wv> markup tag) should match the original's duration; longer audio is
//     cut off when the game moves on rather than delaying it.
//   - The substitution is per clip, so replacing one sound effect in a bank of
//     two hundred leaves the other 199 shipped ones playing untouched, which
//     is the same guarantee the rest of this API makes.
//
// The .wav files in assets/sound are a different case entirely: those are
// plain big-endian PCM, not XMA, so they are replaced as whole files with a
// byte swap and no decoder involvement. Address them by guest path with no
// '#' selector, or just drop the file in the mod's assets/ tree.

typedef struct EternalSonataAudio {
  // Interleaved PCM. `sample_rate` and `channels` are the source's own; the
  // host converts. Use EternalSonataReplaceAudioFromFile for encoded formats.
  const int16_t* samples;
  uint32_t frame_count;  // frames, not samples: channels are interleaved
  uint32_t sample_rate;
  uint16_t channels;

  // Loop points in frames, for music. Both 0 = play through and stop; the
  // shipped track's own loop points are used instead when
  // `inherit_loop_points` is non-zero, which is what a straight music swap
  // wants (47 of the 62 retail .cxs tracks carry loop points).
  uint32_t loop_start;
  uint32_t loop_end;
  uint8_t inherit_loop_points;
} EternalSonataAudio;

typedef EternalSonataAssetResult (*EternalSonataReplaceAudioFn)(const char* ref,
                                                                const EternalSonataAudio* audio,
                                                                uint32_t flags);

// Same, from a .wav / .flac / .ogg on disk. Loop points are taken from the
// file's own metadata when it carries any (a WAV 'smpl' chunk, an Ogg
// LOOPSTART/LOOPLENGTH comment), otherwise from the shipped track.
typedef EternalSonataAssetResult (*EternalSonataReplaceAudioFromFileFn)(const char* ref,
                                                                        const char* host_path,
                                                                        uint32_t flags);

// ---------------------------------------------------------------------------
// Raw
// ---------------------------------------------------------------------------
// A whole-file replacement, equivalent to dropping the file in the mod's game/
// folder but decided at runtime. `bytes` is the *decoded* content: the host
// serves it uncompressed and rewrites the TOC record for it, so a mod never
// has to implement the range coder or the LZSS layer.
typedef EternalSonataAssetResult (*EternalSonataReplaceFileFn)(const char* guest_path,
                                                               const uint8_t* bytes, uint32_t size,
                                                               uint32_t flags);

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
// Every asset a container holds, in file order. Called once per asset; return
// 0 to stop the walk early. `ref` is a complete, valid reference string, and is
// only valid for the duration of the callback.
//
// For text, `name` is the string's current value in the reference's language
// and `size` its byte length. For textures and meshes `name` is the embedded
// name (empty when the chunk has none) and `size` the chunk's byte size.
typedef int (*EternalSonataAssetVisitorFn)(const char* ref, EternalSonataAssetKind kind,
                                           const char* name, uint32_t size, void* user);

// `guest_path` may name a single container, or end in '*' to walk a subtree.
// This decodes each file it touches, so it is a browse/tooling call, not
// something to run per frame.
typedef EternalSonataAssetResult (*EternalSonataEnumerateAssetsFn)(
    const char* guest_path, EternalSonataAssetVisitorFn visitor, void* user);

// ---------------------------------------------------------------------------
// Lazy providers
// ---------------------------------------------------------------------------
// Registering thousands of patches up front to cover text a player may never
// reach is wasteful. A provider is instead called once per container, at the
// moment the host is about to build its patched image, and may register any
// number of patches for that container from inside the callback. It is the
// callback form of the "eternalsonata.asset.loading" event.
//
// Called on the guest thread doing the load. It must be thread-safe, must not
// touch ImGui, and must not block: the game is waiting on this file.
typedef void (*EternalSonataAssetProviderFn)(const char* guest_path, void* user);

// Returns a token for EternalSonataUnregisterAssetProvider, or 0 on failure.
typedef uint32_t (*EternalSonataRegisterAssetProviderFn)(EternalSonataAssetProviderFn provider,
                                                         void* user);
typedef void (*EternalSonataUnregisterAssetProviderFn)(uint32_t token);

// ---------------------------------------------------------------------------
// Cache control
// ---------------------------------------------------------------------------
// Drops the host's cached patched image for a container, so the next open
// rebuilds it with whatever is registered then. Pass NULL to drop everything.
// This is what makes live iteration possible; the asset browser overlay's
// Reload button is this call.
typedef void (*EternalSonataInvalidateAssetFn)(const char* guest_path);

// Removes a patch this mod registered, returning the reference to whatever the
// next mod in priority order registered for it, or to the shipped asset.
typedef EternalSonataAssetResult (*EternalSonataClearAssetPatchFn)(const char* ref);

typedef uint32_t (*EternalSonataAssetAbiVersionFn)(void);

#ifdef __cplusplus
}  // extern "C"
#endif
