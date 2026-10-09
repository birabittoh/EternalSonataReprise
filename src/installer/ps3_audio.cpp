// PS3 audio, a port of scripts/ps3_audio.py.
//
// PS3 sound banks hold mono ATRAC3 clips (192 byte frames) where the 360's
// hold XMA2, and no XMA encoder exists. Every clip gets a stub payload that
// starts with an RXPcmSub tag, and its frames go to pcm/<token>.wav, which the
// host decodes and substitutes when the XMA decoder meets the tag. Music
// tracks in PS-ADPCM get a .cxs whose payload carries the tag the same way.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "ps3_bytes.h"
#include "ps3_convert.h"

namespace eternalsonata::ps3 {
namespace {

constexpr char kTag[] = "RXPcmSub";
constexpr size_t kPacket = 0x800;
constexpr uint32_t kAtrac3Frame = 192;
constexpr uint32_t kAtrac3Samples = 1024;

constexpr int kAdpcmCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

constexpr uint32_t kCxsHeader = 0x800;
constexpr uint32_t kCxsPayload = 0x1000;
constexpr uint32_t kCxsBlock = 0x10000;

size_t Align(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

void PadAlign(Bytes& d, size_t a) {
  d.resize(Align(d.size(), a));
}

using Loop = std::pair<uint32_t, uint32_t>;

// Little endian WAV around encoded frames; loop is (start, end) in samples,
// end exclusive.
Bytes SidecarWav(const Bytes& fmt, const Bytes& data, const Loop* loop, uint32_t rate) {
  Bytes body;
  Append(body, "fmt ", 4);
  PutLe32(body, uint32_t(fmt.size()));
  Append(body, fmt);
  if (loop) {
    Bytes smpl;
    for (uint32_t v : {0u, 0u, 1000000000u / rate, 60u, 0u, 0u, 0u, 1u, 0u})
      PutLe32(smpl, v);
    for (uint32_t v : {0u, 0u, loop->first, loop->second - 1, 0u, 0u})
      PutLe32(smpl, v);
    Append(body, "smpl", 4);
    PutLe32(body, uint32_t(smpl.size()));
    Append(body, smpl);
  }
  Append(body, "data", 4);
  PutLe32(body, uint32_t(data.size()));
  Append(body, data);
  Bytes out;
  Append(out, "RIFF", 4);
  PutLe32(out, uint32_t(4 + body.size()));
  Append(out, "WAVE", 4);
  Append(out, body);
  return out;
}

Bytes Atrac3Wav(const Bytes& frames, uint32_t rate, const Loop* loop) {
  Bytes ext;
  PutLe16(ext, 1);
  PutLe32(ext, 0);
  for (uint32_t v : {0u, 0u, 1u, 0u})
    PutLe16(ext, v);
  Bytes fmt;
  PutLe16(fmt, kWaveAtrac3);
  PutLe16(fmt, 1);
  PutLe32(fmt, rate);
  PutLe32(fmt, kAtrac3Frame * rate / kAtrac3Samples);
  PutLe16(fmt, kAtrac3Frame);
  PutLe16(fmt, 0);
  PutLe16(fmt, uint32_t(ext.size()));
  Append(fmt, ext);
  return SidecarWav(fmt, frames, loop, rate);
}

Bytes PsxAdpcmWav(const Bytes& data, uint32_t channels, uint32_t rate, const Loop* loop) {
  Bytes fmt;
  PutLe16(fmt, kWavePsxAdpcm);
  PutLe16(fmt, channels);
  PutLe32(fmt, rate);
  PutLe32(fmt, rate * channels * 16 / 28);
  PutLe16(fmt, 16 * channels);
  PutLe16(fmt, 4);
  return SidecarWav(fmt, data, loop, rate);
}

// The 360's own sound/cxs/*.wav layout: RIFF with big endian fields and samples.
Bytes GuestWav(const Bytes& pcm_be, uint32_t channels, uint32_t rate) {
  Bytes fmt;
  Put16(fmt, 1);
  Put16(fmt, channels);
  Put32(fmt, rate);
  Put32(fmt, rate * channels * 2);
  Put16(fmt, channels * 2);
  Put16(fmt, 16);
  Bytes body;
  Append(body, "fmt ", 4);
  Put32(body, fmt.size());
  Append(body, fmt);
  Append(body, "data", 4);
  Put32(body, pcm_be.size());
  Append(body, pcm_be);
  Bytes out;
  Append(out, "RIFF", 4);
  Put32(out, 4 + body.size());
  Append(out, "WAVE", 4);
  Append(out, body);
  return out;
}

Bytes TaggedPacket(const Token& tok) {
  Bytes out(kTag, kTag + 8);
  out.insert(out.end(), tok.begin(), tok.end());
  out.resize(kPacket);
  return out;
}

// ---------------------------------------------------------------------------
// Sound banks
// ---------------------------------------------------------------------------
struct Tim {
  Bytes raw;
  uint32_t flags, rate, offset, size, loop_start, loop_end, lip_off, lip_len, xma_off, xma_len,
      seek_off, seek_len;

  Tim(const Bytes& d, size_t at) : raw(Slice(d, at, at + Rd32(d, at + 4))) {
    uint32_t* fields[] = {&flags,   &rate,    &offset,  &size,    &loop_start, &loop_end,
                          &lip_off, &lip_len, &xma_off, &xma_len, &seek_off,   &seek_len};
    for (int i = 0; i < 12; ++i)
      *fields[i] = Rd32(raw, 8 + 4 * i);
  }

  uint32_t Samples() const { return size / kAtrac3Frame * kAtrac3Samples; }

  uint32_t Kind() const { return flags == 1 ? 0xFF : flags; }
};

struct Prog {
  Bytes head;
  std::vector<Tim> tims;
};

struct Csf {
  Bytes pre;   // bytes before PGHD
  Bytes pghd;  // PGHD header
  std::vector<Prog> progs;
  size_t head = 0;
};

Csf ParseCsf(const Bytes& d) {
  Csf csf;
  csf.head = Rd32(d, 8);
  size_t at = 0x10;
  while (!(at + 4 <= d.size() && std::memcmp(d.data() + at, "PGHD", 4) == 0)) {
    at += Rd32(d, at + 4);
    if (at >= csf.head)
      throw std::runtime_error("no PGHD in the bank header");
  }
  csf.pre = Slice(d, 0, at);
  csf.pghd = Slice(d, at, at + 16);
  const size_t end = at + Rd32(d, at + 4);
  at += 16;
  while (at < end) {
    if (!(at + 4 <= d.size() && std::memcmp(d.data() + at, "PROG", 4) == 0))
      throw std::runtime_error("expected PROG in the bank header");
    const size_t prog_end = at + Rd32(d, at + 4);
    Prog prog{Slice(d, at, at + 16), {}};
    for (size_t t = at + 16; t < prog_end; t += Rd32(d, t + 4))
      prog.tims.emplace_back(d, t);
    csf.progs.push_back(std::move(prog));
    at = prog_end;
  }
  return csf;
}

// A 360 TIM for a substituted clip: the PS3 clip's parameters, LIP and rate,
// with one XMA block of stub payload described as frames long.
Bytes NewTim(const Tim& ps3, uint32_t offset, uint32_t size, uint32_t frames, bool loops) {
  const Bytes& raw = ps3.raw;
  size_t params_end = raw.size();
  bool any = false;
  for (uint32_t o : {ps3.lip_off, ps3.xma_off, ps3.seek_off}) {
    if (o) {
      params_end = any ? std::min<size_t>(params_end, o) : o;
      any = true;
    }
  }
  Bytes params = Slice(raw, 0x38, params_end & ~size_t(3));
  // The low half of the first parameter word is 6 on every PS3 clip, 0 on the 360.
  Splice(params, 2, 4, Bytes{0, 0});
  const Bytes lip = ps3.lip_len ? Slice(raw, ps3.lip_off, size_t(ps3.lip_off) + ps3.lip_len) : Bytes();
  Bytes xma;
  for (uint32_t v : {loops ? 0x030100FFu : 0x03010000u, 0u, loops ? frames : 0u, ps3.rate, size,
                     frames, frames, 1u, 0x01000001u})
    Put32(xma, v);
  Bytes seek;
  Put32(seek, frames);
  const size_t lip_off = lip.empty() ? 0 : 0x38 + params.size();
  const size_t xma_off = Align(0x38 + params.size() + lip.size(), 4);
  Bytes out;
  Append(out, "TIM \0\0\0\0", 8);
  for (uint64_t v : {uint64_t(ps3.Kind()), uint64_t(ps3.rate), uint64_t(offset), uint64_t(size),
                     uint64_t(0), uint64_t(0), uint64_t(lip_off), uint64_t(lip.size()),
                     uint64_t(xma_off), uint64_t(xma.size()), uint64_t(xma_off + xma.size()),
                     uint64_t(seek.size())})
    Put32(out, v);
  Append(out, params);
  Append(out, lip);
  out.resize(xma_off);
  Append(out, xma);
  Append(out, seek);
  Wr32(out, 4, out.size());
  return out;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

}  // namespace

// 8 byte FNV-1a of a stable key; the sidecar's file name and the tag's tail.
Token MakeToken(const std::string& key) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : key)
    h = (h ^ c) * 0x100000001B3ull;
  Token token;
  for (int i = 0; i < 8; ++i)
    token[i] = uint8_t(h >> (8 * i));
  return token;
}

std::string TokenHex(const Token& token) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : token) {
    out += kDigits[b >> 4];
    out += kDigits[b & 15];
  }
  return out;
}

