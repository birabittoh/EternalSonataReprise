// PS3 audio, a port of scripts/ps3_audio.py.
//
// PS3 sound banks hold mono ATRAC3 clips (192 byte frames) where the 360's
// hold XMA2, and no XMA encoder exists. A clip whose 360 twin has the same
// length keeps the 360 XMA; any other clip gets a stub payload that starts
// with an RXPcmSub tag, and its PCM goes to pcm/<token>.wav, which the host
// substitutes when the XMA decoder meets the tag.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <rex/audio/atrac3.h>

#include "ps3_bytes.h"
#include "ps3_convert.h"

namespace eternalsonata::ps3 {
namespace {

constexpr char kTag[] = "RXPcmSub";
constexpr size_t kPacket = 0x800;
constexpr uint32_t kAtrac3Frame = 192;
constexpr uint32_t kAtrac3Samples = 1024;
// A duration within this many samples counts as the same clip: ATRAC3 and XMA
// pad a clip to different frame sizes.
constexpr int64_t kSameClipSlack = 3000;

constexpr int kAdpcmCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

size_t Align(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

void PadAlign(Bytes& d, size_t a) {
  d.resize(Align(d.size(), a));
}

// Little endian 16 bit WAV; loop is (start, end) in frames, end exclusive.
Bytes PcmWav(const Bytes& pcm, uint32_t channels, uint32_t rate,
             const std::pair<uint32_t, uint32_t>* loop) {
  Bytes fmt;
  PutLe16(fmt, 1);
  PutLe16(fmt, channels);
  PutLe32(fmt, rate);
  PutLe32(fmt, rate * channels * 2);
  PutLe16(fmt, channels * 2);
  PutLe16(fmt, 16);
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
  PutLe32(body, uint32_t(pcm.size()));
  Append(body, pcm);
  Bytes out;
  Append(out, "RIFF", 4);
  PutLe32(out, uint32_t(4 + body.size()));
  Append(out, "WAVE", 4);
  Append(out, body);
  return out;
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

Bytes Samples(const std::vector<int16_t>& pcm) {
  Bytes out;
  out.reserve(pcm.size() * 2);
  for (int16_t s : pcm)
    PutLe16(out, uint16_t(s));
  return out;
}

Bytes DecodeAtrac3(const Bytes& frames, uint32_t rate) {
  std::vector<int16_t> pcm;
  if (!rex::audio::DecodeAtrac3(frames.data(), frames.size(), rate, 1, kAtrac3Frame, pcm))
    throw std::runtime_error("ATRAC3 decoding failed");
  return Samples(pcm);
}

// PS-ADPCM, channels interleaved per 16 byte frame -> little endian PCM.
Bytes DecodePsxAdpcm(const Bytes& data, uint32_t channels) {
  const size_t frames = data.size() / (16 * size_t(channels));
  std::vector<int16_t> mixed(frames * 28 * channels);
  for (uint32_t c = 0; c < channels; ++c) {
    int64_t h1 = 0, h2 = 0;
    size_t s = 0;
    for (size_t f = 0; f < frames; ++f) {
      const size_t o = (f * channels + c) * 16;
      const uint8_t head = data[o];
      const int shift = head & 15, filt = head >> 4;
      const int64_t c1 = filt < 5 ? kAdpcmCoef[filt][0] : 0;
      const int64_t c2 = filt < 5 ? kAdpcmCoef[filt][1] : 0;
      for (int i = 0; i < 28; ++i) {
        const int n = data[o + 2 + (i >> 1)] >> ((i & 1) * 4) & 15;
        int64_t v = (int64_t(n > 7 ? n - 16 : n) * 4096 >> shift) + ((h1 * c1 + h2 * c2 + 32) >> 6);
        v = std::clamp<int64_t>(v, -32768, 32767);
        mixed[s++ * channels + c] = int16_t(v);
        h2 = h1;
        h1 = v;
      }
    }
  }
  return Samples(mixed);
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

  int64_t Samples() const {
    if (xma_off) {
      // ep171 and ep186 each hold a 20 byte header without a sample count; a
      // length no clip can be within the slack of never pairs.
      if (xma_len < 0x18)
        return -(int64_t(1) << 40);
      return Rd32(raw, size_t(xma_off) + 0x14);
    }
    return int64_t(size / kAtrac3Frame) * kAtrac3Samples;
  }

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

struct Shape {
  uint32_t kind;
  int64_t samples;
};

std::vector<Shape> ClipShapes(const Bytes& bank) {
  std::vector<Shape> out;
  for (const Prog& prog : ParseCsf(bank).progs) {
    for (const Tim& t : prog.tims)
      out.push_back({t.Kind(), t.Samples()});
  }
  return out;
}

bool SameClip(const Shape& a, const Shape& b) {
  return a.kind == b.kind && std::llabs(a.samples - b.samples) <= kSameClipSlack;
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

Bytes ConvertCsf(const std::string& rel, const Bytes& ps3, const Bytes* x360,
                 const PcmWriter& write_pcm, Report& report) {
  const Csf csf = ParseCsf(ps3);
  const Bytes dropped = Slice(ps3, csf.pre.size() + Rd32(csf.pghd, 4), csf.head);
  if (std::any_of(dropped.begin(), dropped.end(), [](uint8_t b) { return b != 0; }))
    report.Warn(rel + ": data after PGHD in the bank header is dropped");
  std::vector<Tim> twins;
  size_t x_head = 0;
  if (x360) {
    for (Prog& prog : ParseCsf(*x360).progs)
      twins.insert(twins.end(), prog.tims.begin(), prog.tims.end());
    x_head = Rd32(*x360, 8);
  }
  size_t ordinal = 0;
  Bytes progs, payload;
  for (const Prog& prog : csf.progs) {
    Bytes blob = prog.head;
    for (const Tim& tim : prog.tims) {
      const Tim* twin = ordinal < twins.size() ? &twins[ordinal] : nullptr;
      PadAlign(payload, 0x1000);
      const size_t offset = payload.size();
      if (twin && tim.Kind() == twin->flags &&
          std::llabs(tim.Samples() - twin->Samples()) <= kSameClipSlack) {
        Append(payload, Slice(*x360, x_head + twin->offset, x_head + twin->offset + twin->size));
        Bytes t = twin->raw;
        Wr32(t, 0x10, offset);
        Append(blob, t);
        ++report.counts["audio clips kept from the 360"];
      } else {
        const Bytes frames = Slice(ps3, csf.head + tim.offset, csf.head + tim.offset + tim.size);
        if (tim.size % kAtrac3Frame || frames.empty() || frames[0] != 0xA2)
          report.Warn(rel + ": clip " + std::to_string(ordinal) + " is not mono 192 byte ATRAC3");
        const Bytes pcm = DecodeAtrac3(frames, tim.rate);
        const uint32_t n = uint32_t(pcm.size() / 2);
        const bool loops = tim.flags == 1;
        std::pair<uint32_t, uint32_t> loop;
        if (loops) {
          const uint32_t start = tim.loop_start / kAtrac3Frame * kAtrac3Samples;
          const uint32_t end_frames = tim.loop_end / kAtrac3Frame * kAtrac3Samples;
          const uint32_t end = std::min(n, end_frames ? end_frames : n);
          loop = start < end ? std::make_pair(start, end) : std::make_pair(0u, n);
        }
        const Token tok = MakeToken(Lower(rel) + "#" + std::to_string(ordinal));
        write_pcm(tok, PcmWav(pcm, 1, tim.rate, loops ? &loop : nullptr));
        Append(payload, kTag, 8);
        payload.insert(payload.end(), tok.begin(), tok.end());
        payload.resize(payload.size() + kPacket - 16);
        Append(blob, NewTim(tim, uint32_t(offset), kPacket, n, loops));
        ++report.counts["audio clips decoded to PCM"];
      }
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

// The 360 bank whose clips line up with the most of this one's, by ordinal as
// ConvertCsf reuses them; ties go to the same bank index.
const Bytes* BestTwin(const Bytes& bank, const std::vector<Bytes>& candidates, size_t index) {
  const auto shapes = ClipShapes(bank);
  const Bytes* best = nullptr;
  size_t best_score = 0;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto theirs = ClipShapes(candidates[i]);
    size_t score = 0;
    for (size_t k = 0; k < std::min(shapes.size(), theirs.size()); ++k)
      score += SameClip(shapes[k], theirs[k]);
    if (score > best_score || (score == best_score && score && i == index)) {
      best = &candidates[i];
      best_score = score;
    }
  }
  return best;
}

// The 360 bank directory whose banks line up with the most of this one's
// clips, position by position; ties go to the closest durations.
const std::vector<Bytes>* BestGroup(const std::vector<Bytes>& group,
                                    const std::vector<std::vector<Bytes>>& candidates) {
  std::vector<std::vector<Shape>> shapes;
  for (const Bytes& b : group)
    shapes.push_back(ClipShapes(b));
  const std::vector<Bytes>* best = nullptr;
  int64_t best_score = 0, best_distance = 0;
  for (const auto& x : candidates) {
    int64_t score = 0, distance = 0;
    for (size_t b = 0; b < std::min(shapes.size(), x.size()); ++b) {
      const auto theirs = ClipShapes(x[b]);
      const auto& mine = shapes[b];
      for (size_t k = 0; k < std::min(mine.size(), theirs.size()); ++k) {
        if (SameClip(mine[k], theirs[k])) {
          ++score;
          distance += std::llabs(mine[k].samples - theirs[k].samples);
        }
      }
    }
    if (score && (score > best_score || (score == best_score && -distance > -best_distance))) {
      best = &x;
      best_score = score;
      best_distance = distance;
    }
  }
  return best;
}

std::vector<Output> ConvertCps(const std::string& rel, const Bytes& d,
                               const std::function<bool(const std::string&)>& has_base,
                               const CxsDonors& donors, const PcmWriter& write_pcm,
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
    const std::string target = "sound/cxs/" + stem + ".wav";
    if (has_base(Lower(target)))
      return {};
    ++report.counts["music tracks added as WAV"];
    return {{target, GuestWav(data, channels, rate)}};
  }
  const std::string target = "sound/cxs/" + stem + ".cxs";
  if (has_base(Lower(target)))
    return {};
  const Bytes pcm = DecodePsxAdpcm(data, channels);
  const uint32_t frames = uint32_t(pcm.size() / (2 * size_t(channels)));
  const bool loops = loop_end != 0;
  const std::pair<uint32_t, uint32_t> loop{loop_start / (16 * channels) * 28, frames};
  // Any 360 track can carry the tag; the one closest in shape keeps whatever
  // the guest derives from its header nearest the truth.
  const Bytes* donor = nullptr;
  std::pair<bool, int64_t> donor_key;
  for (const auto& [name, cxs] : donors) {
    const std::pair<bool, int64_t> key{(Rd32(cxs, 0x18) != 0) != loops,
                                       std::llabs(int64_t(Rd32(cxs, 0x10)) - frames)};
    if (!donor || key < donor_key) {
      donor = &cxs;
      donor_key = key;
    }
  }
  if (!donor)
    throw std::runtime_error("no 360 .cxs to carry " + target);
  Bytes cxs = *donor;
  const Token tok = MakeToken(Lower(target));
  const size_t at = Rd32(cxs, 0x20);
  Bytes tag(kTag, kTag + 8);
  tag.insert(tag.end(), tok.begin(), tok.end());
  Splice(cxs, at, at + 16, tag);
  write_pcm(tok, PcmWav(pcm, channels, rate, loops ? &loop : nullptr));
  ++report.counts["music tracks added as PCM"];
  return {{target, std::move(cxs)}};
}

}  // namespace eternalsonata::ps3
