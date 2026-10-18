#include "ps3_install.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include "ps3_bytes.h"
#include "ps3_convert.h"

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;
using ps3::Bytes;

constexpr const char* kProbe = "pcalg_v1.p3obj";
constexpr const char* kOriginal = ".orig";
// The unpacked PS3 files, listed on the first run: what the conversion added
// must not be taken for PS3 data.
constexpr const char* kShipped = "ps3-shipped.txt";
// Bumped with every conversion change.
constexpr const char* kStamp = "ps3-convert.stamp";
constexpr const char* kStampVersion = "1";
// Written when a conversion starts on freshly unpacked files. A tree converted
// before the current revision lacks it and has to be unpacked again.
constexpr const char* kFresh = "ps3-fresh.stamp";
constexpr const char* kFreshVersion = "1";
// Present while a conversion runs: the tree needs another.
constexpr const char* kBusy = "ps3-converting.stamp";
// The outputs written over their shipped file, as "size crc32 path" lines.
// Every same name format keeps its PS3 magic, so only a match says a file is
// already converted.
constexpr const char* kConverted = "ps3-converted.txt";
// The disc's title id, in a name Walk skips so no conversion touches it.
constexpr const char* kTitleId = "ps3-title-id.stamp";
constexpr std::array kNotData = {".i64", ".idb", ".id0", ".id1", ".id2",
                                 ".nam", ".til", ".bak", ".orig", ".stamp",
                                 "reprise-version.toml"};

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool ReadFile(const fs::path& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

void WriteFile(const fs::path& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!out)
    throw std::runtime_error("Could not write " + path.string() + ".");
}

fs::path WithSuffix(fs::path path, const char* suffix) {
  path += suffix;
  return path;
}

// Written aside and renamed over, so the old file stays whole until the new
// one is. Renaming also unlinks what an older converter hard linked in from a
// 360 copy.
void ReplaceFile(const fs::path& path, const Bytes& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path partial = WithSuffix(path, ".partial");
  WriteFile(partial, data);
  fs::rename(partial, path);
}

// Relative paths of the data files under `root`, '/' separated.
std::vector<std::string> Walk(const fs::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (name.starts_with("."))
        it.disable_recursion_pending();
      continue;
    }
    const std::string low = Lower(name);
    if (name.starts_with(".") ||
        std::any_of(kNotData.begin(), kNotData.end(), [&](const char* e) { return EndsWith(low, e); }))
      continue;
    out.push_back(it->path().lexically_relative(root).generic_string());
  }
  return out;
}

// Lowercase relative path -> the PS3 spelling, for the files as unpacked.
std::map<std::string, std::string> ShippedFiles(const fs::path& root) {
  std::vector<std::string> files;
  Bytes list;
  if (ReadFile(root / kShipped, list)) {
    std::string line;
    for (uint8_t c : list) {
      if (c == '\n') {
        if (!line.empty())
          files.push_back(line);
        line.clear();
      } else if (c != '\r') {
        line += char(c);
      }
    }
    if (!line.empty())
      files.push_back(line);
  } else {
    for (std::string& rel : Walk(root)) {
      if (rel != kShipped)
        files.push_back(std::move(rel));
    }
    std::sort(files.begin(), files.end());
    Bytes text;
    for (const std::string& rel : files) {
      ps3::Append(text, rel.data(), rel.size());
      text.push_back('\n');
    }
    ReplaceFile(root / kShipped, text);
  }
  std::map<std::string, std::string> out;
  for (std::string& rel : files)
    out[Lower(rel)] = std::move(rel);
  return out;
}

uint32_t Crc32(const Bytes& d) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : d)
    c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return ~c;
}

std::string FileKey(const Bytes& d) {
  char text[32];
  std::snprintf(text, sizeof(text), "%zu %08x", d.size(), Crc32(d));
  return text;
}

// kConverted, appended to before each output replaces its input.
class ConvertedList {
 public:
  explicit ConvertedList(fs::path path) : path_(std::move(path)) {
    Bytes text;
    ReadFile(path_, text);
    std::string line;
    for (uint8_t c : text) {
      if (c != '\n') {
        line += char(c);
        continue;
      }
      const size_t space = line.find(' ', line.find(' ') + 1);
      if (space != std::string::npos)
        keys_[line.substr(space + 1)] = line.substr(0, space);
      line.clear();
    }
  }

