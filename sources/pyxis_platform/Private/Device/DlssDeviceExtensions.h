// Pyxis platform — Streamline (DLSS) pre-device-creation bootstrap.
//
// DLSS Stage 2a (rtx-realtime-alignment-design.md, "DLSS scope includes
// upscaling"). ProgrammingGuideManualHooking.md, section "3.0 INITIALIZATION
// AND SHUTDOWN": "Unlike regular SL integrations, the D3D device can be
// created before or after slInit is called. When using Vulkan however,
// device still must be created AFTER the slInit call." Section "5.2.1
// INSTANCE AND DEVICE ADDITIONS": "SL features can request special
// extensions, device features or even modifications to the number of
// command queues... therefore BEFORE creating VK instance and device you
// must call slGetFeatureRequirements for each enabled feature."
//
// Both constraints land squarely in pyxis_platform's VkDeviceManager /
// VkDeviceManagerHeadless, which create the VkInstance + VkDevice BEFORE
// pyxis_renderer (and its Private/Dlss/DlssProvider, the Stage 1 capability
// probe) even exists. This header is therefore the one place in the whole
// stack where pyxis_platform gains (guarded, optional) Streamline
// awareness: it discovers + loads sl.interposer.dll, calls slInit with the
// manual-hooking preference flag, and asks slGetFeatureRequirements(
// kFeatureDLSS) which extra Vulkan instance/device extensions the DLSS
// plugin needs -- purely so VkDeviceManager can fold them into the SAME
// extension vectors it already builds (mirrors ExternalInterop.h's
// AppendExternalInteropExtensionsIfAvailable pattern one file up: guarded,
// additive, never fatal). It does NOT call slSetVulkanInfo or
// slIsFeatureSupported -- those need the fully-created device/queue and
// are DlssProvider's job (Private/Dlss/DlssProvider.cpp, pyxis_renderer),
// once PyxisRenderer is constructed with the resulting VulkanContext.
//
// Never vendors proprietary binaries (NVIDIA RTX SDKs License -- see
// DlssProvider.h's file comment); sl.interposer.dll is always
// runtime-loaded from OUTSIDE the repo. On ANY failure (DLL not staged,
// slInit rejected, feature not available) this returns an all-false/empty
// result and the caller's device-creation extension lists are simply left
// unchanged -- non-NVIDIA / SDK-less machines boot exactly as before.
//
// Scope cut (documented, not silently skipped): this queries EXTENSION
// NAMES only. slGetFeatureRequirements also returns Vulkan 1.2/1.3
// FEATURE-bit requests (vkFeatures12/vkFeatures13, merged via
// sl_helpers_vk.h's getVkPhysicalDeviceVulkan12Features/13Features
// helpers); VkDeviceManager already enables a broad superset of common
// 1.2/1.3 features (descriptor indexing, buffer device address, timeline
// semaphores, synchronization2, dynamic rendering -- see its
// VkPhysicalDeviceVulkan12/13Features blocks), so the fine-grained
// feature-bit merge is not implemented here. If slSetVulkanInfo /
// slIsFeatureSupported later fails because of a missing feature bit,
// DlssProvider reports that reason and the renderer falls back to the
// builtin denoiser -- the same graceful-degradation contract as every
// other Stage 1/2 failure mode.

#pragma once

#include <string>
#include <vector>

namespace pyxis {

struct DlssDeviceRequirements {
  // True iff slInit succeeded (manual-hooking mode, kFeatureDLSS
  // requested) AND slGetFeatureRequirements(kFeatureDLSS) succeeded.
  // False on every other outcome (DLL not staged, symbol missing, slInit
  // rejected the preferences, feature plugin failed to load).
  bool streamlineActive = false;
  // Extra VK_KHR_/VK_EXT_ instance extension names Streamline wants,
  // owned copies (the FeatureRequirements' own `const char**` arrays are
  // only guaranteed valid while the plugin DLL stays loaded -- which it
  // does for the process lifetime here, but an owned copy removes any
  // doubt and outlives the local FeatureRequirements value regardless).
  std::vector<std::string> instanceExtensions;
  std::vector<std::string> deviceExtensions;
};

// Attempts to bring up Streamline in MANUAL HOOKING mode before the Vulkan
// instance exists. Call this ONCE, at the very top of
// VkDeviceManager(Headless)::Bringup(), before CreateInstance(...); fold
// the returned instanceExtensions into the instance-creation extension
// list and the returned deviceExtensions into the device-creation
// extension list (both additive -- never removes anything the caller
// already requested).
//
// Discovery mirrors DlssProvider::Probe()'s Stage 1 logic exactly
// (PYXIS_DLSS_PATH env var naming a directory, else <exe-dir>) --
// duplicated rather than shared because pyxis_platform cannot depend on
// pyxis_renderer's Private/ headers (four-layer stack, plan §1); see this
// header's own file comment for why that's an acceptable, documented
// scope cut rather than an oversight.
//
// Intentionally leaks the loaded sl.interposer.dll module (never calls
// FreeLibrary) -- Streamline is meant to stay initialised for the whole
// process; DlssProvider's later LoadLibraryW call against the same path
// just bumps the OS refcount and returns the same HMODULE.
[[nodiscard]] DlssDeviceRequirements TryBootstrapStreamlineForVulkan() noexcept;

// Calls slShutdown() iff `active` is true (i.e. TryBootstrapStreamlineForVulkan
// returned streamlineActive=true on this device manager). No-op otherwise.
// ProgrammingGuideDLSS.md 1.0: "Call slShutdown() before destroying
// dxgi/d3d11/d3d12/vk instances, devices and other components" -- callers
// must invoke this BEFORE vkDestroyDevice/vkDestroyInstance in their
// Teardown().
void ShutdownStreamlineIfActive(bool active) noexcept;

}  // namespace pyxis
