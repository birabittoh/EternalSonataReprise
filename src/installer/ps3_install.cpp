#include "ps3_install.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include "eternalsonata_asset_container.h"
#include "ps3_bytes.h"
#include "ps3_convert.h"

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;
using ps3::Bytes;

constexpr const char* kProbe = "pcalg_v1.p3obj";
constexpr const char* kOriginal = ".orig";
// The unpacked PS3 files, listed on the first run: what later runs added has
// no .orig and must not be taken for PS3 data.
constexpr const char* kShipped = "ps3-shipped.txt";
// Bumped with every conversion change, here and in ps3_convert.py.
constexpr const char* kStamp = "ps3-convert.stamp";
constexpr const char* kStampVersion = "1";
constexpr std::array kNotData = {".i64", ".idb", ".id0", ".id1", ".id2",
                                 ".nam", ".til", ".bak", ".orig", ".stamp"};
// Never served from the game directory: the guest image is built in, and the
// release patch bundle is the 360 installer's.
constexpr std::array kNotDonor = {"index.vmtoc", "default.xex", "release-patches.bin"};

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

void MakeRoom(const fs::path& path, bool shipped) {
  // A shipped file is kept beside its replacement. Anything else may be a
  // hard link to the 360 tree: unlink it so a write never reaches the 360 file.
  std::error_code ec;
  const fs::path original = WithSuffix(path, kOriginal);
  if (shipped && !fs::exists(original, ec)) {
    fs::rename(path, original, ec);
    if (ec)
      throw std::runtime_error("Could not keep the original " + path.string() + " (" +
                               ec.message() + ").");
  } else if (fs::exists(fs::symlink_status(path, ec))) {
    fs::remove(path, ec);
  }
  fs::create_directories(path.parent_path(), ec);
}

void ReplaceFile(const fs::path& path, const Bytes& data, bool shipped = false) {
  MakeRoom(path, shipped);
  WriteFile(path, data);
}

void LinkFile(const fs::path& src, const fs::path& dst, bool shipped = false) {
  MakeRoom(dst, shipped);
  std::error_code ec;
  fs::create_hard_link(src, dst, ec);
  if (ec) {
    ec.clear();
    fs::copy_file(src, dst, ec);
    if (ec)
      throw std::runtime_error("Could not copy " + src.string() + " (" + ec.message() + ").");
  }
}

// The shipped file's bytes, from before any earlier run.
Bytes ReadShipped(const fs::path& path) {
  std::error_code ec;
  const fs::path original = WithSuffix(path, kOriginal);
  Bytes out;
  if (!ReadFile(fs::exists(original, ec) ? original : path, out))
    throw std::runtime_error("Could not read " + path.string() + ".");
  return out;
}

