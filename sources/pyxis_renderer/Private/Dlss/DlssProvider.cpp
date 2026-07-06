// Pyxis renderer — DlssProvider implementation: lifecycle (ctor/dtor,
// Stage 1 Probe(), Stage 2a Initialize()). Per-frame Evaluate() /
// GetOptimalRenderResolution() / ReleaseResources() live in
// DlssProviderFrame.cpp — see DlssProviderImpl.h for why the class is
// split across two .cpp files.
//
// See DlssProvider.h for the full Stage 1 / Stage 2a split rationale.

#include "Dlss/DlssProvider.h"

#include "Dlss/DlssProviderImpl.h"

#include <Pyxis/Platform/Device/VulkanContext.h>
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

#include <cstdlib>
#include <string>
#include <string_view>

namespace pyxis {

namespace {

constexpr const char* INTERPOSER_DLL_NAME = "sl.interposer.dll";
constexpr const char* DLSS_PATH_ENV_VAR = "PYXIS_DLSS_PATH";

#if defined(_WIN32)
// Stage 2a — real Streamline core-API function TYPES (sl_core_api.h),
// replacing Stage 1's local placeholder typedefs (PFNslInit etc, which
// existed only to prove GetProcAddress found something before the real
// SDK headers were vendored). PFun_slInit / PFun_slSetFeatureLoaded /
// PFun_slIsFeatureSupported are declared at GLOBAL scope in sl_core_api.h
// (see DlssProviderImpl.h's sibling file DlssDeviceExtensions.cpp in
// pyxis_platform for the same observation) even though their signatures
// reference sl:: types.
using PFunSlInit = PFun_slInit*;
using PFunSlSetFeatureLoaded = PFun_slSetFeatureLoaded*;
using PFunSlIsFeatureSupported = PFun_slIsFeatureSupported*;

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
#endif  // _WIN32

}  // namespace

DlssProvider::DlssProvider() = default;

DlssProvider::~DlssProvider() {
#if defined(_WIN32)
  if (_interposerModule != nullptr)
    ::FreeLibrary(static_cast<HMODULE>(_interposerModule));
#endif
}

const DlssProvider::Availability& DlssProvider::Probe() noexcept {
  if (_probed)
    return _availability;
  _probed = true;

  auto& log = Logging::Get();

#if !defined(_WIN32)
  _availability.usable = false;
  _availability.failedAt = ProbeStage::DllDiscovery;
  _availability.reason = "DLSS Stage 1 is Windows-only (v1 platform scope)";
  log.Info(log::RENDER, std::string{"DlssProvider: probe — "} + _availability.reason);
  return _availability;
#else
  // Step (a) — DLL discovery. PYXIS_DLSS_PATH names a DIRECTORY containing
  // sl.interposer.dll (mirrors _tools/setup_dlss.py's staging
  // instructions); falls back to <exe-dir> so a packager can just drop the
  // Streamline + NGX DLLs next to pyxis.exe with zero environment setup.
  std::string searchedFrom;
  Path candidate;
  const char* const envOverride = std::getenv(DLSS_PATH_ENV_VAR);
  if (envOverride != nullptr && *envOverride != '\0')
  {
    candidate = Path(envOverride).Join(INTERPOSER_DLL_NAME);
    searchedFrom = std::string{"PYXIS_DLSS_PATH="} + envOverride;
  }
  else
  {
    const AssetLocator locator;
    candidate = locator.ExecutableDirectory().Join(INTERPOSER_DLL_NAME);
    searchedFrom = "exe directory";
  }

  if (!candidate.Exists())
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::DllDiscovery;
    _availability.reason = std::string{INTERPOSER_DLL_NAME} + " not found (checked " + searchedFrom
                           + ": " + std::string{candidate.View()} + ")";
    log.Info(log::RENDER, "DlssProvider: probe — " + _availability.reason);
    return _availability;
  }

  const std::wstring widePath = Utf8ToWide(candidate.View());
  const HMODULE module = ::LoadLibraryW(widePath.c_str());
  if (module == nullptr)
  {
    const DWORD lastError = ::GetLastError();
    _availability.usable = false;
    _availability.failedAt = ProbeStage::DllDiscovery;
    _availability.reason = std::string{"LoadLibraryW("} + std::string{candidate.View()}
                           + ") failed (GetLastError=" + std::to_string(lastError) + ")";
    log.Info(log::RENDER, "DlssProvider: probe — " + _availability.reason);
    return _availability;
  }
  _interposerModule = module;