  bool Matches(const std::string& low, const Bytes& d) const {
    const auto it = keys_.find(low);
    return it != keys_.end() && it->second == FileKey(d);
  }

  void Add(const std::string& low, const Bytes& d) {
    const std::string& key = keys_[low] = FileKey(d);
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    out << key << ' ' << low << '\n';
    if (!out)
      throw std::runtime_error("Could not write " + path_.string() + ".");
  }

 private:
  fs::path path_;
  std::unordered_map<std::string, std::string> keys_;
};

// An older build left no kConverted: whatever lies under a shipped name it
// converts in place is its output.
void ListConvertedTree(const fs::path& root, const std::map<std::string, std::string>& shipped,
                       const ExtractProgress& progress) {
  std::error_code ec;
  Bytes text;
  size_t done = 0;
  for (const auto& [low, rel] : shipped) {
    if (progress)
      progress("Checking game files...", float(done++) / float(shipped.size()), rel);
    Bytes d;
    if (!ps3::RenamedOutputs(rel).empty() || fs::exists(WithSuffix(root / rel, kOriginal), ec) ||
        !ReadFile(root / rel, d))
      continue;
    const std::string line = FileKey(d) + ' ' + low + '\n';
    ps3::Append(text, line.data(), line.size());
  }
  ReplaceFile(root / kConverted, text);
}

// index.vmtoc: 48 byte records sorted by
// name, binary searched by sub_8210D080.
class TocBuilder {
 public:
  // Keeps the records of `old` whose files are still under `root`, such as
  // what the patch phase added after an earlier run.
  void KeepExisting(const fs::path& root, const Bytes& old) {
    std::error_code ec;
    for (size_t at = 0; at + 48 <= old.size(); at += 48) {
      Record rec;
      std::copy_n(old.begin() + at, 48, rec.begin());
      const char* text = reinterpret_cast<const char*>(rec.data());
      std::string name(text, strnlen(text, 32));
      std::replace(name.begin(), name.end(), '\\', '/');
      if (fs::is_regular_file(root / name, ec))
        records_.emplace(Lower(name), rec);
    }
  }

  bool SetStored(const std::string& path, uint32_t size) {
    const std::string key = Lower(path);
    auto it = records_.find(key);
    if (it == records_.end()) {
      std::string name = key;
      std::replace(name.begin(), name.end(), '/', '\\');
      if (name.size() >= 32)
        return false;
      Record rec{};
      std::memcpy(rec.data(), name.data(), name.size());
      it = records_.emplace(key, rec).first;
    }
    Record& rec = it->second;
    for (int i = 0; i < 4; ++i)
      rec[32 + i] = uint8_t(size >> (24 - 8 * i));
    rec[36] = 0;
    return true;
  }

  Bytes bytes() const {
    std::vector<const Record*> sorted;
    for (const auto& [_, rec] : records_)
      sorted.push_back(&rec);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Record* a, const Record* b) {
      return std::memcmp(a->data(), b->data(), 32) < 0;
    });
    Bytes out;
    for (const Record* rec : sorted)
      out.insert(out.end(), rec->begin(), rec->end());
    return out;
  }

 private:
  using Record = std::array<uint8_t, 48>;
  std::unordered_map<std::string, Record> records_;
};

