#include "ps3_install.h"

#include <algorithm>
#include <array>
#include <cctype>
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
// Bumped with every conversion change, here and in ps3_convert.py.
constexpr const char* kStamp = "ps3-convert.stamp";
constexpr const char* kStampVersion = "2";
// Present while a conversion runs. Inputs are deleted as they are converted,
// so an interrupted run cannot be resumed.
constexpr const char* kBusy = "ps3-converting.stamp";
// The disc's title id, in a name Walk skips so no conversion touches it.
constexpr const char* kTitleId = "ps3-title-id.stamp";
constexpr std::array kNotData = {".i64", ".idb", ".id0", ".id1", ".id2",
                                 ".nam", ".til", ".bak", ".orig", ".stamp"};

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
    WriteFile(root / kShipped, text);
  }
  std::map<std::string, std::string> out;
  for (std::string& rel : files)
    out[Lower(rel)] = std::move(rel);
  return out;
}

void ReplaceFile(const fs::path& path, const Bytes& data) {
  // Unlink first: an older converter hard linked files in from a 360 copy.
  std::error_code ec;
  if (fs::exists(fs::symlink_status(path, ec)))
    fs::remove(path, ec);
  fs::create_directories(path.parent_path(), ec);
  WriteFile(path, data);
}

// The shipped file's bytes. An older converter kept them in a .orig.
Bytes ReadShipped(const fs::path& path) {
  std::error_code ec;
  const fs::path original = WithSuffix(path, kOriginal);
  Bytes out;
  if (!ReadFile(fs::exists(original, ec) ? original : path, out))
    throw std::runtime_error("Could not read " + path.string() + ".");
  return out;
}

// Drops a converted input so the tree never holds both it and its output.
void DiscardShipped(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
  fs::remove(WithSuffix(path, kOriginal), ec);
}

// index.vmtoc as ps3_convert.py's Toc writes it: 48 byte records sorted by
// name, binary searched by sub_8210D080.
class TocBuilder {
 public:
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
  if (fs::exists(root / kBusy, ec))
    return "An earlier conversion was interrupted. Unpack the game files again.";
  WriteFile(root / kBusy, {});
  const auto shipped = ShippedFiles(root);
  // Start from the unpacked tree: an earlier run's outputs, or files an older
  // converter linked in from a 360 copy, would linger otherwise.
  for (const std::string& rel : Walk(root)) {
    if (rel != kShipped && !shipped.count(Lower(rel)))
      fs::remove(root / rel, ec);
  }
  fs::remove_all(root / "pcm", ec);

  TocBuilder toc;
  ps3::Report report;
  auto write_pcm = [&](const ps3::Token& tok, const Bytes& wav) {
    ReplaceFile(root / "pcm" / (ps3::TokenHex(tok) + ".wav"), wav);
  };
  auto emit = [&](std::string out_rel, const Bytes& data) {
    const std::string low = Lower(out_rel);
    // Converted files keep the PS3 spelling.
    const auto spelled = shipped.find(low);
    if (spelled != shipped.end())
      out_rel = spelled->second;
    ReplaceFile(root / out_rel, data);
    if (!toc.SetStored(out_rel, uint32_t(data.size())))
      report.Warn(out_rel + ": path too long for an index.vmtoc record");
  };

