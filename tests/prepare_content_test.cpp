// Test the real transaction filesystem and CLI with a fake installer boundary.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "prepare_content.h"
#include "install_pipeline.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
namespace fs = std::filesystem;
namespace {
bool fail_install = false, unknown_source = false, dirty_output = false;
fs::path changed_destination;
int installs = 0;
fs::path Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string Text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void Write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}
int Command(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  std::ostringstream out, err;
  const int result = eternalsonata::PrepareContentCommand(int(argv.size()), argv.data(), out, err);
  if (result == 1) std::cerr << err.str();
  return result;
}
}
namespace eternalsonata {
bool SameFileName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}
SourceInfo IdentifySource(const std::string&) {
  SourceInfo info;
  if (!unknown_source) info.id = "360-pal";
  return info;
}
bool IsGameDirectory(const fs::path& p) { return fs::is_regular_file(p / "index.vmtoc"); }
bool NeedsConversion(const fs::path&) { return dirty_output; }
bool NeedsPatching(const fs::path&) { return false; }
bool IsOutdated(const fs::path&) { return false; }
fs::path ExtractedSource(const std::string& s) { return fs::is_directory(Path(s)) ? Path(s) : fs::path(); }
const char* PhaseName(Phase) { return "Preparing"; }
std::string Install(const std::string& picked, const fs::path& dest, const InstallHooks& hooks,
                    fs::path& ready) {
  ++installs;
  assert(hooks.progress);
  hooks.phase(Phase::kExtract, PhaseState::kActive);
  hooks.progress("Extracting\nfiles", 0.5f, {});
  if (fs::is_directory(Path(picked))) {
    assert(fs::equivalent(Path(picked), dest));
    Write(dest / "original.txt", "patched");
  }
  Write(dest / "index.vmtoc", "ready");
  if (!changed_destination.empty())
    fs::remove(changed_destination / ".recomplauncher-content");
  ready = dest;
  return fail_install ? "injected installer failure" : "";
}
}
int main() {
  const auto base = fs::temp_directory_path() /
      ("reprise-prepare-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(fs::create_directory(base));
  const auto source = base / "my dump", dest = base / "install" / "managed";
  Write(source / "original.txt", "original");
  std::ostringstream out;
  assert(eternalsonata::PrepareContent(source, dest, out).empty());
  assert(Text(source / "original.txt") == "original");
  assert(Text(dest / "original.txt") == "patched");
  assert(Text(dest / "index.vmtoc") == "ready");
  assert(out.str().find("progress 1.000 Ready to play") != std::string::npos);
  assert(out.str().find("Extracting\nfiles") == std::string::npos);
  {
    std::istringstream lines(out.str());
    std::string line;
    double last = 0;
    while (std::getline(lines, line)) {
      if (!line.starts_with("progress ")) continue;
      const double value = std::stod(line.substr(9));
      assert(value >= last && value >= 0 && value <= 1);
      last = value;
    }
    assert(last == 1);
  }
  assert(!fs::exists(dest.string() + ".preparing"));

  Write(dest / "old.txt", "old");
  assert(eternalsonata::PrepareContent(source, dest, out).empty());
  assert(!fs::exists(dest / "old.txt"));

  Write(dest / "keep.txt", "previous version");
  fail_install = true;
  assert(!eternalsonata::PrepareContent(source, dest, out).empty());
  assert(Text(dest / "keep.txt") == "previous version");
  assert(Text(source / "original.txt") == "original");
  assert(!fs::exists(dest.string() + ".preparing"));
  fail_install = false;
  dirty_output = true;
  assert(!eternalsonata::PrepareContent(source, dest, out).empty());
  assert(Text(dest / "keep.txt") == "previous version");
  dirty_output = false;

  unknown_source = true;
  assert(!eternalsonata::PrepareContent(source, dest, out).empty());
  unknown_source = false;
  // An external change to ownership during a long install is respected.
  changed_destination = dest;
  assert(!eternalsonata::PrepareContent(source, dest, out).empty());
  assert(Text(dest / "keep.txt") == "previous version");
  Write(dest / ".recomplauncher-content", "EternalSonataReprise content v1\n");
  changed_destination.clear();
  const auto personal = base / "personal";
  Write(personal / "keep.txt", "mine");
  assert(!eternalsonata::PrepareContent(source, personal, out).empty());
  assert(Text(personal / "keep.txt") == "mine");
  std::error_code link_error;
  fs::create_directory_symlink(personal, source / "linked", link_error);
  if (!link_error) {
    assert(!eternalsonata::PrepareContent(source, dest, out).empty());
    assert(Text(personal / "keep.txt") == "mine");
    fs::remove(source / "linked");
  }

  const auto transaction = fs::path(dest.string() + ".preparing");
  Write(transaction, "do not delete");
  assert(!eternalsonata::PrepareContent(source, dest, out).empty());
  assert(Text(transaction) == "do not delete");
  fs::remove(transaction);
  Write(transaction / "old" / "recovery", "keep");
  assert(!eternalsonata::PrepareContent(source, dest, out).empty());
  assert(Text(transaction / "old" / "recovery") == "keep");
  fs::remove_all(transaction);
  assert(!eternalsonata::PrepareContent(source, source, out).empty());
  assert(!eternalsonata::PrepareContent(source, source / "child", out).empty());
  if (fs::exists(base / "MY DUMP"))
    assert(!eternalsonata::PrepareContent(source, base / "MY DUMP" / "child", out).empty());
  assert(!eternalsonata::PrepareContent(source, base, out).empty());
  assert(!eternalsonata::PrepareContent(base / "missing", dest, out).empty());

  const auto image = base / "disc.iso";
  Write(image, "fake disc");
  assert(eternalsonata::PrepareContent(image, dest, out).empty());
  assert(Text(image) == "fake disc");
  assert(Command({"game", "--fullscreen=true"}) == -1);
  assert(Command({"game", "--prepare-content"}) == 2);
  assert(Command({"game", "--prepare-content", "--source", "--destination"}) == 2);
  assert(Command({"game", "--prepare-content", "--source", "a", "--source", "b"}) == 2);
  assert(Command({"game", "--prepare-content", "--unexpected"}) == 2);

  const fs::path unicode = base / fs::path(u8"δοκιμή 日本語");
  Write(unicode / "original.txt", "original");
  const auto u = unicode.u8string(), d = dest.u8string();
  assert(Command({"game", "--prepare-content", "--source", std::string(u.begin(), u.end()),
                  "--destination", std::string(d.begin(), d.end())}) == 0);
  assert(Text(unicode / "original.txt") == "original");
  // Only remove the unique directory created by this invocation.
  fs::remove_all(base);
  std::cout << "Content preparation transaction checks passed (" << installs << " installer calls).\n";
}