std::string Run(const fs::path& root, const ExtractProgress& progress) {
  std::error_code ec;
  const auto shipped = ShippedFiles(root);
  if (!fs::exists(root / kConverted, ec) && !fs::exists(root / kStamp, ec))
    WriteFile(root / kFresh, Bytes(kFreshVersion, kFreshVersion + std::strlen(kFreshVersion)));
  if (!fs::exists(root / kConverted, ec)) {
    // An older build deleted inputs before converting them.
    if (fs::exists(root / kBusy, ec))
      return "An earlier conversion was interrupted. Unpack the game files again.";
    if (fs::exists(root / kStamp, ec))
      ListConvertedTree(root, shipped, progress);
    else
      ReplaceFile(root / kConverted, {});
  }
  WriteFile(root / kBusy, {});
  ConvertedList converted(root / kConverted);

  TocBuilder toc;
  if (Bytes old; ReadFile(root / "index.vmtoc", old))
    toc.KeepExisting(root, old);
  ps3::Report report;
  auto write_pcm = [&](const ps3::Token& tok, const Bytes& wav) {
    ReplaceFile(root / "pcm" / (ps3::TokenHex(tok) + ".wav"), wav);
  };
  auto keep = [&](const std::string& rel, size_t size) {
    if (!toc.SetStored(rel, uint32_t(size)))
      report.Warn(rel + ": path too long for an index.vmtoc record");
  };
  // Converted files keep the PS3 spelling.
  auto spell = [&](const std::string& rel) {
    const auto it = shipped.find(Lower(rel));
    return it == shipped.end() ? rel : it->second;
  };
  // Returns whether the output went over a shipped file.
  auto emit = [&](const std::string& out_rel, const Bytes& data) {
    const std::string rel = spell(out_rel);
    const std::string low = Lower(rel);
    const bool over_shipped = shipped.count(low) != 0;
    if (over_shipped)
      converted.Add(low, data);
    ReplaceFile(root / rel, data);
    keep(rel, data.size());
    return over_shipped;
  };
  // An input an earlier run converted under another name. A .cxs is rebuilt,
  // since its header holds all the converter needs.
  auto keep_renamed = [&](const std::vector<std::string>& outputs) {
    for (const std::string& out : outputs) {
      const std::string rel = spell(out);
      Bytes d;
      if (!ReadFile(root / rel, d))
        continue;
      ++report.counts["already converted"];
      if (const auto cxs = ps3::RebuildCxs(rel, d); cxs && *cxs != d) {
        ++report.counts["music tracks rebuilt"];
        emit(rel, *cxs);
      } else {
        keep(rel, d.size());
      }
      return true;
    }
    return false;
  };

  size_t done = 0;
  for (const auto& [low, rel] : shipped) {
    if (progress)
      progress("Converting game files...", float(done++) / float(shipped.size()), rel);
    const fs::path path = root / rel;
    // An older converter kept the shipped bytes in a .orig.
    const fs::path original = WithSuffix(path, kOriginal);
    const auto renamed = ps3::RenamedOutputs(rel);
    Bytes d;
    const bool from_original = ReadFile(original, d);
    if (!from_original && !ReadFile(path, d)) {
      if (!keep_renamed(renamed))
        report.Warn(rel + ": neither the PS3 file nor its conversion is there");
      continue;
    }
    if (!from_original && renamed.empty() && converted.Matches(low, d)) {
      ++report.counts["already converted"];
      if (low == ps3::kBattleKeep) {
        if (Bytes aligned = ps3::AlignBattleKeep(d); aligned != d) {
          ++report.counts["BattleKeep banks realigned"];
          emit(rel, aligned);
          continue;
        }
      }
      keep(rel, d.size());
      continue;
    }
    std::vector<ps3::Output> outputs;
    if (EndsWith(low, ".csf")) {
      outputs.push_back({rel, ps3::ConvertCsf(rel, d, write_pcm, report)});
    } else if (EndsWith(low, ".cps")) {
      outputs = ps3::ConvertCps(rel, d, write_pcm, report);
    } else {
      const ps3::AudioConverter audio = [&](size_t index, const Bytes& bank) {
        return ps3::ConvertCsf(rel + "#" + std::to_string(index), bank, write_pcm, report);
      };
      auto result = ps3::ConvertFile(rel, d, report, &audio);
      if (!result) {
        const std::string name = low.substr(low.find_last_of('/') + 1);
        const size_t dot = name.find_last_of('.');
        ++report.counts["served as is " + (dot == std::string::npos || dot == 0 ? name : name.substr(dot))];
        keep(rel, d.size());
        continue;
      }
      ++report.counts["converted"];
      if (low == ps3::kBattleKeep)
        result->data = ps3::AppendEmptyBattleKeep(result->data, report);
      outputs.push_back(std::move(*result));
    }
    bool replaced = false;
    for (const ps3::Output& out : outputs)
      replaced |= emit(out.path, out.data);
    // The input goes only once its output is safely written.
    if (!replaced)
      fs::remove(path, ec);
    fs::remove(original, ec);
  }
  ReplaceFile(root / "index.vmtoc", toc.bytes());
  ReplaceFile(root / kStamp, Bytes(kStampVersion, kStampVersion + std::strlen(kStampVersion)));
  fs::remove(root / kBusy, ec);

  for (const auto& [what, count] : report.counts)
    REXLOG_INFO("PS3 conversion: {:8}  {}", count, what);
  for (const std::string& warning : report.warnings)
    REXLOG_WARN("PS3 conversion: {}", warning);
  return {};
}