Bytes ConvertCsf(const std::string& rel, const Bytes& ps3, const PcmWriter& write_pcm,
                 Report& report) {
  const Csf csf = ParseCsf(ps3);
  const Bytes dropped = Slice(ps3, csf.pre.size() + Rd32(csf.pghd, 4), csf.head);
  if (std::any_of(dropped.begin(), dropped.end(), [](uint8_t b) { return b != 0; }))
    report.Warn(rel + ": data after PGHD in the bank header is dropped");
  size_t ordinal = 0;
  Bytes progs, payload;
  for (const Prog& prog : csf.progs) {
    Bytes blob = prog.head;
    for (const Tim& tim : prog.tims) {
      PadAlign(payload, 0x1000);
      const size_t offset = payload.size();
      const Bytes frames = Slice(ps3, csf.head + tim.offset, csf.head + tim.offset + tim.size);
      if (tim.xma_off) {
        // ep121 and ep146 each kept a 360 clip, XMA header and all.
        Append(payload, frames);
        Bytes t = tim.raw;
        Wr32(t, 0x10, offset);
        Append(blob, t);
        ++report.counts["audio clips already XMA"];
        ++ordinal;
        continue;
      }
      if (tim.size % kAtrac3Frame || frames.empty() || frames[0] != 0xA2)
        report.Warn(rel + ": clip " + std::to_string(ordinal) + " is not mono 192 byte ATRAC3");
      const uint32_t n = tim.Samples();
      const bool loops = tim.flags == 1;
      Loop loop;
      if (loops) {
        const uint32_t start = tim.loop_start / kAtrac3Frame * kAtrac3Samples;
        const uint32_t end_frames = tim.loop_end / kAtrac3Frame * kAtrac3Samples;
        const uint32_t end = std::min(n, end_frames ? end_frames : n);
        loop = start < end ? Loop(start, end) : Loop(0u, n);
      }
      const Token tok = MakeToken(Lower(rel) + "#" + std::to_string(ordinal));
      write_pcm(tok, Atrac3Wav(frames, tim.rate, loops ? &loop : nullptr));
      Append(payload, TaggedPacket(tok));
      Append(blob, NewTim(tim, uint32_t(offset), kPacket, n, loops));
      ++report.counts["audio clips"];
      ++ordinal;
    }
    Wr32(blob, 4, blob.size());
    Append(progs, blob);
  }
  Bytes pg = csf.pghd;
  Append(pg, progs);
  Wr32(pg, 4, pg.size());
  Bytes header = csf.pre;
  Append(header, pg);
  PadAlign(header, 0x1000);
  PadAlign(payload, 0x1000);
  Wr32(header, 4, header.size() + payload.size());
  Wr32(header, 8, header.size());
  Wr32(header, 12, payload.size());
  Append(header, payload);
  return header;
}

