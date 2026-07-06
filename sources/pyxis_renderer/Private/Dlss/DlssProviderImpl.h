// Pyxis renderer — DlssProvider::Impl (DLSS Stage 2a).
//
// The resolved Streamline function pointers + viewport/frame-token state,
// split out of DlssProvider.h so that PUBLIC-to-this-DLL header stays
// free of <sl.h> (mirrors DlssProvider.h's own file comment). Included by
// BOTH DlssProvider.cpp (ctor/dtor/Probe/Initialize) and
// DlssProviderFrame.cpp (GetOptimalRenderResolution/Evaluate/
// ReleaseResources) — the two-file split follows the project's "long
// methods split per-phase" convention applied to a non-PIMPL-by-default
// class that grew too large for one file once Stage 2a's per-frame path
// landed.

#pragma once

#include "Dlss/DlssProvider.h"

// sl_helpers_vk.h uses VkPhysicalDeviceVulkan12/13Features etc. without
// including <vulkan/vulkan.h> itself (it expects the app to have already
// included it with whatever VK_USE_PLATFORM_* macro it needs) — must come
// first.
#include <vulkan/vulkan.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>

namespace pyxis {

struct DlssProvider::Impl {
  // Core API (sl_core_api.h) — resolved in Initialize() via GetProcAddress
  // against the SAME sl.interposer.dll module Stage 1's Probe() already
  // loaded (_interposerModule).
  PFun_slSetVulkanInfo* slSetVulkanInfo = nullptr;
  PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
  PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
  PFun_slSetConstants* slSetConstants = nullptr;
  PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
  PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
  PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
  PFun_slAllocateResources* slAllocateResources = nullptr;
  PFun_slFreeResources* slFreeResources = nullptr;

  // DLSS-specific (sl_dlss.h) — resolved via slGetFeatureFunction(
  // kFeatureDLSS, "slDLSS...", ptr) rather than the header's own inline
  // helpers (those assume slGetFeatureFunction is statically linked,
  // which Pyxis deliberately never does — see DlssProvider.cpp).
  PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
  PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;

  // v1 — exactly one viewport (the whole display), matching PyxisRenderer's
  // own single-viewport render graph. Multi-viewport (§8.0 of
  // ProgrammingGuideDLSS.md) is out of scope.
  sl::ViewportHandle viewport{0};

  // True once slAllocateResources(kFeatureDLSS, viewport) has been called
  // at least once (or the first slEvaluateFeature implicitly allocated it)
  // — gates ReleaseResources() so it doesn't call slFreeResources on a
  // feature that was never actually allocated.
  bool resourcesEverEvaluated = false;

  // True once Evaluate() has run successfully at least once. Combined
  // with lastRenderWidth/Height below, Evaluate() derives
  // sl::Constants::reset itself (first frame OR a render-resolution
  // change both mean "no valid history") so DlssPass's caller doesn't
  // have to track history-validity state of its own.
  bool hasEvaluatedOnce = false;
  uint32_t lastRenderWidth = 0;
  uint32_t lastRenderHeight = 0;
};

}  // namespace pyxis