bool SameName(const std::string& a, const std::string& b) {
  return Lower(a) == Lower(b);
}

// A child of `dir` named `name` in any case.
fs::path Child(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (SameName(entry.path().filename().string(), name))
      return entry.path();
  }
  return {};
}

bool HasArchives(const fs::path& dir) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (EndsWith(Lower(entry.path().filename().string()), ".files"))
      return true;
  }
  return false;
}

std::string UnpackPs3Files(const std::vector<ImageFile>& inputs, const std::string& title_id,
                           const fs::path& out_dir, const ExtractProgress& progress) {
  std::error_code ec;
  if (inputs.empty())
    return "No PS3 archives (*.files) found.";
  std::vector<const ImageFile*> sorted;
  for (const ImageFile& f : inputs)
    sorted.push_back(&f);
  std::sort(sorted.begin(), sorted.end(),
            [](const ImageFile* a, const ImageFile* b) { return a->name < b->name; });

  fs::path partial = out_dir;
  partial += ".partial";
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec)
    return "Could not create " + partial.string() + ": " + ec.message();

  std::vector<char> buffer(size_t(1) << 22);
  for (size_t a = 0; a < sorted.size(); ++a) {
    const ImageFile& in = *sorted[a];
    char head[16];
    if (!in.read(0, head, 16) || std::memcmp(head, "FILE", 4) != 0)
      return in.name + " is not a PS3 archive.";
    const Bytes h(head, head + 16);
    const uint32_t count = ps3::Rd32(h, 8);
    struct Entry {
      std::string name;
      uint32_t offset, size;
    };
    std::vector<Entry> entries;
    for (uint32_t i = 0; i < count; ++i) {
      char rec[0x30];
      if (!in.read(16 + uint64_t(i) * sizeof(rec), rec, sizeof(rec)))
        return in.name + " is truncated.";
      const Bytes r(rec, rec + sizeof(rec));
      std::string name(rec, strnlen(rec, 32));
      std::replace(name.begin(), name.end(), '\\', '/');
      entries.push_back({std::move(name), ps3::Rd32(r, 32), ps3::Rd32(r, 36)});
    }
    for (const Entry& e : entries) {
      if (e.name.empty() || e.name.find("..") != std::string::npos || e.name.front() == '/')
        return in.name + " names an unsafe path: " + e.name;
      if (progress)
        progress("Unpacking game files...", float(a) / float(sorted.size()), e.name);
      const fs::path dst = partial / e.name;
      fs::create_directories(dst.parent_path(), ec);
      std::ofstream out(dst, std::ios::binary | std::ios::trunc);
      uint64_t at = e.offset;
      for (uint64_t left = e.size; left;) {
        const size_t n = size_t(std::min<uint64_t>(left, buffer.size()));
        if (!in.read(at, buffer.data(), n) || !out.write(buffer.data(), std::streamsize(n))) {
          fs::remove_all(partial, ec);
          return "Unpacking " + e.name + " failed. The disc may be damaged, or the disk full.";
        }
        at += n;
        left -= n;
      }
    }
  }
  if (!title_id.empty())
    WriteFile(partial / kTitleId, Bytes(title_id.begin(), title_id.end()));
  if (fs::exists(out_dir, ec) && !MoveAside(out_dir))
    return "Could not move the existing " + out_dir.string() + " out of the way.";
  fs::rename(partial, out_dir, ec);
  if (ec)
    return "Could not rename the unpacked files: " + ec.message();
  REXLOG_INFO("Unpacked {} PS3 archives into {}", sorted.size(), out_dir.string());
  return {};
}

}  // namespace

bool IsPs3Directory(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / kProbe, ec);
}

Ps3Region ReadPs3Region(const fs::path& dir) {
  Bytes id;
  if (!ReadFile(dir / kTitleId, id) || id.size() < 3)
    return Ps3Region::kPal;
  // BLES/BCES Europe, BLUS/BCUS America, BLJS/BCJS/BLJM/BCJM Japan.
  if (id[2] == 'U')
    return Ps3Region::kUsa;
  if (id[2] == 'J')
    return Ps3Region::kJapan;
  return Ps3Region::kPal;
}