  // Step (b) — resolve the handful of Streamline C entry points this
  // Stage-1 probe checks for. Never called here — Initialize() (Stage 2a,
  // below) does the actual device-interop calls once a VulkanContext is
  // available.
  const PFunSlInit slInit = reinterpret_cast<PFunSlInit>(::GetProcAddress(module, "slInit"));
  if (slInit == nullptr)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::SymbolResolution;
    _availability.reason = std::string{INTERPOSER_DLL_NAME} + " loaded but missing symbol 'slInit'";
    log.Info(log::RENDER, "DlssProvider: probe — " + _availability.reason);
    return _availability;
  }
  const PFunSlSetFeatureLoaded slSetFeatureLoaded =
      reinterpret_cast<PFunSlSetFeatureLoaded>(::GetProcAddress(module, "slSetFeatureLoaded"));
  if (slSetFeatureLoaded == nullptr)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::SymbolResolution;
    _availability.reason =
        std::string{INTERPOSER_DLL_NAME} + " loaded but missing symbol 'slSetFeatureLoaded'";
    log.Info(log::RENDER, "DlssProvider: probe — " + _availability.reason);
    return _availability;
  }
  const PFunSlIsFeatureSupported slIsFeatureSupported =
      reinterpret_cast<PFunSlIsFeatureSupported>(::GetProcAddress(module, "slIsFeatureSupported"));
  if (slIsFeatureSupported == nullptr)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::SymbolResolution;
    _availability.reason =
        std::string{INTERPOSER_DLL_NAME} + " loaded but missing symbol 'slIsFeatureSupported'";
    log.Info(log::RENDER, "DlssProvider: probe — " + _availability.reason);
    return _availability;
  }

  // Step (c) — discovery + symbol resolution both succeeded. Initialize()
  // (Stage 2a) picks up from here once PyxisRenderer's ctor has a
  // VulkanContext to hand it; until then `usable` stays false.
  _availability.usable = false;
  _availability.failedAt = ProbeStage::DeviceInteropPending;
  _availability.reason = "device interop pending (Initialize() not yet called)";
  log.Info(log::RENDER, std::string{"DlssProvider: probe — sl.interposer.dll loaded + symbols "
                                    "resolved; "}
                            + _availability.reason);
  return _availability;
#endif  // _WIN32
}

