// PS3 audio conversion.
//
// PS3 sound banks hold mono ATRAC3 clips (192 byte frames) and music in
// PS-ADPCM where the 360 holds XMA2. Both are decoded and encoded as XMA2
// (xma2enc), so the guest's own decoder plays them.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include <rex/audio/atrac3.h>
#include <xma2enc/xma2enc.h>

#include "ps3_bytes.h"
#include "ps3_convert.h"

namespace eternalsonata::ps3 {
namespace {

constexpr size_t kPacket = xma2enc::kPacketSize;
constexpr uint32_t kAtrac3Frame = 192;
constexpr uint32_t kAtrac3Samples = 1024;

// The 360's own clips run at about 128 kbps mono; the ATRAC3 source is 66.
constexpr uint32_t kClipBitrate = 96000;
constexpr uint32_t kMusicBitrate = 192000;

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

xma2enc::Result EncodeXma(const std::vector<int16_t>& pcm, uint32_t channels, uint32_t rate,
                          uint32_t block_size, uint32_t bitrate) {
  xma2enc::Options options;
  options.bitrate = bitrate * channels;
  options.block_size = block_size;
  xma2enc::Result out;
  if (!xma2enc::Encode(pcm.data(), pcm.size() / channels, channels, rate, options, &out))
    throw std::runtime_error("XMA2 encoding failed");
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

// A 360 TIM for a converted clip: the PS3 clip's parameters, LIP and rate
// around the encoded stream, with its XMA2 header and seek table.
Bytes NewTim(const Tim& ps3, uint32_t offset, const xma2enc::Result& xma, const Loop* loop) {
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
  const uint32_t encoded = xma.frames * xma2enc::kFrameSamples;
  Bytes header;
  for (uint32_t v : {loop ? 0x030100FFu : 0x03010000u, loop ? loop->first : 0u,
                     loop ? loop->second : 0u, ps3.rate, xma.block_size, encoded, xma.samples,
                     xma.block_count, 0x01000001u})
    Put32(header, v);
  Bytes seek;
  for (uint32_t v : xma.seek_table)
    Put32(seek, v);
  const size_t lip_off = lip.empty() ? 0 : 0x38 + params.size();
  const size_t xma_off = Align(0x38 + params.size() + lip.size(), 4);
  Bytes out;
  Append(out, "TIM \0\0\0\0", 8);
  for (uint64_t v : {uint64_t(ps3.Kind()), uint64_t(ps3.rate), uint64_t(offset),
                     uint64_t(xma.packets.size()), uint64_t(0), uint64_t(0), uint64_t(lip_off),
                     uint64_t(lip.size()), uint64_t(xma_off), uint64_t(header.size()),
                     uint64_t(xma_off + header.size()), uint64_t(seek.size())})
    Put32(out, v);
  Append(out, params);
  Append(out, lip);
  out.resize(xma_off);
  Append(out, header);
  Append(out, seek);
  Wr32(out, 4, out.size());
  return out;
}

}  // namespace

Bytes ConvertCsf(const std::string& rel, const Bytes& ps3, Report& report) {
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
      std::vector<int16_t> pcm;
      if (!rex::audio::DecodeAtrac3(frames.data(), frames.size(), tim.rate, 1, kAtrac3Frame, pcm) ||
          pcm.empty()) {
        report.Warn(rel + ": clip " + std::to_string(ordinal) + " does not decode, left silent");
        pcm.assign(std::max<uint32_t>(n, 1), 0);
      }
      const bool loops = tim.flags == 1;
      Loop loop;
      if (loops) {
        // The XMA encoder takes loop points on 128 sample steps, from sample 384.
        const uint32_t start = tim.loop_start / kAtrac3Frame * kAtrac3Samples;
        const uint32_t end_frames = tim.loop_end / kAtrac3Frame * kAtrac3Samples;
        const uint32_t end = std::min(n, end_frames ? end_frames : n);
        loop = Loop(start < end ? std::max<uint32_t>(384, start / 128 * 128) : 384,
                    uint32_t(Align(end, 128)));
      }
      const uint32_t block = tim.flags == 0x100 ? 0x1000 : 0x4000;
      const xma2enc::Result xma = EncodeXma(pcm, 1, tim.rate, block, kClipBitrate);
      if (loops)
        loop.second = std::min<uint32_t>(loop.second, xma.frames * xma2enc::kFrameSamples);
      Append(payload, xma.packets);
      Append(blob, NewTim(tim, uint32_t(offset), xma, loops ? &loop : nullptr));
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

// A .cxs as every 360 track has it: the header, one u32 per block with the
// samples played by its end, then the XMA2 packets.
Bytes NewCxs(const xma2enc::Result& xma, uint32_t rate, uint32_t samples, const Loop* loop) {
  Bytes out;
  Append(out, "CXS ", 4);
  for (uint64_t v : {uint64_t(kCxsHeader), uint64_t(rate), uint64_t(xma.channels),
                     uint64_t(samples), uint64_t(loop ? loop->first : 0),
                     uint64_t(loop ? Align(loop->second, 512) : 0), uint64_t(xma.block_count),
                     uint64_t(kCxsPayload), uint64_t(xma.packets.size()), uint64_t(kPacket),
                     uint64_t(kPacket)})
    Put32(out, v);
  out.resize(kCxsHeader);
  for (uint32_t v : xma.seek_table)
    Put32(out, v);
  out.resize(kCxsPayload);
  Append(out, xma.packets);
  return out;
}

std::vector<std::string> CpsOutputs(const std::string& rel) {
  std::string stem = rel.substr(rel.find_last_of('/') + 1);
  if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot)
    stem.resize(dot);
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  return {"sound/cxs/" + stem + ".cxs", "sound/cxs/" + stem + ".wav"};
}

std::vector<Output> ConvertCps(const std::string& rel, const Bytes& d, Report& report) {
  const uint32_t header = Rd32(d, 4), channels = Rd32(d, 8), size = Rd32(d, 12),
                 rate = Rd32(d, 16), loop_start = Rd32(d, 20), loop_end = Rd32(d, 24),
                 kind = Rd32(d, 28);
  const Bytes data = Slice(d, header, size_t(header) + size);
  const auto outputs = CpsOutputs(rel);
  if (kind == 0) {
    // Plain PCM, already big endian: the scripts ask for these as .wav.
    ++report.counts["music tracks as WAV"];
    return {{outputs[1], GuestWav(data, channels, rate)}};
  }
  const std::string& target = outputs[0];
  if (channels < 1 || channels > 2)
    throw std::runtime_error(rel + ": " + std::to_string(channels) + " channel ADPCM music");
  const std::vector<int16_t> pcm = DecodePsxAdpcm(data.data(), data.size(), channels);
  const uint32_t frames = uint32_t(pcm.size() / channels);
  // The XMA encoder takes loop points on 128 sample steps.
  const Loop loop{loop_start / (16 * channels) * 28 / 128 * 128, frames};
  const xma2enc::Result xma = EncodeXma(pcm, channels, rate, kCxsBlock, kMusicBitrate);
  ++report.counts["music tracks"];
  return {{target, NewCxs(xma, rate, frames, loop_end ? &loop : nullptr)}};
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
