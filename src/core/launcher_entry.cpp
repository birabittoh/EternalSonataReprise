// Preparation runs before the SDK's desktop entry point can initialize SDL.
#include <iostream>
#include <string>
#include <vector>
#include "prepare_content.h"
#include <rex/cvar.h>
#include <rex/logging.h>

namespace {
int Dispatch(int argc, char** argv) {
  bool prepare = false;
  for (int i = 1; i < argc; ++i)
    prepare |= std::string_view(argv[i]) == "--prepare-content";
  if (!prepare) return -1;
  rex::InitLoggingEarly();
  // Diagnostics from the shared installer stay readable in the launcher log.
  rex::cvar::SetFlagByName("user_language", "1", /*persist=*/false);
  return eternalsonata::PrepareContentCommand(argc, argv, std::cout, std::cerr);
}
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
extern "C" int WINAPI EternalSonataWindowedWinMain(HINSTANCE, HINSTANCE, LPWSTR, int);
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command, int show) {
  int argc = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!wide) return 2;
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (!length) { LocalFree(wide); return 2; }
    std::string value(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), length, nullptr, nullptr);
    value.pop_back();
    args.push_back(std::move(value));
  }
  LocalFree(wide);
  std::vector<char*> argv;
  for (auto& value : args) argv.push_back(value.data());
  const int result = Dispatch(argc, argv.data());
  return result < 0 ? EternalSonataWindowedWinMain(instance, previous, command, show) : result;
}
#else
int EternalSonataWindowedMain(int argc, char** argv);
int main(int argc, char** argv) {
  const int result = Dispatch(argc, argv);
  return result < 0 ? EternalSonataWindowedMain(argc, argv) : result;
}
#endif