bool IsPs3Converted(const fs::path& dir) {
  std::error_code ec;
  Bytes stamp;
  if (ReadFile(dir / kStamp, stamp))
    return std::string(stamp.begin(), stamp.end()) == kStampVersion && !fs::exists(dir / kBusy, ec);
  // Built by the old Python converter into a directory of its own: converting
  // it again would convert converted files.
  return fs::is_regular_file(dir / "index.vmtoc", ec) && !fs::exists(dir / kShipped, ec);
}

bool IsPs3Outdated(const fs::path& dir) {
  Bytes mark;
  return IsPs3Directory(dir) &&
         !(ReadFile(dir / kFresh, mark) && std::string(mark.begin(), mark.end()) == kFreshVersion);
}

std::string ConvertPs3(const fs::path& dir, const ExtractProgress& progress) {
  try {
    return Run(dir, progress);
  } catch (const std::exception& e) {
    return std::string("Converting the PS3 files failed: ") + e.what();
  }
}

fs::path FindPs3Archives(const fs::path& picked) {
  fs::path at = picked;
  for (const char* part : {"PS3_GAME", "USRDIR", "archives"}) {
    if (SameName(at.filename().string(), part))
      continue;
    if (fs::path next = Child(at, part); !next.empty())
      at = next;
  }
  return SameName(at.filename().string(), "archives") && HasArchives(at) ? at : fs::path();
}

std::string ReadTitleId(const fs::path& sfo) {
  Bytes d;
  return ReadFile(sfo, d) ? TitleIdFromSfo(d) : std::string();
}

std::string TitleIdFromSfo(const Bytes& d) {
  if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0)
    return {};
  auto le32 = [&](size_t o) {
    return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 |
           uint32_t(d[o + 3]) << 24;
  };
  const size_t keys = le32(8), values = le32(12), count = le32(16);
  for (size_t i = 0; i < count && 20 + 16 * (i + 1) <= d.size(); ++i) {
    const size_t entry = 20 + 16 * i;
    const size_t key = keys + (size_t(d[entry]) | size_t(d[entry + 1]) << 8);
    const size_t value = values + le32(entry + 12);
    if (key >= d.size() || value >= d.size())
      continue;
    if (std::strncmp(reinterpret_cast<const char*>(&d[key]), "TITLE_ID", 9) == 0)
      return std::string(reinterpret_cast<const char*>(&d[value]),
                         strnlen(reinterpret_cast<const char*>(&d[value]), d.size() - value));
  }
  return {};
}

uint64_t Ps3ArchiveBytes(const fs::path& archives) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(archives, ec)) {
    std::error_code size_ec;
    if (EndsWith(Lower(entry.path().filename().string()), ".files"))
      total += entry.file_size(size_ec);
  }
  return total;
}

std::string UnpackPs3(const fs::path& archives, const fs::path& out_dir,
                      const ExtractProgress& progress) {
  std::error_code ec;
  std::vector<fs::path> inputs;
  for (const auto& entry : fs::directory_iterator(archives, ec)) {
    if (EndsWith(Lower(entry.path().filename().string()), ".files"))
      inputs.push_back(entry.path());
  }
  std::sort(inputs.begin(), inputs.end());
  std::vector<std::ifstream> streams;
  streams.reserve(inputs.size());  // the readers below keep pointers into this vector
  std::vector<ImageFile> files;
  for (const fs::path& input : inputs) {
    streams.emplace_back(input, std::ios::binary);
    std::ifstream* in = &streams.back();
    files.push_back({input.filename().string(), fs::file_size(input, ec),
                     [in](uint64_t off, void* dst, uint64_t len) {
                       in->clear();
                       in->seekg(std::streamoff(off));
                       return bool(in->read(static_cast<char*>(dst), std::streamsize(len)));
                     }});
  }
  // archives is PS3_GAME/USRDIR/archives.
  return UnpackPs3Files(files, ReadTitleId(archives.parent_path().parent_path() / "PARAM.SFO"),
                        out_dir, progress);
}

std::string UnpackPs3Image(const std::string& image, const fs::path& out_dir,
                           const ExtractProgress& progress) {
  std::vector<uint8_t> sfo;
  ReadIsoImageFile(image, "PS3_GAME/PARAM.SFO", sfo);
  const std::string title_id = TitleIdFromSfo(sfo);
  return WithIsoFiles(image, "PS3_GAME/USRDIR/archives", ".files",
                      [&](const std::vector<ImageFile>& files) {
                        return UnpackPs3Files(files, title_id, out_dir, progress);
                      });
}

}  // namespace eternalsonata
