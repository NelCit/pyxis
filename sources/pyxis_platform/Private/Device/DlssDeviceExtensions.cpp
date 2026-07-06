// Pyxis platform — Streamline pre-device-creation bootstrap implementation.
//
// See DlssDeviceExtensions.h for the full rationale. GetProcAddress-based
// (not statically linked against sl.interposer.lib) so pyxis_platform
// builds and runs identically whether or not the Streamline SDK is staged
// -- ProgrammingGuideManualHooking.md 1.1 Vulkan: "linking the
// sl.interposer.lib would result in additional CPU overhead so the best
// approach is to dynamically load sl.interposer.dll instead", which is
// also exactly the shape DlssProvider's Stage 1 discovery already uses.

#include "Device/DlssDeviceExtensions.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/FileSystem/Path.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <sl.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace pyxis {

namespace {

constexpr const char* INTERPOSER_DLL_NAME = "sl.interposer.dll";
constexpr const char* DLSS_PATH_ENV_VAR = "PYXIS_DLSS_PATH";

#if defined(_WIN32)
std::wstring Utf8ToWide(std::string_view utf8) noexcept {
  if (utf8.empty())
    return {};
  const int required =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (required <= 0)
    return {};
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        required);
  return wide;
}

// Mirrors DlssProvider::Probe()'s Stage 1 discovery (pyxis_renderer,
// Private/Dlss/DlssProvider.cpp) -- see DlssDeviceExtensions.h's file
// comment for why this is duplicated rather than shared. Returns the
// directory (not the .dll path itself -- Streamline's own
// Preferences::pathsToPlugins wants a directory) and the module handle,
// or {"", nullptr} on any miss.
struct DiscoveredInterposer {
  std::wstring directoryWide;
  HMODULE module = nullptr;
};

DiscoveredInterposer DiscoverAndLoadInterposer() noexcept {
  auto& log = Logging::Get();
  Path directory;
  std::string searchedFrom;
  const char* const envOverride = std::getenv(DLSS_PATH_ENV_VAR);
  if (envOverride != nullptr && *envOverride != '\0')
  {
    directory = Path(envOverride);
    searchedFrom = std::string{"PYXIS_DLSS_PATH="} + envOverride;
  }
  else
  {
    const AssetLocator locator;
    directory = locator.ExecutableDirectory();
    searchedFrom = "exe directory";
  }

  const Path candidate = directory.Join(INTERPOSER_DLL_NAME);
  if (!candidate.Exists())
  {
    log.Info(log::PLATFORM, std::string{"DlssDeviceExtensions: "} + INTERPOSER_DLL_NAME
                                + " not found (checked " + searchedFrom + "); Streamline "
                                + "pre-device bootstrap skipped, device creation unaffected");
    return {};
  }

  const std::wstring widePath = Utf8ToWide(candidate.View());
  const HMODULE module = ::LoadLibraryW(widePath.c_str());
  if (module == nullptr)
  {
    const DWORD lastError = ::GetLastError();
    log.Info(log::PLATFORM, std::string{"DlssDeviceExtensions: LoadLibraryW("}
                                + std::string{candidate.View()}
                                + ") failed (GetLastError=" + std::to_string(lastError) + ")");
    return {};
  }
  return DiscoveredInterposer{Utf8ToWide(directory.View()), module};
}
#endif  // _WIN32

}  // namespace