// A .cxs header as every 360 track has it, and a payload about as long as
// the 360's XMA tracks take (0.11 to 0.28 bytes per sample and channel),
// since the guest streams it while the track plays; only its first packet
// carries the tag.
Bytes NewCxs(const Token& tok, uint32_t channels, uint32_t rate, uint32_t frames,
             const Loop* loop) {
  const size_t size = Align(size_t(frames) * channels / 4, kPacket);
  const size_t blocks = (size + kCxsBlock - 1) / kCxsBlock;
  Bytes out;
  Append(out, "CXS ", 4);
  for (uint64_t v : {uint64_t(kCxsHeader), uint64_t(rate), uint64_t(channels), uint64_t(frames),
                     uint64_t(loop ? loop->first : 0), uint64_t(loop ? Align(loop->second, 512) : 0),
                     uint64_t(blocks), uint64_t(kCxsPayload), uint64_t(size), uint64_t(kPacket),
                     uint64_t(kPacket)})
    Put32(out, v);
  out.resize(kCxsHeader);
  // The samples played by the end of each block, in whole XMA frames, the
  // last one frame past the end as on the 360. The guest does not start a
  // track whose loop end falls outside the table.
  for (size_t i = 0; i + 1 < blocks; ++i)
    Put32(out, Align((i + 1) * frames / blocks, 512));
  Put32(out, Align(frames, 512) + 512);
  out.resize(kCxsPayload);
  Append(out, TaggedPacket(tok));
  out.resize(kCxsPayload + size);
  return out;
}