// index.vmtoc as ps3_convert.py's Toc writes it: 48 byte records sorted by
// name, binary searched by sub_8210D080.
class TocBuilder {
 public:
  bool Load(const fs::path& path) {
    Bytes data;
    if (!ReadFile(path, data))
      return false;
    for (size_t i = 0; i + 48 <= data.size(); i += 48) {
      Record rec;
      std::memcpy(rec.data(), data.data() + i, 48);
      const size_t n = strnlen(reinterpret_cast<const char*>(rec.data()), 32);
      std::string key = Lower(std::string(reinterpret_cast<const char*>(rec.data()), n));
      std::replace(key.begin(), key.end(), '\\', '/');
      records_[key] = rec;
    }
    return true;
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

// Embedded banks: each pairs with a bank of the 360 container of the same
// name, found by clip shapes.
class EmbeddedAudio {
 public:
  EmbeddedAudio(std::function<std::optional<Bytes>(const std::string&)> decode360,
                ps3::PcmWriter write_pcm, ps3::Report& report)
      : decode360_(std::move(decode360)), write_pcm_(std::move(write_pcm)), report_(report) {}

  Bytes Convert(const std::string& rel, size_t index, const Bytes& bank,
                const ps3::BankMember* member) {
    const std::string low = Lower(rel);
    Twin& twin_file = Load(low);
    const Bytes* twin = nullptr;
    if (member) {
      // Each language is a directory of banks, and a bank alone can match the
      // other language's, so directories are paired whole.
      const auto key = std::make_pair(low, member->directory);
      auto it = paired_.find(key);
      if (it == paired_.end())
        it = paired_.emplace(key, ps3::BestGroup(*member->group, twin_file.groups)).first;
      const std::vector<Bytes>* x = it->second;
      if (x && member->position < x->size())
        twin = &(*x)[member->position];
    }
    if (!twin)
      twin = ps3::BestTwin(bank, twin_file.banks, index);
    return ps3::ConvertCsf(rel + "#" + std::to_string(index), bank, twin, write_pcm_, report_);
  }

 private:
  struct Twin {
    std::vector<Bytes> banks;
    std::vector<std::vector<Bytes>> groups;
  };

  Twin& Load(const std::string& low) {
    auto it = twins_.find(low);
    if (it != twins_.end())
      return it->second;
    Twin& t = twins_[low];
    if (const auto x = decode360_(low)) {
      for (size_t o = 0; o + 16 < x->size(); o += 0x1000) {
        if (ps3::IsCsf(*x, o, x->size()))
          t.banks.push_back(ps3::Slice(*x, o, o + ps3::Rd32(*x, o + 4)));
      }
      for (size_t o = 0; o + 16 < x->size(); o += 0x1000) {
        if (!ps3::IsCsl(*x, o, x->size()))
          continue;
        std::vector<Bytes> group;
        for (size_t b : ps3::CslBanks(*x, o))
          group.push_back(ps3::Slice(*x, b, b + ps3::Rd32(*x, b + 4)));
        t.groups.push_back(std::move(group));
      }
    }
    return t;
  }

  std::function<std::optional<Bytes>(const std::string&)> decode360_;
  ps3::PcmWriter write_pcm_;
  ps3::Report& report_;
  std::map<std::string, Twin> twins_;
  std::map<std::pair<std::string, size_t>, const std::vector<Bytes>*> paired_;
};

std::string Run(const fs::path& root, const fs::path& base, const ExtractProgress& progress) {
  std::error_code ec;
  const auto shipped = ShippedFiles(root);
  TocBuilder toc;
  if (!toc.Load(base / "index.vmtoc"))
    return "The Xbox 360 files at " + base.string() + " have no index.vmtoc.";

  // Lowercase path -> the 360 spelling, for every 360 file.
  std::map<std::string, std::string> base_names;
  for (std::string& rel : Walk(base)) {
    const std::string name = Lower(rel.substr(rel.find_last_of('/') + 1));
    if (std::find(kNotDonor.begin(), kNotDonor.end(), name) == kNotDonor.end())
      base_names[Lower(rel)] = std::move(rel);
  }
  if (progress)
    progress("Converting game files...", -1.0f, "Adding the Xbox 360 files");
  for (const auto& [low, rel] : base_names) {
    if (!shipped.count(low))
      LinkFile(base / rel, root / rel);
  }
  // Converted files keep the PS3 spelling, added ones the 360's.
  auto names = base_names;
  for (const auto& [low, rel] : shipped)
    names[low] = rel;
  fs::remove_all(root / "pcm", ec);

  assets::Toc base_toc;
  if (!base_toc.Load(base / "index.vmtoc"))
    return "The Xbox 360 index.vmtoc could not be read.";
  auto decode360 = [&](const std::string& low) -> std::optional<Bytes> {
    const assets::TocEntry* entry = base_toc.Find(low);
    if (!entry)
      return std::nullopt;
    const auto spelled = base_names.find(low);
    Bytes encoded;
    if (!ReadFile(base / (spelled != base_names.end() ? spelled->second : low), encoded))
      return std::nullopt;
    if (entry->flag == 0)
      return encoded;
    Bytes out;
    if (!assets::DecodeAsset(encoded.data(), encoded.size(), entry->size, entry->flag, out))
      throw std::runtime_error("Could not decode the Xbox 360 " + low + ".");
    return out;
  };

  ps3::CxsDonors donors;
  for (const auto& entry : fs::directory_iterator(base / "sound" / "cxs", ec)) {
    const std::string name = entry.path().filename().string();
    Bytes d;
    if (EndsWith(Lower(name), ".cxs") && ReadFile(entry.path(), d) && d.size() >= 4 &&
        std::memcmp(d.data(), "CXS ", 4) == 0)
      donors.emplace_back(name, std::move(d));
  }
  const auto battlekeep = decode360(ps3::kBattleKeep);
  if (!battlekeep)
    return "The Xbox 360 files have no BattleKeep.bop.";

  ps3::Report report;
  auto write_pcm = [&](const ps3::Token& tok, const Bytes& wav) {
    ReplaceFile(root / "pcm" / (ps3::TokenHex(tok) + ".wav"), wav);
  };
  EmbeddedAudio embedded(decode360, write_pcm, report);
  auto emit = [&](std::string out_rel, const Bytes& data) {
    std::string low = Lower(out_rel);
    if (!base_names.count(low))
      ++report.counts["new files"];
    if (const auto it = names.find(low); it != names.end())
      out_rel = it->second;
    ReplaceFile(root / out_rel, data, shipped.count(low) != 0);
    if (!toc.SetStored(out_rel, uint32_t(data.size())))
      report.Warn(out_rel + ": path too long for an index.vmtoc record");
  };

  size_t done = 0;
  for (const auto& [low, rel] : shipped) {
    if (progress)
      progress("Converting game files...", float(done++) / float(shipped.size()), rel);
    const fs::path path = root / rel;
    const Bytes d = ReadShipped(path);
    if (EndsWith(low, ".csf")) {
      const auto x360 = decode360(low);
      emit(rel, ps3::ConvertCsf(rel, d, x360 ? &*x360 : nullptr, write_pcm, report));
      continue;
    }
    if (EndsWith(low, ".cps")) {
      const auto has_base = [&](const std::string& p) { return base_names.count(p) != 0; };
      for (const auto& out : ps3::ConvertCps(rel, d, has_base, donors, write_pcm, report))
        emit(out.path, out.data);
      continue;
    }
    const ps3::AudioConverter audio = [&](size_t index, const Bytes& bank,
                                          const ps3::BankMember* member) {
      return embedded.Convert(rel, index, bank, member);
    };
    auto result = ps3::ConvertFile(rel, d, report, &audio);
    const std::string ext = [&] {
      const std::string name = low.substr(low.find_last_of('/') + 1);
      const size_t dot = name.find_last_of('.');
      return dot == std::string::npos || dot == 0 ? name : name.substr(dot);
    }();
    if (!result) {
      const auto twin = base_names.find(low);
      if (twin == base_names.end()) {
        ++report.counts["kept PS3 " + ext];
        continue;
      }
      ++report.counts["kept 360 " + ext];
      LinkFile(base / twin->second, path, true);
      continue;
    }
    ++report.counts["converted"];
    if (low == ps3::kBattleKeep)
      result->data = ps3::AppendDroppedBattleKeep(result->data, *battlekeep, report);
    emit(result->path, result->data);
  }
  ReplaceFile(root / "index.vmtoc", toc.bytes());
  ReplaceFile(root / kStamp, Bytes(kStampVersion, kStampVersion + std::strlen(kStampVersion)));

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

bool IsPs3Converted(const fs::path& dir) {
  Bytes stamp;
  if (ReadFile(dir / kStamp, stamp))
    return std::string(stamp.begin(), stamp.end()) == kStampVersion;
  // Built by an older ps3_convert.py into a directory of its own: converting
  // it again would convert converted files.
  std::error_code ec;
  return fs::is_regular_file(dir / "index.vmtoc", ec) && !fs::exists(dir / kShipped, ec);
}

std::string ConvertPs3(const fs::path& dir, const fs::path& donor, const ExtractProgress& progress) {
  try {
    return Run(dir, donor, progress);
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

std::string UnpackPs3(const fs::path& archives, const fs::path& out_dir, const fs::path& case_from,
                      const ExtractProgress& progress) {
  std::error_code ec;
  std::unordered_map<std::string, std::string> casing;
  if (!case_from.empty()) {
    for (auto it = fs::recursive_directory_iterator(case_from, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
      const std::string rel = it->path().lexically_relative(case_from).generic_string();
      casing[Lower(rel)] = rel;
    }
  }
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
      const auto spelled = casing.find(Lower(e.name));
      const fs::path dst = partial / (spelled != casing.end() ? spelled->second : e.name);
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
  if (fs::exists(out_dir, ec) && !MoveAside(out_dir))
    return "Could not move the existing " + out_dir.string() + " out of the way.";
  fs::rename(partial, out_dir, ec);
  if (ec)
    return "Could not rename the unpacked files: " + ec.message();
  REXLOG_INFO("Unpacked {} PS3 archives into {}", inputs.size(), out_dir.string());
  return {};
}

}  // namespace eternalsonata