DlssDeviceRequirements TryBootstrapStreamlineForVulkan() noexcept {
#if !defined(_WIN32)
  return {};
#else
  auto& log = Logging::Get();

  const DiscoveredInterposer interposer = DiscoverAndLoadInterposer();
  if (interposer.module == nullptr)
    return {};

  // NOTE: PFun_slInit / PFun_slGetFeatureRequirements / PFun_slShutdown
  // (sl_core_api.h) are declared at GLOBAL scope (they sit alongside the
  // `SL_API sl::Result slInit(...)` extern "C" declarations, not inside
  // `namespace sl`), even though their signatures reference sl:: types.
  using PFunSlInit = PFun_slInit*;
  using PFunSlGetFeatureRequirements = PFun_slGetFeatureRequirements*;
  const auto slInitFn =
      reinterpret_cast<PFunSlInit>(::GetProcAddress(interposer.module, "slInit"));
  const auto slGetFeatureRequirementsFn = reinterpret_cast<PFunSlGetFeatureRequirements>(
      ::GetProcAddress(interposer.module, "slGetFeatureRequirements"));
  if (slInitFn == nullptr || slGetFeatureRequirementsFn == nullptr)
  {
    log.Info(log::PLATFORM,
             "DlssDeviceExtensions: sl.interposer.dll loaded but missing slInit / "
             "slGetFeatureRequirements symbols -- Streamline pre-device bootstrap skipped");
    return {};
  }

  // ProgrammingGuideManualHooking.md 3.0 -- manual hooking flag; we never
  // hook the DXGI/D3D/Vulkan present chain (Pyxis's own VkDeviceManager
  // owns the swapchain outright), so eUseManualHooking is mandatory.
  // eAllowOTA / eLoadDownloadedPlugins are explicitly OFF (default-on in
  // sl::Preferences) -- Pyxis stages its own pinned DLL set
  // (_tools/setup_dlss.py) and should never silently pull a
  // network-downloaded plugin.
  const sl::Feature featuresToLoad[] = {sl::kFeatureDLSS};
  const wchar_t* pluginPaths[] = {interposer.directoryWide.c_str()};

  sl::Preferences pref{};
  // eUseFrameBasedResourceTagging -- sl_core_api.h's own deprecation note
  // on the OLD slSetTag ("Use ... slSetTagForFrame and set
  // sl::PreferenceFlags::eUseFrameBasedResourceTagging") documents this as
  // required for the frame-token-based tagging call DlssProvider::Evaluate
  // uses (DlssProviderFrame.cpp); omitting it produced
  // sl::Result::eErrorInvalidIntegration from slSetTagForFrame in testing
  // on this machine.
  pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking
             | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.renderAPI = sl::RenderAPI::eVulkan;  // required for correct slGetFeatureRequirements (guide 5.2.1 note)
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "1";
  pref.pathsToPlugins = pluginPaths;
  pref.numPathsToPlugins = 1;
  pref.featuresToLoad = featuresToLoad;
  pref.numFeaturesToLoad = 1;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.showConsole = false;

  const sl::Result initResult = slInitFn(pref, sl::kSDKVersion);
  if (initResult != sl::Result::eOk)
  {
    log.Info(log::PLATFORM, "DlssDeviceExtensions: slInit failed (sl::Result="
                                + std::to_string(static_cast<int>(initResult))
                                + ") -- Streamline stays inactive, device creation unaffected");
    return {};
  }

  sl::FeatureRequirements reqs{};
  const sl::Result reqsResult = slGetFeatureRequirementsFn(sl::kFeatureDLSS, reqs);
  if (reqsResult != sl::Result::eOk)
  {
    log.Info(log::PLATFORM,
             "DlssDeviceExtensions: slInit succeeded but slGetFeatureRequirements(kFeatureDLSS) "
             "failed (sl::Result=" + std::to_string(static_cast<int>(reqsResult))
                 + ") -- Streamline initialised but DLSS plugin requirements unknown; no extra "
                   "Vulkan extensions requested");
    // slInit already succeeded -- report streamlineActive so the caller
    // still calls slShutdown at teardown, just with no extra extensions.
    return DlssDeviceRequirements{true, {}, {}};
  }

  DlssDeviceRequirements result;
  result.streamlineActive = true;
  result.instanceExtensions.reserve(reqs.vkNumInstanceExtensions);
  for (uint32_t i = 0; i < reqs.vkNumInstanceExtensions; ++i)
    result.instanceExtensions.emplace_back(reqs.vkInstanceExtensions[i]);
  result.deviceExtensions.reserve(reqs.vkNumDeviceExtensions);
  for (uint32_t i = 0; i < reqs.vkNumDeviceExtensions; ++i)
    result.deviceExtensions.emplace_back(reqs.vkDeviceExtensions[i]);

  std::string summary = "DlssDeviceExtensions: Streamline initialised (manual hooking, "
                        "kFeatureDLSS requested); requires "
                        + std::to_string(result.instanceExtensions.size())
                        + " instance extension(s), " + std::to_string(result.deviceExtensions.size())
                        + " device extension(s)";
  for (const std::string& ext : result.instanceExtensions)
    summary += "\n  [instance] " + ext;
  for (const std::string& ext : result.deviceExtensions)
    summary += "\n  [device] " + ext;
  summary += "; " + std::to_string(reqs.numRequiredTags) + " required buffer tag(s):";
  for (uint32_t i = 0; i < reqs.numRequiredTags; ++i)
    summary += " " + std::to_string(reqs.requiredTags[i]);
  summary += "; maxNumViewports=" + std::to_string(reqs.maxNumViewports)
           + " maxNumCPUThreads=" + std::to_string(reqs.maxNumCPUThreads);
  log.Info(log::PLATFORM, summary);

  return result;
#endif  // _WIN32
}

void ShutdownStreamlineIfActive(bool active) noexcept {
#if defined(_WIN32)
  if (!active)
    return;

  const DiscoveredInterposer interposer = DiscoverAndLoadInterposer();
  if (interposer.module == nullptr)
    return;  // Already logged by DiscoverAndLoadInterposer; nothing to shut down.

  using PFunSlShutdown = PFun_slShutdown*;
  const auto slShutdownFn =
      reinterpret_cast<PFunSlShutdown>(::GetProcAddress(interposer.module, "slShutdown"));
  if (slShutdownFn == nullptr)
    return;

  const sl::Result result = slShutdownFn();
  Logging::Get().Info(log::PLATFORM,
                      "DlssDeviceExtensions: slShutdown "
                          + std::string{result == sl::Result::eOk ? "ok" : "reported an error"});
#else
  (void)active;
#endif
}

}  // namespace pyxis
