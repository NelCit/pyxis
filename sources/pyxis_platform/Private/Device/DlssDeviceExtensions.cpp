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

#include <algorithm>
#include <cstdlib>
#include <iterator>
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

// DLSS Stage 2b diagnostic aid — Streamline's OWN internal log messages
// (sl_core_types.h's PFun_LogMessageCallback) were previously discarded
// entirely (pref.logMessageCallback left null, pref.showConsole = false —
// showConsole opens a SEPARATE Win32 console window that a headless/
// redirected-stdout run never captures anyway). Installing this callback
// surfaces Streamline's own reasoning (e.g. WHY slIsFeatureSupported
// rejected a feature) into Pyxis's normal log stream instead of a void.
// House rule (dlss-streamline-integration skill): "the Streamline logging
// callback is process-global; install once" — this IS that one place,
// since TryBootstrapStreamlineForVulkan runs exactly once before the
// device (and therefore before slInit) exists.
void StreamlineLogCallback(sl::LogType type, const char* msg) noexcept {
  if (msg == nullptr)
    return;
  auto& log = Logging::Get();
  const std::string line = std::string{"Streamline: "} + msg;
  switch (type)
  {
    case sl::LogType::eError: log.Error(log::PLATFORM, line); break;
    case sl::LogType::eWarn: log.Warn(log::PLATFORM, line); break;
    default: log.Info(log::PLATFORM, line); break;
  }
}

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
  //
  // DLSS Stage 2b -- kFeatureDLSS_RR (Ray Reconstruction, sl.dlss_d.dll).
  // RESOLVED + LIVE (2026-07-06): "DLSS-RR is usable ... dlss: RR active".
  //
  // History + the two-part fix (both required together):
  //  1. VERSION PAIRING. The earlier storm (100-400 `nvngx_update.exe`
  //     spawned per run, accumulating until killed) was NGX looping to
  //     OTA-fetch a DLSS-D model the staged snippet wanted but the machine
  //     lacked. This machine's NGX cache has DLSS-D model 160_E658700.bin
  //     (== 310.6.0 generation), but Streamline SDK 2.12.0's nvngx_dlssd.dll
  //     (v310.7.0) wants the 310.7 model (160_E658703.bin) which was never
  //     provisioned here and OTA never fetched. FIX: stage the Streamline
  //     2.11.1 runtime DLL set (nvngx_dlssd.dll v310.6.0 -- an EXACT match
  //     to the cached model), so RR resolves LOCALLY with no OTA. Result:
  //     the updater spawns drop to a small bounded burst that self-clears
  //     in <4s (normal per-launch model-currency check), NOT the runaway.
  //     _tools/setup_dlss.py stages 2.11.1; headers stay 2.12.0 (sl_dlss_d.h
  //     / sl_result.h are byte-identical across the two -- verified).
  //  2. projectId (below). 2.11.1 hard-enforces a valid NGX app identifier
  //     where 2.12.0 tolerated an empty one; without it 2.11.1 disables ALL
  //     NGX features (SR included). See pref.projectId.
  //
  // If ever staged against a driver/Streamline pairing where the cached
  // model and the snippet DIVERGE again, the storm can recur -- kill
  // nvngx_update.exe, re-pair versions (match nvngx_dlssd.dll's FileVersion
  // to the models\dlssd cache generation), or drop kFeatureDLSS_RR here.
  const sl::Feature featuresToLoad[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_RR};
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
  // projectId — a developer-chosen GUID string. REQUIRED: when engineVersion
  // is non-empty, sl.common's slInit takes the Project_Id NGX-identifier
  // branch (commonEntry.cpp) and passes projectId straight to
  // NVSDK_NGX_VULKAN_GetFeatureRequirements. Leaving it empty makes that a
  // zero-length string -> NGX 0xbad00005 (FAIL_InvalidParameter) -> "Please
  // provide correct application id ... NGX based features will be disabled".
  // Streamline 2.12.0 silently tolerated the empty id for DLSS-SR; 2.11.1
  // (pinned to match this machine's cached DLSS-RR model 310.6.0 — see the
  // featuresToLoad comment above) enforces it and disables ALL NGX features
  // without it. A self-generated GUID satisfies the dev/non-registered path
  // (no NVIDIA app-id registration needed for a private tool).
  pref.projectId = "a1b2c3d4-e5f6-4a5b-8c9d-0e1f2a3b4c5d";
  pref.pathsToPlugins = pluginPaths;
  pref.numPathsToPlugins = 1;
  pref.featuresToLoad = featuresToLoad;
  pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(featuresToLoad));
  // DLSS Stage 2b — route Streamline's own log messages into Pyxis's log
  // (see StreamlineLogCallback's doc comment above). sl_core_types.h's
  // LogType doc comment: eWarn/eError "Always shown regardless of
  // LogLevel" -- only eInfo is gated by logLevel -- so eDefault here still
  // surfaces every WARN/ERROR Streamline emits (this is how the
  // "DLSSD feature is not supported" / "ngxResult failed" root-cause
  // lines that diagnosed the RR-unsupported finding below were found)
  // without eVerbose's several-thousand-line-per-run eInfo firehose
  // (plugin/hook mapping, tag registration, ...) landing in every future
  // run's log by default. Bump to eVerbose locally when digging into a
  // NEW Streamline-side mystery.
  pref.logMessageCallback = &StreamlineLogCallback;
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

  // DLSS Stage 2b -- kFeatureDLSS_RR's OWN requirements, queried and
  // logged the SAME way as kFeatureDLSS above (purely informational here;
  // pyxis_renderer's DlssProvider::Initialize is what actually decides
  // usability once a device exists -- see this file's header comment on
  // why the fine-grained feature-bit merge isn't implemented). Extension
  // names are MERGED into `result` with de-duplication: vkCreateInstance/
  // vkCreateDevice reject a pEnabledExtensionNames list containing the
  // same name twice (VUID-VkInstanceCreateInfo-ppEnabledExtensionNames-01840
  // and the device-side equivalent), and kFeatureDLSS_RR commonly asks for
  // a subset/superset overlapping kFeatureDLSS's own list.
  sl::FeatureRequirements reqsRR{};
  const sl::Result reqsRRResult = slGetFeatureRequirementsFn(sl::kFeatureDLSS_RR, reqsRR);
  if (reqsRRResult != sl::Result::eOk)
  {
    log.Info(log::PLATFORM,
             "DlssDeviceExtensions: slGetFeatureRequirements(kFeatureDLSS_RR) failed "
             "(sl::Result=" + std::to_string(static_cast<int>(reqsRRResult))
                 + ") -- Ray Reconstruction requirements unknown; DlssProvider will fall back to "
                   "DLSS-SR + the builtin denoiser chain if kFeatureDLSS_RR later reports "
                   "unsupported");
  }
  else
  {
    auto mergeUnique = [](std::vector<std::string>& dst, const char* const* names, uint32_t count) {
      for (uint32_t i = 0; i < count; ++i)
      {
        const std::string name{names[i]};
        if (std::find(dst.begin(), dst.end(), name) == dst.end())
          dst.push_back(name);
      }
    };
    mergeUnique(result.instanceExtensions, reqsRR.vkInstanceExtensions, reqsRR.vkNumInstanceExtensions);
    mergeUnique(result.deviceExtensions, reqsRR.vkDeviceExtensions, reqsRR.vkNumDeviceExtensions);

    std::string summaryRR = "DlssDeviceExtensions: kFeatureDLSS_RR requirements -- "
                            + std::to_string(reqsRR.vkNumInstanceExtensions)
                            + " instance extension(s), "
                            + std::to_string(reqsRR.vkNumDeviceExtensions) + " device extension(s) "
                              "(merged above, de-duplicated); "
                            + std::to_string(reqsRR.numRequiredTags) + " required buffer tag(s):";
    for (uint32_t i = 0; i < reqsRR.numRequiredTags; ++i)
      summaryRR += " " + std::to_string(reqsRR.requiredTags[i]);
    log.Info(log::PLATFORM, summaryRR);
  }

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