  size_t done = 0;
  for (const auto& [low, rel] : shipped) {
    if (progress)
      progress("Converting game files...", float(done++) / float(shipped.size()), rel);
    const Bytes d = ReadShipped(root / rel);
    // Files served as is stay; converted ones go before their output is written.
    auto discard = [&] { DiscardShipped(root / rel); };
    if (EndsWith(low, ".csf")) {
      discard();
      emit(rel, ps3::ConvertCsf(rel, d, write_pcm, report));
      continue;
    }
    if (EndsWith(low, ".cps")) {
      discard();
      for (const auto& out : ps3::ConvertCps(rel, d, write_pcm, report))
        emit(out.path, out.data);
      continue;
    }
    const ps3::AudioConverter audio = [&](size_t index, const Bytes& bank) {
      return ps3::ConvertCsf(rel + "#" + std::to_string(index), bank, write_pcm, report);
    };
    auto result = ps3::ConvertFile(rel, d, report, &audio);
    if (!result) {
      const std::string name = low.substr(low.find_last_of('/') + 1);
      const size_t dot = name.find_last_of('.');
      ++report.counts["served as is " + (dot == std::string::npos || dot == 0 ? name : name.substr(dot))];
      toc.SetStored(rel, uint32_t(d.size()));
      continue;
    }
    ++report.counts["converted"];
    if (low == ps3::kBattleKeep)
      result->data = ps3::AppendEmptyBattleKeep(result->data, report);
    discard();
    emit(result->path, result->data);
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
  Bytes stamp;
  if (ReadFile(dir / kStamp, stamp))
    return std::string(stamp.begin(), stamp.end()) == kStampVersion;
  // Built by an older ps3_convert.py into a directory of its own: converting
  // it again would convert converted files.
  std::error_code ec;
  return fs::is_regular_file(dir / "index.vmtoc", ec) && !fs::exists(dir / kShipped, ec);
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
  if (!ReadFile(sfo, d) || d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0)
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

std::string UnpackPs3(const fs::path& archives, const fs::path& out_dir,
                      const ExtractProgress& progress) {
  std::error_code ec;
  std::vector<fs::path> inputs;
  for (const auto& entry : fs::directory_iterator(archives, ec)) {
    if (EndsWith(entry.path().filename().string(), ".files"))
      inputs.push_back(entry.path());
  }
  std::sort(inputs.begin(), inputs.end());
  if (inputs.empty())
    return "No PS3 archives (*.files) in " + archives.string() + ".";

  fs::path partial = out_dir;
  partial += ".partial";
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec)
    return "Could not create " + partial.string() + ": " + ec.message();

  std::vector<char> buffer(size_t(1) << 22);
  for (size_t a = 0; a < inputs.size(); ++a) {
    std::ifstream in(inputs[a], std::ios::binary);
    char head[16];
    if (!in.read(head, 16) || std::memcmp(head, "FILE", 4) != 0)
      return inputs[a].filename().string() + " is not a PS3 archive.";
    const Bytes h(head, head + 16);
    const uint32_t count = ps3::Rd32(h, 8);
    struct Entry {
      std::string name;
      uint32_t offset, size;
    };
    std::vector<Entry> entries;
    for (uint32_t i = 0; i < count; ++i) {
      char rec[0x30];
      if (!in.read(rec, sizeof(rec)))
        return inputs[a].filename().string() + " is truncated.";
      const Bytes r(rec, rec + sizeof(rec));
      std::string name(rec, strnlen(rec, 32));
      std::replace(name.begin(), name.end(), '\\', '/');
      entries.push_back({std::move(name), ps3::Rd32(r, 32), ps3::Rd32(r, 36)});
    }
    for (const Entry& e : entries) {
      if (e.name.empty() || e.name.find("..") != std::string::npos || e.name.front() == '/')
        return inputs[a].filename().string() + " names an unsafe path: " + e.name;
      if (progress)
        progress("Unpacking game files...", float(a) / float(inputs.size()), e.name);
      const fs::path dst = partial / e.name;
      fs::create_directories(dst.parent_path(), ec);
      std::ofstream out(dst, std::ios::binary | std::ios::trunc);
      in.seekg(e.offset);
      for (uint64_t left = e.size; left;) {
        const size_t n = size_t(std::min<uint64_t>(left, buffer.size()));
        if (!in.read(buffer.data(), std::streamsize(n)) || !out.write(buffer.data(), std::streamsize(n))) {
          fs::remove_all(partial, ec);
          return "Unpacking " + e.name + " failed. The disc may be damaged, or the disk full.";
        }
        left -= n;
      }
    }
  }
  // archives is PS3_GAME/USRDIR/archives.
  const std::string title_id = ReadTitleId(archives.parent_path().parent_path() / "PARAM.SFO");
  if (!title_id.empty())
    WriteFile(partial / kTitleId, Bytes(title_id.begin(), title_id.end()));
  if (fs::exists(out_dir, ec) && !MoveAside(out_dir))
    return "Could not move the existing " + out_dir.string() + " out of the way.";
  fs::rename(partial, out_dir, ec);
  if (ec)
    return "Could not rename the unpacked files: " + ec.message();
  REXLOG_INFO("Unpacked {} PS3 archives into {}", inputs.size(), out_dir.string());
  return {};
}

}  // namespace eternalsonata