void DlssProvider::Initialize(const VulkanContext& vk) noexcept {
#if !defined(_WIN32)
  (void)vk;
  return;
#else
  if (_initialized)
    return;
  _initialized = true;

  if (_availability.failedAt != ProbeStage::DeviceInteropPending)
    return;  // Stage 1 discovery/symbol resolution never succeeded — nothing to do.

  auto& log = Logging::Get();
  const HMODULE module = static_cast<HMODULE>(_interposerModule);

  _impl = std::make_unique<Impl>();

  // Resolve every core-API + DLSS entry point Stage 2a's per-frame path
  // needs, up front, so Evaluate()/GetOptimalRenderResolution() never have
  // to null-check a lazily-resolved pointer mid-frame.
  _impl->slSetVulkanInfo =
      reinterpret_cast<PFun_slSetVulkanInfo*>(::GetProcAddress(module, "slSetVulkanInfo"));
  _impl->slIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(
      ::GetProcAddress(module, "slIsFeatureSupported"));
  _impl->slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(
      ::GetProcAddress(module, "slGetFeatureFunction"));
  _impl->slSetConstants =
      reinterpret_cast<PFun_slSetConstants*>(::GetProcAddress(module, "slSetConstants"));
  _impl->slSetTagForFrame =
      reinterpret_cast<PFun_slSetTagForFrame*>(::GetProcAddress(module, "slSetTagForFrame"));
  _impl->slEvaluateFeature =
      reinterpret_cast<PFun_slEvaluateFeature*>(::GetProcAddress(module, "slEvaluateFeature"));
  _impl->slGetNewFrameToken =
      reinterpret_cast<PFun_slGetNewFrameToken*>(::GetProcAddress(module, "slGetNewFrameToken"));
  _impl->slAllocateResources =
      reinterpret_cast<PFun_slAllocateResources*>(::GetProcAddress(module, "slAllocateResources"));
  _impl->slFreeResources =
      reinterpret_cast<PFun_slFreeResources*>(::GetProcAddress(module, "slFreeResources"));

  if (_impl->slSetVulkanInfo == nullptr || _impl->slIsFeatureSupported == nullptr
      || _impl->slGetFeatureFunction == nullptr || _impl->slSetConstants == nullptr
      || _impl->slSetTagForFrame == nullptr || _impl->slEvaluateFeature == nullptr
      || _impl->slGetNewFrameToken == nullptr || _impl->slAllocateResources == nullptr
      || _impl->slFreeResources == nullptr)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::DeviceInteropFailed;
    _availability.reason = "Stage 2a symbol resolution failed (missing one of slSetVulkanInfo/"
                           "slIsFeatureSupported/slGetFeatureFunction/slSetConstants/"
                           "slSetTagForFrame/slEvaluateFeature/slGetNewFrameToken/"
                           "slAllocateResources/slFreeResources)";
    log.Info(log::RENDER, "DlssProvider: Initialize — " + _availability.reason);
    _impl.reset();
    return;
  }

  // ProgrammingGuideManualHooking.md 5.2.2 — "Once that is done [instance/
  // device created] you need to inform SL about the devices, instance and
  // queues". Pyxis's VkDeviceManager(Headless) creates exactly ONE queue
  // (graphics), so computeQueue* aliases the same family/index — DLSS-SR
  // needs no dedicated queue (documented scope cut: DlssDeviceExtensions.h
  // in pyxis_platform doesn't grow VkDeviceQueueCreateInfo::queueCount to
  // satisfy a nonzero vkNumGraphicsQueuesRequired/vkNumComputeQueuesRequired
  // — fine for SR, would need revisiting for DLSS-G's dedicated optical-
  // flow queue).
  sl::VulkanInfo vulkanInfo{};
  vulkanInfo.device = static_cast<VkDevice>(vk.device);
  vulkanInfo.instance = static_cast<VkInstance>(vk.instance);
  vulkanInfo.physicalDevice = static_cast<VkPhysicalDevice>(vk.physicalDevice);
  vulkanInfo.graphicsQueueFamily = vk.graphicsFamily;
  vulkanInfo.graphicsQueueIndex = 0;
  vulkanInfo.computeQueueFamily = vk.graphicsFamily;
  vulkanInfo.computeQueueIndex = 0;

  const sl::Result vkInfoResult = _impl->slSetVulkanInfo(vulkanInfo);
  if (vkInfoResult != sl::Result::eOk)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::DeviceInteropFailed;
    _availability.reason = "slSetVulkanInfo failed (sl::Result="
                           + std::to_string(static_cast<int>(vkInfoResult))
                           + ") -- pyxis_platform's pre-device Streamline bootstrap "
                             "(DlssDeviceExtensions.h) likely never ran slInit; see its log lines";
    log.Info(log::RENDER, "DlssProvider: Initialize — " + _availability.reason);
    _impl.reset();
    return;
  }

  sl::AdapterInfo adapterInfo{};
  adapterInfo.vkPhysicalDevice = vk.physicalDevice;
  const sl::Result supportResult = _impl->slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
  if (supportResult != sl::Result::eOk)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::DeviceInteropFailed;
    _availability.reason = "slIsFeatureSupported(kFeatureDLSS) failed (sl::Result="
                           + std::to_string(static_cast<int>(supportResult))
                           + ") -- non-RTX adapter, driver too old, or nvngx_dlss.dll not staged "
                             "alongside sl.dlss.dll";
    log.Info(log::RENDER, "DlssProvider: Initialize — " + _availability.reason);
    _impl.reset();
    return;
  }

  // DLSS-specific entry points are NOT direct sl.interposer.dll exports —
  // they live in the sl.dlss.dll plugin and are fetched via
  // slGetFeatureFunction (the same mechanism sl_dlss.h's own inline
  // SL_FEATURE_FUN_IMPORT_STATIC helpers use internally; Pyxis calls
  // slGetFeatureFunction directly instead of those inline helpers because
  // the helpers assume a statically-linked slGetFeatureFunction symbol,
  // which this GetProcAddress-only integration deliberately never links).
  void* dlssGetOptimalSettingsFn = nullptr;
  void* dlssSetOptionsFn = nullptr;
  const sl::Result optSettingsFnResult = _impl->slGetFeatureFunction(
      sl::kFeatureDLSS, "slDLSSGetOptimalSettings", dlssGetOptimalSettingsFn);
  const sl::Result setOptionsFnResult =
      _impl->slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", dlssSetOptionsFn);
  if (optSettingsFnResult != sl::Result::eOk || setOptionsFnResult != sl::Result::eOk
      || dlssGetOptimalSettingsFn == nullptr || dlssSetOptionsFn == nullptr)
  {
    _availability.usable = false;
    _availability.failedAt = ProbeStage::DeviceInteropFailed;
    _availability.reason = "slGetFeatureFunction couldn't resolve slDLSSGetOptimalSettings / "
                           "slDLSSSetOptions from the sl.dlss.dll plugin";
    log.Info(log::RENDER, "DlssProvider: Initialize — " + _availability.reason);
    _impl.reset();
    return;
  }
  _impl->slDLSSGetOptimalSettings =
      reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(dlssGetOptimalSettingsFn);
  _impl->slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(dlssSetOptionsFn);

  _availability.usable = true;
  _availability.failedAt = ProbeStage::Usable;
  _availability.reason.clear();
  log.Info(log::RENDER, "DlssProvider: Initialize — slSetVulkanInfo + "
                        "slIsFeatureSupported(kFeatureDLSS) both OK; DLSS-SR is usable on this "
                        "adapter/driver");

  // DLSS Stage 2b — Ray Reconstruction. ProgrammingGuideDLSS_RR.md 3.0:
  // "DLSS-RR is an extension to DLSS" — only probed now that SR itself is
  // confirmed usable (adapterInfo/vulkanInfo already set up above). A
  // failure here is NOT fatal to SR: it just leaves _rrAvailability.usable
  // false, and DlssPass/PyxisRenderer fall back to the SR+builtin-denoiser
  // rung (the ladder's middle step) — SR itself is untouched either way.
  const sl::Result rrSupportResult = _impl->slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
  if (rrSupportResult != sl::Result::eOk)
  {
    _rrAvailability.usable = false;
    _rrAvailability.failedAt = ProbeStage::DeviceInteropFailed;
    _rrAvailability.reason = "slIsFeatureSupported(kFeatureDLSS_RR) failed (sl::Result="
                             + std::to_string(static_cast<int>(rrSupportResult))
                             + ") -- nvngx_dlssd.dll likely not staged alongside sl.dlss_d.dll, "
                               "or the adapter/driver doesn't support Ray Reconstruction; "
                               "falling back to DLSS-SR + the builtin denoiser chain";
    log.Info(log::RENDER, "DlssProvider: Initialize — " + _rrAvailability.reason);
  }
  else
  {
    void* dlssdGetOptimalSettingsFn = nullptr;
    void* dlssdSetOptionsFn = nullptr;
    const sl::Result rrOptSettingsFnResult = _impl->slGetFeatureFunction(
        sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", dlssdGetOptimalSettingsFn);
    const sl::Result rrSetOptionsFnResult = _impl->slGetFeatureFunction(
        sl::kFeatureDLSS_RR, "slDLSSDSetOptions", dlssdSetOptionsFn);
    if (rrOptSettingsFnResult != sl::Result::eOk || rrSetOptionsFnResult != sl::Result::eOk
        || dlssdGetOptimalSettingsFn == nullptr || dlssdSetOptionsFn == nullptr)
    {
      _rrAvailability.usable = false;
      _rrAvailability.failedAt = ProbeStage::DeviceInteropFailed;
      _rrAvailability.reason =
          "slGetFeatureFunction couldn't resolve slDLSSDGetOptimalSettings / slDLSSDSetOptions "
          "from the sl.dlss_d.dll plugin; falling back to DLSS-SR + the builtin denoiser chain";
      log.Info(log::RENDER, "DlssProvider: Initialize — " + _rrAvailability.reason);
    }
    else
    {
      _impl->slDLSSDGetOptimalSettings =
          reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(dlssdGetOptimalSettingsFn);
      _impl->slDLSSDSetOptions = reinterpret_cast<PFun_slDLSSDSetOptions*>(dlssdSetOptionsFn);
      _rrAvailability.usable = true;
      _rrAvailability.failedAt = ProbeStage::Usable;
      _rrAvailability.reason.clear();
      log.Info(log::RENDER,
               "DlssProvider: Initialize — slIsFeatureSupported(kFeatureDLSS_RR) OK; DLSS-RR "
               "(Ray Reconstruction) is usable on this adapter/driver -- replaces the builtin "
               "denoiser chain + SR when active");
    }
  }
#endif  // _WIN32
}

}  // namespace pyxis
