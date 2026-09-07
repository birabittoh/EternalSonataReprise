#include "generated/eternalsonata_init.h"

#include <cctype>
#include <cstring>
#include <string>

#include <rex/hook.h>

#include "settings.h"

// ---------------------------------------------------------------------------
// Mod-defined voice languages
// ---------------------------------------------------------------------------
//
// Voice banks are `.csf` files under `btldata\voice\`. There is no language
// directory and no language field in the container: the language IS a filename
// suffix. The Japanese bank is the bare name, the English one has `_usa`
// appended, and a mod voice language gets a suffix of its own (see
// settings.h's VoiceLanguageOption).
//
// The game has two path builders - sub_821BD0D0, which formats
// `%spc%03d[_usa].csf` for clip ids 1..10, and sub_821BD480, which takes a name
// out of the table at unk_82024100 and rewrites its extension - and neither is
// hooked here. Both instead probe the path they built through one shared
// function, sub_8210D380, and both then open *the same buffer* they probed:
//
//   // sub_821BD1C0, buffer v22[160]
//   if ( !sub_821BD0D0(a1, *v7, v22) || !sub_8210C9D8(332 * v13 + a1, v22) )
//
//   // sub_821BD778, buffer v33[64]
//   if ( !sub_821BD480(a1, a2, i, j, v33, BYTE2(dword_8243FC04)) ||
//        !sub_8210C9D8(v23, v33) )
//
// So rewriting the buffer in place during the probe also changes the file that
// is then opened, and neither builder has to be reimplemented. That matters
// beyond tidiness: sub_821BD480 finds the split in a name like `em07_v1` by
// scanning for the *first* underscore, and it has a special case that turns a
// `bosCPN_v1` request into `spc004.csf` regardless of the clip asked for. At
// the probe the name is already resolved, so both come along for free.
//
// The other half of the trick is that the guest's own selection is a
// try-then-fall-back rather than a switch: with the byte set it builds
// `pcNNN_usa.csf`, asks whether it exists, and silently falls back to
// `pcNNN.csf` when it does not. 27 of the 45 shipped banks have no `_usa` twin,
// so that fall-back is the common path and is well exercised - which is why
// this hook restores the original bytes and probes again on a miss, rather than
// letting a bank the mod did not synthesize turn into silence.

namespace {

// sub_8210D380 is the file-existence probe for *every* file in the game, not a
// voice-specific one, so the filter has to come first. Both ends are checked:
// the directory prefix and the extension.
constexpr char kVoiceDir[] = "btldata\\voice\\";
constexpr size_t kVoiceDirLen = sizeof(kVoiceDir) - 1;

// The smaller of the two caller buffers (sub_821BD778's v33[64]); sub_821BD1C0
// gives 160. A rewritten path is only ever the original plus at most
// kMaxVoiceSuffixBytes, and the longest shipped voice path is 28 bytes, so this
// has plenty of headroom - but the buffer belongs to the caller, so the bound
// is checked rather than assumed.
constexpr size_t kCallerBufferBytes = 64;

// Reads the NUL-terminated guest string at `at`, up to the caller's buffer.
// Returns false if it is not terminated inside it, which is the same "do not
// touch this" answer as a path that is not a voice bank.
bool ReadGuestPath(u8* base, u32 at, std::string* out) {
  out->clear();
  for (size_t i = 0; i < kCallerBufferBytes; ++i) {
    const char c = static_cast<char>(REX_LOAD_U8(at + static_cast<u32>(i)));
    if (!c)
      return true;
    out->push_back(c);
  }
  return false;
}

void WriteGuestPath(u8* base, u32 at, const std::string& path) {
  for (size_t i = 0; i < path.size(); ++i)
    REX_STORE_U8(at + static_cast<u32>(i), static_cast<u8>(path[i]));
  REX_STORE_U8(at + static_cast<u32>(path.size()), 0);
}

bool IsVoiceBankPath(const std::string& path) {
  if (path.size() <= kVoiceDirLen + 4)
    return false;
  // The builders use backslashes; compare case-insensitively anyway, since the
  // clip-name table is authored data and nothing guarantees its case.
  for (size_t i = 0; i < kVoiceDirLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(path[i])) !=
        std::tolower(static_cast<unsigned char>(kVoiceDir[i])))
      return false;
  }
  return path.compare(path.size() - 4, 4, ".csf") == 0;
}

// `btldata\voice\pc001[_usa].csf` -> `btldata\voice\pc001<suffix>.csf`. The
// donor's own suffix is stripped first, so this is correct whichever value the
// guest's byte happens to hold - a mod voice language leaves that byte at its
// donor's value, and the guest may therefore have built either form. Kept
// byte-identical to VoiceBankWithSuffix in eternalsonata_asset_system.cpp,
// which decides the cache path this then has to ask for.
std::string WithSuffix(const std::string& path, const char* suffix) {
  std::string stem = path.substr(0, path.size() - 4);
  if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_usa") == 0)
    stem.resize(stem.size() - 4);
  return stem + suffix + ".csf";
}

}  // namespace

REX_EXTERN(__imp__sub_8210D380);

REX_HOOK_RAW(sub_8210D380) {
  // BootVoiceSuffix is null for both shipped voice languages, which is the
  // overwhelmingly common case and the whole of the "stands down" condition:
  // the guest's own byte then decides and nothing here runs. It reads a value
  // latched on the host thread at startup (see BootVoiceLanguageIndex), so this
  // is a plain load rather than a trip through the cvar registry on every
  // file-existence check in the game.
  const char* suffix = eternalsonata::BootVoiceSuffix();
  const u32 buffer = ctx.r3.u32;
  if (!suffix || !buffer) {
    __imp__sub_8210D380(ctx, base);
    return;
  }

  std::string path;
  if (!ReadGuestPath(base, buffer, &path) || !IsVoiceBankPath(path)) {
    __imp__sub_8210D380(ctx, base);
    return;
  }

  const std::string wanted = WithSuffix(path, suffix);
  if (wanted.size() + 1 > kCallerBufferBytes || wanted == path) {
    __imp__sub_8210D380(ctx, base);
    return;
  }

  WriteGuestPath(base, buffer, wanted);
  __imp__sub_8210D380(ctx, base);
  if (ctx.r3.u32 != 0) {
    // Found. The buffer keeps the rewritten path, which is what the caller
    // hands to sub_8210C9D8 next - that is the whole mechanism.
    return;
  }

  // A clip this mod did not synthesize a bank for. Put the original path back
  // and let the guest's own fall-back chain proceed exactly as it would have,
  // so the line plays in the donor language instead of falling silent.
  WriteGuestPath(base, buffer, path);
  ctx.r3.u32 = buffer;
  __imp__sub_8210D380(ctx, base);
}
