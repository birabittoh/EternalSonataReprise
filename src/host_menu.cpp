#include "host_menu.h"

#include <rex/platform.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_ANDROID
#include <jni.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <rex/string.h>
#endif

namespace eternalsonata {

namespace {
// Routes the JNI callbacks (see below) back to the instance that opened the
// dialog. There is only ever one HostMenu, owned by the app for its whole
// lifetime, so a raw pointer is fine.
HostMenu* g_instance = nullptr;
}  // namespace

#if REX_PLATFORM_ANDROID

namespace {

// Menu entries, in the order EternalSonataActivity shows them. The indices
// are what nativeHostMenuSelect switches on, so keep the two in step.
enum MenuItem : int {
  kInstallDlc = 0,
  kManageDlc,
  kExportSaves,
  kImportSaves,
  kExit,
};

const char* const kMenuItems[] = {
    "Install DLC...",     "Remove DLC...",
    "Export saves", "Import saves", "Exit",
};

// Which document request a JNI file callback belongs to. Passed to Java and
// handed straight back, since the picker result arrives on a later turn of
// the Android activity's own lifecycle.
enum DocumentOp : int {
  kOpInstallDlc = 0,
  kOpImportSaves,
  kOpExportSaves,
};

// The DLC listing the manage dialog last showed, so a selection index means
// something when it comes back. Only ever touched on the UI thread.
std::vector<rex::system::xam::XCONTENT_AGGREGATE_DATA> g_listed_dlc;

// Marketplace DLC is filed under the common (all zeroes) xuid, on the dummy
// HDD device id 1; the same pair dlc_auto_install.cpp enumerates with.
constexpr uint32_t kDlcDeviceId = 1;
constexpr uint64_t kCommonXuid = 0;

JNIEnv* JniEnv() { return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); }

// Calls a void method on the activity. `signature` and the varargs are the
// usual JNI ones.
void CallActivityVoid(const char* name, const char* signature, ...) {
  JNIEnv* env = JniEnv();
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetMethodID(cls, name, signature);
  if (mid) {
    va_list args;
    va_start(args, signature);
    env->CallVoidMethodV(activity, mid, args);
    va_end(args);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

jobjectArray ToJavaStringArray(JNIEnv* env, const std::vector<std::string>& items) {
  jclass string_cls = env->FindClass("java/lang/String");
  jobjectArray array = env->NewObjectArray(static_cast<jsize>(items.size()), string_cls, nullptr);
  for (size_t i = 0; i < items.size(); ++i) {
    jstring s = env->NewStringUTF(items[i].c_str());
    env->SetObjectArrayElement(array, static_cast<jsize>(i), s);
    env->DeleteLocalRef(s);
  }
  env->DeleteLocalRef(string_cls);
  return array;
}

void ShowNativeMenu() {
  JNIEnv* env = JniEnv();
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = env->GetMethodID(cls, "showHostMenu", "([Ljava/lang/String;)V");
  if (mid) {
    std::vector<std::string> items(std::begin(kMenuItems), std::end(kMenuItems));
    jobjectArray array = ToJavaStringArray(env, items);
    env->CallVoidMethod(activity, mid, array);
    env->DeleteLocalRef(array);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

void ShowToast(const std::string& message) {
  JNIEnv* env = JniEnv();
  if (!env) {
    return;
  }
  jstring text = env->NewStringUTF(message.c_str());
  CallActivityVoid("showHostToast", "(Ljava/lang/String;)V", text);
  env->DeleteLocalRef(text);
}

rex::system::xam::ContentManager* ContentManager() {
  auto* kernel_state = rex::system::kernel_state();
  return kernel_state ? kernel_state->content_manager() : nullptr;
}

// content_root/<xuid>/<title id>. Saves live under this per profile, so an
// export has to sweep every xuid directory rather than just the signed in
// one; the reference layout has both a zeroed and a real profile id.
std::string TitleIdDirName() {
  auto* kernel_state = rex::system::kernel_state();
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "%08X", kernel_state ? kernel_state->title_id() : 0u);
  return buffer;
}

// Copies every <xuid>/<title id> tree into `staging`, keeping the xuid level
// so an import can put each back where it came from.
bool StageSavesForExport(const std::filesystem::path& user_data_root,
                         const std::filesystem::path& staging, std::string& error) {
  const std::string title_id = TitleIdDirName();
  std::error_code ec;
  std::filesystem::create_directories(staging, ec);

  int copied = 0;
  for (const auto& entry : std::filesystem::directory_iterator(user_data_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory(ec) || ec) {
      continue;
    }
    // A profile directory is a 16 hex digit xuid; everything else under the
    // user folder (achievements, cache, ...) is not a save.
    const std::string name = entry.path().filename().string();
    if (name.size() != 16 ||
        name.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos) {
      continue;
    }
    const auto source = entry.path() / title_id;
    if (!std::filesystem::is_directory(source, ec)) {
      continue;
    }
    const auto dest = staging / name / title_id;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::filesystem::copy(source, dest,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
      error = "could not read saves for profile " + name + ": " + ec.message();
      return false;
    }
    ++copied;
  }

  if (!copied) {
    error = "no saves found for this game";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The menu actions. Each runs on the UI thread (see the JNI callbacks below)
// and returns the message to show the user.
// ---------------------------------------------------------------------------

std::string DoInstallDlc(const std::filesystem::path& package) {
  auto* content_manager = ContentManager();
  if (!content_manager) {
    return "Not ready yet, try again once the game has loaded.";
  }
  // Trust the package header over the picker: InstallContent reads the title
  // id and content type out of the container itself and files it accordingly,
  // so a package for another game lands somewhere this title never looks
  // rather than being silently adopted. Check it here so the user is told.
  const auto result = content_manager->InstallContent(package);
  if (XFAILED(result)) {
    return "Could not install that package (it may not be Xbox 360 DLC).";
  }
  return "DLC installed. Restart the game if it does not appear.";
}

std::string DoExportSaves(const std::filesystem::path& user_data_root,
                          const std::filesystem::path& cache_root,
                          std::filesystem::path& out_archive) {
  const auto staging = cache_root / "export_staging";
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);

  std::string error;
  if (!StageSavesForExport(user_data_root, staging, error)) {
    std::filesystem::remove_all(staging, ec);
    return "Export failed: " + error;
  }

  const std::time_t now = std::time(nullptr);
  std::tm local_time;
  localtime_r(&now, &local_time);
  char timestamp[16] = {};
  std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time);
  const auto archive = cache_root / (std::string("eternalsonata-saves-") + timestamp + ".zip");
  std::filesystem::remove(archive, ec);
  const bool ok = rex::filesystem::CreateZip(staging, archive, error);
  std::filesystem::remove_all(staging, ec);
  if (!ok) {
    return "Export failed: " + error;
  }
  out_archive = archive;
  return {};
}

std::string DoImportSaves(const std::filesystem::path& archive,
                          const std::filesystem::path& user_data_root) {
  std::string error;
  if (!rex::filesystem::ExtractZip(archive, user_data_root, error)) {
    return "Import failed: " + error;
  }
  return "Saves imported successfully.";
}

void ShowDlcManager() {
  auto* content_manager = ContentManager();
  if (!content_manager) {
    ShowToast("Not ready yet, try again once the game has loaded.");
    return;
  }
  g_listed_dlc = content_manager->ListContent(kDlcDeviceId, kCommonXuid,
                                              rex::system::XContentType::kMarketplaceContent);
  if (g_listed_dlc.empty()) {
    ShowToast("No DLC installed.");
    return;
  }

  std::vector<std::string> labels;
  labels.reserve(g_listed_dlc.size());
  for (const auto& item : g_listed_dlc) {
    std::string label = rex::string::to_utf8(item.display_name());
    if (label.empty()) {
      label = item.file_name();
    }
    labels.push_back(std::move(label));
  }

  JNIEnv* env = JniEnv();
  if (!env) {
    return;
  }
  jobjectArray array = ToJavaStringArray(env, labels);
  CallActivityVoid("showDlcManager", "([Ljava/lang/String;)V", array);
  env->DeleteLocalRef(array);
}

void RunOnUiThread(std::function<void()> work) {
  if (!g_instance) {
    return;
  }
  g_instance->window_for_callback()->app_context().CallInUIThread(std::move(work));
}

}  // namespace

// The JNI callbacks below all arrive on an Android thread that is not the one
// owning the SDL window, so anything touching engine state is marshalled
// through CallInUIThread; that is the same hazard and fix as settings.cpp's
// RestartNow(). The file work is marshalled too, since the content manager is
// shared with the guest's own DLC enumeration.

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_eternalsonata_EternalSonataActivity_nativeHostMenuSelect(JNIEnv*, jobject,
                                                                            jint index) {
  if (!g_instance) {
    return;
  }
  rex::ui::Window* window = g_instance->window_for_callback();
  const auto user_data_root = g_instance->user_data_root();
  const auto cache_root = g_instance->cache_root();

  switch (index) {
    case kInstallDlc:
      // Any type: STFS packages have no MIME type of their own and pickers
      // hide what they cannot name.
      CallActivityVoid("requestOpenDocument", "(I)V", static_cast<jint>(kOpInstallDlc));
      break;
    case kManageDlc:
      RunOnUiThread([] { ShowDlcManager(); });
      break;
    case kExportSaves:
      CallActivityVoid("requestCreateDocument", "(I)V", static_cast<jint>(kOpExportSaves));
      break;
    case kImportSaves:
      CallActivityVoid("requestOpenDocument", "(I)V", static_cast<jint>(kOpImportSaves));
      break;
    case kExit:
      RunOnUiThread([window] { window->RequestClose(); });
      break;
    default:  // The dialog was dismissed.
      break;
  }
}

// `path` is a plain cache file Java already copied the picked document into
// (import/install), or null when Java is asking for the file to write out
// (export). Returns the path to hand back to the picked destination, or null.
// Keeping SAF on the Java side like this is what stops content:// URIs from
// having to reach the save and content code at all.
extern "C" JNIEXPORT jstring JNICALL
Java_com_birabittoh_eternalsonata_EternalSonataActivity_nativeDocumentReady(JNIEnv* env, jobject,
                                                                           jint op, jstring path) {
  if (!g_instance) {
    return nullptr;
  }
  const auto user_data_root = g_instance->user_data_root();
  const auto cache_root = g_instance->cache_root();

  std::filesystem::path host_path;
  if (path) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    host_path = rex::to_path(std::string(chars ? chars : ""));
    env->ReleaseStringUTFChars(path, chars);
  }

  std::filesystem::path produced;
  std::string message;
  // Synchronous so the result can be reported here, and so Java does not copy
  // the export archive out before it has been written.
  g_instance->window_for_callback()->app_context().CallInUIThreadSynchronous(
      [&, op, user_data_root, cache_root] {
        switch (op) {
          case kOpInstallDlc:
            message = DoInstallDlc(host_path);
            break;
          case kOpImportSaves:
            message = DoImportSaves(host_path, user_data_root);
            break;
          case kOpExportSaves:
            message = DoExportSaves(user_data_root, cache_root, produced);
            break;
          default:
            break;
        }
      });

  if (!message.empty()) {
    ShowToast(message);
  }
  if (produced.empty()) {
    return nullptr;
  }
  return env->NewStringUTF(produced.string().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_birabittoh_eternalsonata_EternalSonataActivity_nativeDlcRemove(JNIEnv*, jobject,
                                                                       jint index) {
  RunOnUiThread([index] {
    auto* content_manager = ContentManager();
    if (!content_manager || index < 0 || static_cast<size_t>(index) >= g_listed_dlc.size()) {
      return;
    }
    const auto data = g_listed_dlc[static_cast<size_t>(index)];
    // Unmount first: the guest may already have this package mounted, and
    // deleting the directory out from under an open package leaves the
    // enumerator returning a phantom entry.
    const auto result = content_manager->UnmountAndDeleteContent(kCommonXuid, data);
    if (XFAILED(result)) {
      ShowToast("Could not remove that DLC.");
      return;
    }
    ShowToast("DLC removed. Restart the game to finish unloading it.");
  });
}

#endif  // REX_PLATFORM_ANDROID

HostMenu::HostMenu(rex::ui::Window* window, std::filesystem::path user_data_root,
                   std::filesystem::path cache_root)
    : window_(window),
      user_data_root_(std::move(user_data_root)),
      cache_root_(std::move(cache_root)) {
  g_instance = this;
#if REX_PLATFORM_ANDROID
  // "BrowserBack" is the Android back button (trapped by SDL_HINT_
  // ANDROID_TRAP_BACK_BUTTON in window_sdl.cpp so it arrives as a key event
  // instead of backgrounding the app) and doubles as a desktop keyboard's
  // dedicated back key. "Back" (the gamepad Back/View button) is a separate
  // bind, same two-bind pattern as OverlayMenuDialog's Y/Insert pair, since
  // one bind can only own one key or button.
  rex::ui::RegisterBind("bind_host_menu", "BrowserBack", "Open host menu",
                        [] { ShowNativeMenu(); });
  rex::ui::RegisterBind("bind_host_menu_gamepad", "Back", "Open host menu (gamepad)",
                        [] { ShowNativeMenu(); });
#endif
}

HostMenu::~HostMenu() {
#if REX_PLATFORM_ANDROID
  rex::ui::UnregisterBind("bind_host_menu_gamepad");
  rex::ui::UnregisterBind("bind_host_menu");
#endif
  g_instance = nullptr;
}

}  // namespace eternalsonata
