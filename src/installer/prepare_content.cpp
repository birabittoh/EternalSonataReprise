#include "prepare_content.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <ostream>
#include <stdexcept>
#include <string_view>

#include "install_pipeline.h"

namespace eternalsonata {
namespace {
namespace fs = std::filesystem;
constexpr const char* kOwner = ".recomplauncher-content";
constexpr const char* kOwnerText = "EternalSonataReprise content v1\n";

fs::path FromUtf8(std::string_view value) {
  return fs::path(std::u8string(value.begin(), value.end()));
}

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool Contains(const fs::path& parent, const fs::path& child) {
  // Ask the filesystem too: Windows and typical macOS volumes are case
  // insensitive, and different spellings can name the same directory.
  for (fs::path at = child; !at.empty(); at = at.parent_path()) {
    std::error_code ec;
    if (parent == at || fs::equivalent(parent, at, ec)) return true;
    if (at == at.parent_path()) break;
  }
  return false;
}

bool Owned(const fs::path& dir) {
  std::ifstream in(dir / kOwner, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {}) == kOwnerText;
}

void CopySource(const fs::path& source, const fs::path& dest) {
  fs::create_directories(dest);
  for (const auto& entry : fs::recursive_directory_iterator(source)) {
    const auto status = entry.symlink_status();
    if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status)))
      throw std::runtime_error("Game folders must contain regular files and directories only.");
    const fs::path target = dest / entry.path().lexically_relative(source);
    if (fs::is_directory(status))
      fs::create_directories(target);
    else
      fs::copy_file(entry.path(), target);
  }
}

std::string OneLine(std::string text) {
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

class Progress {
 public:
  explicit Progress(std::ostream& out) : out_(out) { out_.imbue(std::locale::classic()); }
  void Report(double fraction, const std::string& text) {
    value_ = std::max(value_, std::clamp(fraction, 0.0, 1.0));
    out_ << "progress " << std::fixed << std::setprecision(3) << value_ << ' '
         << OneLine(text) << std::endl;
  }
 private:
  std::ostream& out_;
  double value_ = 0;
};
}  // namespace

std::string PrepareContent(const fs::path& source_path, const fs::path& destination_path,
                           std::ostream& out) {
  fs::path transaction;
  fs::path destination;
  bool moved_old = false;
  bool published = false;
  bool transaction_created = false;
  try {
    const fs::path source = fs::canonical(source_path);
    destination = fs::weakly_canonical(fs::absolute(destination_path));
    if (destination == destination.root_path() || destination.filename().empty() ||
        Contains(source, destination) || Contains(destination, source))
      return "The source and destination must be separate folders.";
    // Never replace a hand-managed install or follow its directory symlink.
    if (fs::is_symlink(fs::symlink_status(destination_path)))
      return "The destination must not be a symbolic link.";
    if (fs::exists(destination) && (!fs::is_directory(destination) || !Owned(destination)))
      return "The destination already exists and was not prepared by RecompLauncher.";

    Progress progress(out);
    progress.Report(0, "Identifying your copy");
    const SourceInfo info = IdentifySource(Utf8(source));
    if (!info.error.empty())
      return info.error;
    if (info.id.empty())
      return "This copy could not be identified as a supported Eternal Sonata release.";
    if (IsOutdated(source))
      return "This folder was converted by an older version. Select the original dump again.";

    fs::create_directories(destination.parent_path());
    transaction = destination;
    transaction += ".preparing";
    // Atomic reservation: another process or an interrupted run is never removed.
    if (!fs::create_directory(transaction)) {
      transaction.clear();
      return "A previous setup is still present. Keep it for recovery or move it aside and retry.";
    }
    const fs::path staged = transaction / "new";
    transaction_created = true;
    const fs::path extracted = ExtractedSource(Utf8(source));
    const uint64_t copy_bytes = [&] {
      uint64_t size = 0;
      if (!extracted.empty())
        for (const auto& entry : fs::recursive_directory_iterator(extracted))
          if (entry.is_regular_file()) size += entry.file_size();
      return size;
    }();
    const auto space = fs::space(transaction);
    if (space.available < info.required_bytes + copy_bytes)
      throw std::runtime_error("There is not enough free space to prepare this copy safely.");
    if (!extracted.empty()) {
      progress.Report(0.02, "Copying game files");
      CopySource(extracted, staged);
    }
    int phase = 0;
    InstallHooks hooks;
    hooks.phase = [&](Phase current, PhaseState state) {
      phase = static_cast<int>(current);
      progress.Report(0.1 + 0.8 * (phase + (state == PhaseState::kDone ||
                                           state == PhaseState::kSkipped ? 1 : 0)) / 3.0,
                      PhaseName(current));
    };
    hooks.progress = [&](const std::string& title, float fraction, const std::string&) {
      progress.Report(0.1 + 0.8 * (phase + (std::isfinite(fraction) ?
                         std::clamp(double(fraction), 0.0, 1.0) : 0.0)) / 3.0, title);
    };
    fs::path ready;
    const std::string error = Install(extracted.empty() ? Utf8(source) : Utf8(staged),
                                      staged, hooks, ready);
    if (!error.empty()) throw std::runtime_error(error);
    if (!IsGameDirectory(ready) || NeedsConversion(ready) || NeedsPatching(ready) || IsOutdated(ready))
      throw std::runtime_error("The prepared game files are incomplete.");
    {
      std::ofstream marker(staged / kOwner, std::ios::binary);
      marker << kOwnerText;
      marker.close();
      if (!marker) throw std::runtime_error("Could not record the prepared content.");
    }
    progress.Report(0.95, "Activating game files");
    if (fs::exists(destination)) {
      if (fs::is_symlink(fs::symlink_status(destination)) || !Owned(destination))
        throw std::runtime_error("The destination changed during setup. Its files were kept.");
      fs::rename(destination, transaction / "old");
      moved_old = true;
    }
    fs::rename(staged, destination);
    published = true;
    // Only the transaction created by this invocation is cleaned up.
    std::error_code ec;
    fs::remove_all(transaction, ec);
    if (ec) out << "Prepared content is ready; previous files remain in the .preparing folder.\n";
    progress.Report(1, "Ready to play");
    return {};
  } catch (const std::exception& error) {
    std::error_code ec;
    if (moved_old && !published) {
      fs::rename(transaction / "old", destination, ec);
      // Preserve the previous version if rollback itself fails.
      if (ec) return std::string(error.what()) + " Previous content is in " + Utf8(transaction);
    }
    if (transaction_created && !published) fs::remove_all(transaction, ec);
    return error.what();
  }
}

int PrepareContentCommand(int argc, char** argv, std::ostream& out, std::ostream& err) {
  bool requested = false;
  for (int i = 1; i < argc; ++i) requested |= std::string_view(argv[i]) == "--prepare-content";
  if (!requested) return -1;
  fs::path source, destination;
  bool invalid = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--prepare-content") continue;
    if ((arg == "--source" || arg == "--destination") && i + 1 < argc) {
      fs::path& value = arg == "--source" ? source : destination;
      if (!value.empty() || std::string_view(argv[i + 1]).starts_with("--")) { invalid = true; break; }
      value = FromUtf8(argv[++i]);
    } else { invalid = true; break; }
  }
  if (invalid || source.empty() || destination.empty()) {
    err << "Usage: --prepare-content --source <disc-or-folder> --destination <managed-folder>\n";
    return 2;
  }
  const std::string error = PrepareContent(source, destination, out);
  if (!error.empty()) { err << OneLine(error) << std::endl; return 1; }
  return 0;
}
}  // namespace eternalsonata