std::vector<Output> ConvertCps(const std::string& rel, const Bytes& d, const PcmWriter& write_pcm,
                               Report& report) {
  const uint32_t header = Rd32(d, 4), channels = Rd32(d, 8), size = Rd32(d, 12),
                 rate = Rd32(d, 16), loop_start = Rd32(d, 20), loop_end = Rd32(d, 24),
                 kind = Rd32(d, 28);
  const Bytes data = Slice(d, header, size_t(header) + size);
  std::string stem = rel.substr(rel.find_last_of('/') + 1);
  if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot)
    stem.resize(dot);
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  if (kind == 0) {
    // Plain PCM, already big endian: the scripts ask for these as .wav.
    ++report.counts["music tracks as WAV"];
    return {{"sound/cxs/" + stem + ".wav", GuestWav(data, channels, rate)}};
  }
  const std::string target = "sound/cxs/" + stem + ".cxs";
  const uint32_t frames = uint32_t(data.size() / (16 * size_t(channels)) * 28);
  const Loop loop{loop_start / (16 * channels) * 28, frames};
  const Token tok = MakeToken(Lower(target));
  write_pcm(tok, PsxAdpcmWav(data, channels, rate, loop_end ? &loop : nullptr));
  ++report.counts["music tracks as PS-ADPCM"];
  return {{target, NewCxs(tok, channels, rate, frames, loop_end ? &loop : nullptr)}};
}

std::vector<int16_t> DecodePsxAdpcm(const uint8_t* data, size_t size, uint32_t channels) {
  const size_t frames = channels ? size / (16 * size_t(channels)) : 0;
  std::vector<int16_t> mixed(frames * 28 * channels);
  for (uint32_t c = 0; c < channels; ++c) {
    int64_t h1 = 0, h2 = 0;
    size_t s = 0;
    for (size_t f = 0; f < frames; ++f) {
      const uint8_t* frame = data + (f * channels + c) * 16;
      const int shift = frame[0] & 15, filt = frame[0] >> 4;
      const int64_t c1 = filt < 5 ? kAdpcmCoef[filt][0] : 0;
      const int64_t c2 = filt < 5 ? kAdpcmCoef[filt][1] : 0;
      for (int i = 0; i < 28; ++i) {
        const int n = frame[2 + (i >> 1)] >> ((i & 1) * 4) & 15;
        int64_t v = (int64_t(n > 7 ? n - 16 : n) * 4096 >> shift) + ((h1 * c1 + h2 * c2 + 32) >> 6);
        v = std::clamp<int64_t>(v, -32768, 32767);
        mixed[s++ * channels + c] = int16_t(v);
        h2 = h1;
        h1 = v;
      }
    }
  }
  return mixed;
}

}  // namespace eternalsonata::ps3
