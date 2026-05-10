// Pyxis app — Editor panel implementation.
//
// Defines `void ImGuiHost::BuildEditorPanel(GpuScene&)` plus the two
// COM file-dialog helpers used only by the editor (Open scene + Save
// AOV). Split out of ImGuiHost.cpp so that file stays focused on
// lifecycle, theme, init, and the always-on Stats / Performance
// panels — those plus this one were the audit's three "outgrown"
// concerns.
//
// Same TU-as-class trick as TextureReadback / AovExrSaver: the
// function is a member of pyxis::app::ImGuiHost defined in a
// separate .cpp; full access to private fields, no extra friend
// declarations needed.

#include "ImGuiHost.h"

#include "Render/AovRegistry.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/PickResult.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <hlsl++.h>
#include <imgui.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <objbase.h>
    #include <shobjidl.h>
#endif

namespace pyxis::app {

namespace {

#if defined(_WIN32)
// Modal Windows file-SAVE dialog. Returns the path the user picked (or
// the typed-in path if they overrode the suggestion); empty on cancel.
// Defaults to the supplied filename and EXR filter so the AOV save
// flow always lands at <foo>.exr.
std::string SaveFilePickerDialog(std::wstring_view defaultFileName) noexcept {
  HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool weInitedCom = SUCCEEDED(comResult);

  IFileSaveDialog* dialog = nullptr;
  comResult = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_IFileSaveDialog,
                               reinterpret_cast<void**>(&dialog));
  if (FAILED(comResult) || !dialog)
  {
    if (weInitedCom)
      CoUninitialize();
    return {};
  }

  COMDLG_FILTERSPEC filterSpec[] = {
      {L"OpenEXR (*.exr)", L"*.exr"},
  };
  dialog->SetFileTypes(static_cast<UINT>(std::size(filterSpec)), filterSpec);
  dialog->SetTitle(L"Pyxis - Save AOV as EXR");
  dialog->SetDefaultExtension(L"exr");
  if (!defaultFileName.empty())
  {
    const std::wstring nameOwned{defaultFileName};
    dialog->SetFileName(nameOwned.c_str());
  }

  comResult = dialog->Show(nullptr);
  std::string result;
  if (SUCCEEDED(comResult))
  {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item)
    {
      PWSTR widePath = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath)
      {
        const int byteCount =
            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
        if (byteCount > 1)
        {
          result.resize(static_cast<std::size_t>(byteCount - 1));
          WideCharToMultiByte(CP_UTF8, 0, widePath, -1, result.data(), byteCount, nullptr,
                              nullptr);
        }
        CoTaskMemFree(widePath);
      }
      item->Release();
    }
  }
  dialog->Release();
  if (weInitedCom)
    CoUninitialize();
  return result;
}

// Modal Windows file-open dialog (IFileOpenDialog COM interface).
// Returns the selected path on OK; empty string on Cancel / error /
// the user dismissing the dialog. Filters to USD-family extensions
// (.usd / .usda / .usdc / .usdz) since those are the only paths
// either ingest adapter can hand to UsdStage::Open.
//
// COM threading: we initialise STA on the calling thread for the
// duration of the call. That's safe even if Vulkan / GLFW also
// initialised COM on this thread — CoInitializeEx returns
// RPC_E_CHANGED_MODE if a different threading model is already
// installed and we treat that as "already initialised" (still safe
// to call CoCreateInstance).
std::string OpenScenePickerDialog() noexcept {
  HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool weInitedCom = SUCCEEDED(comResult);
  if (comResult == RPC_E_CHANGED_MODE)
  {
    // Some other component already initialised COM on this thread
    // with a different model — that's fine, we can still use the
    // dialog. Don't call CoUninitialize at the end in that case.
  }

  IFileOpenDialog* dialog = nullptr;
  comResult = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog,
                               reinterpret_cast<void**>(&dialog));
  if (FAILED(comResult) || !dialog)
  {
    if (weInitedCom)
      CoUninitialize();
    return {};
  }

  COMDLG_FILTERSPEC filterSpec[] = {
      {L"USD scenes (*.usd;*.usda;*.usdc;*.usdz)", L"*.usd;*.usda;*.usdc;*.usdz"},
      {L"All files",                                L"*.*"},
  };
  dialog->SetFileTypes(static_cast<UINT>(std::size(filterSpec)), filterSpec);
  dialog->SetTitle(L"Pyxis - Open scene");

  comResult = dialog->Show(nullptr);
  std::string result;
  if (SUCCEEDED(comResult))
  {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item)
    {
      PWSTR widePath = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath)
      {
        const int byteCount =
            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
        if (byteCount > 1)
        {
          result.resize(static_cast<std::size_t>(byteCount - 1));
          WideCharToMultiByte(CP_UTF8, 0, widePath, -1, result.data(), byteCount, nullptr,
                              nullptr);
        }
        CoTaskMemFree(widePath);
      }
      item->Release();
    }
  }
  dialog->Release();
  if (weInitedCom)
    CoUninitialize();
  return result;
}
#else
std::string SaveFilePickerDialog(std::wstring_view) noexcept { return {}; }
std::string OpenScenePickerDialog() noexcept { return {}; }
#endif

}  // namespace

void ImGuiHost::BuildEditorPanel(GpuScene& scene) noexcept {
  if (!_ready)
    return;

  // Layout: Editor sits directly under the Scene panel (top-left,
  // anchored each frame using the height Scene reported the previous
  // frame). AlwaysAutoResize so the user sees the entire panel
  // without scrolling. NoCollapse + NoSavedSettings so the layout
  // is stable across launches.
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + 10.0f,
             viewport->WorkPos.y + 10.0f + _layoutScenePanelHeight + 6.0f),
      ImGuiCond_Always);
  if (ImGui::Begin("Editor", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
  {
    // ---- Top-level actions -------------------------------------------
    // Both buttons latch requests drained by ViewerMode (per-frame).
    // - Reload shaders: re-reads .spv from disk + rebuilds RT pipeline.
    //   Slang -> SPIR-V is a CMake build-step (ShaderMake), so the
    //   workflow is "rebuild shaders externally first, then click".
    // - Open scene: pops a Win COM IFileOpenDialog filtered to USD;
    //   the picked path latches into _latches.sceneReloadPath and
    //   ViewerMode handles waitForIdle + GpuScene::Clear + re-ingest.
    if (_shaderRebuildInFlight)
    {
      // Worker thread is spawning cmake / waiting for exit. Disable
      // the button so a second click can't queue parallel rebuilds.
      ImGui::BeginDisabled();
      ImGui::Button("Rebuilding shaders...");
      ImGui::EndDisabled();
    }
    else if (ImGui::Button("Reload shaders"))
    {
      _latches.reloadShaders = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open scene..."))
    {
      std::string picked = OpenScenePickerDialog();
      if (!picked.empty())
        _latches.sceneReloadPath = std::move(picked);
    }
    if (!_latches.sceneReloadPath.empty())
    {
      ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.20f, 1.0f),
                         "Pending scene reload: %s", _latches.sceneReloadPath.c_str());
    }
    ImGui::Spacing();

    // ---- AOV inspector ------------------------------------------------
    // Combo selects which raw AOV the raygen remaps into the BGRA8
    // display target (Color = post-tonemap, Normal = (n*0.5+0.5),
    // Depth = 1/depth grayscale, PrimID = hashed palette). The
    // pixel-picker readout below shows the RAW values at the cursor
    // pulled from PyxisRenderer::LastPickResult() — one frame stale
    // (acceptable for hover-feedback). Save button kicks an EXR save
    // of the currently-selected RAW AOV.
    if (ImGui::CollapsingHeader("AOV inspector"))
    {
      // Combo iterates AOV_REGISTRY directly — single source of truth
      // for (DebugView -> displayLabel + name + texture) shared with
      // ViewerMode's Save AOV button + HeadlessMode's --save-aov
      // dispatch. Pre-refactor a parallel AOV_LABELS[] sat here and
      // had to stay in lockstep with the registry.
      const AovEntry* currentEntry = FindAovByDebugView(_editorDebugView);
      const std::string_view preview =
          (currentEntry != nullptr) ? currentEntry->displayLabel : std::string_view{"?"};
      // ImGui::BeginCombo wants null-terminated; copy into a small
      // stack buffer (display labels are <= 16 chars by construction).
      char previewCStr[32];
      std::snprintf(previewCStr, sizeof(previewCStr), "%.*s",
                    static_cast<int>(preview.size()), preview.data());
      if (ImGui::BeginCombo("Display", previewCStr))
      {
        for (const AovEntry& entry : AOV_REGISTRY)
        {
          const bool isSelected = (entry.debugView == _editorDebugView);
          char itemLabel[32];
          std::snprintf(itemLabel, sizeof(itemLabel), "%.*s",
                        static_cast<int>(entry.displayLabel.size()),
                        entry.displayLabel.data());
          if (ImGui::Selectable(itemLabel, isSelected))
            _editorDebugView = entry.debugView;
          if (isSelected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }

      // Per-AOV display knob: WorldPos period slider. Only useful
      // (and only visible) when a WorldPos display is picked.
      // Range: 0.1 m for unit-scale fixtures up to 200 m for
      // Bistro / outdoor environments. ViewerMode pushes the value
      // into RenderSettings::worldPosPeriod each frame; the same
      // slider feeds both world-space + eye-space periodic encodes
      // (raygen branches both on `gFrameUi.worldPosPeriod`).
      if (_editorDebugView == RenderSettings::DebugView::WorldPos
          || _editorDebugView == RenderSettings::DebugView::WorldPosEye)
      {
        ImGui::PushItemWidth(180.0f);
        ImGui::SliderFloat("WorldPos period (m)", &_editorWorldPosPeriod,
                           0.1f, 200.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::PopItemWidth();
      }

      // Pixel picker — readout from the prior frame's PickResult.
      // Shows EVERY AOV channel the inspector exposes (mirrors the
      // PickResult struct that raygen filled), plus the pixel coords
      // so users can correlate hover position against the image.
      // F key toggles pin/follow: pinned mode freezes the sampled
      // pixel so the user can move the camera and watch the values
      // at that exact world point change. Toggle latches the CURRENT
      // pick pixel (1-frame stale, but that matches everything else
      // we display here).
      ImGui::Separator();
      const PickResult& pick = _editorLastPick;
      if (ImGui::IsKeyPressed(ImGuiKey_F))
      {
        if (!_pickerPinned && pick.pixelX != PICK_PIXEL_NONE)
        {
          // Capture as normalised UV so a window resize after pinning
          // keeps the pin at roughly the same screen location. +0.5
          // lands the UV on the pixel CENTER so denormalisation against
          // the same dims round-trips back to the same integer pixel.
          _pickerPinnedU =
              (static_cast<float>(pick.pixelX) + 0.5f) / static_cast<float>(_renderWidth);
          _pickerPinnedV =
              (static_cast<float>(pick.pixelY) + 0.5f) / static_cast<float>(_renderHeight);
          _pickerPinned = true;
        }
        else
        {
          _pickerPinned = false;
        }
      }
      const bool pickerActive = (pick.pixelX != PICK_PIXEL_NONE);
      if (_pickerPinned)
      {
        const uint32_t pinnedDispX =
            static_cast<uint32_t>(_pickerPinnedU * static_cast<float>(_renderWidth));
        const uint32_t pinnedDispY =
            static_cast<uint32_t>(_pickerPinnedV * static_cast<float>(_renderHeight));
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.26f, 1.0f),
                           "Picker  PINNED @ (%u, %u)  [press F to follow mouse]",
                           static_cast<unsigned int>(pinnedDispX),
                           static_cast<unsigned int>(pinnedDispY));
      }
      else if (pickerActive)
      {
        ImGui::Text("Picker  following mouse @ (%u, %u)  [press F to pin]",
                    static_cast<unsigned int>(pick.pixelX),
                    static_cast<unsigned int>(pick.pixelY));
      }
      else
      {
        ImGui::Text("Picker  following mouse  [press F to pin]");
      }
      const bool didHit = (pick.depth > 0.0f);
      if (didHit)
      {
        // Units: depth + worldPos are in scene units (typically
        // metres — USD's stage metersPerUnit defaults to 1). Suffix
        // makes the magnitude unambiguous when the user compares
        // against the viewport / RenderDoc. Order mirrors AOV_REGISTRY
        // so the readout sits in the same vertical sequence as the
        // Display combo above (color / normal / depth / primId /
        // materialId / baseColor / worldPos / alpha / elementId /
        // normalEye / worldPosEye).
        ImGui::Text("  color        %.3f, %.3f, %.3f",
                    static_cast<double>(pick.colorR),
                    static_cast<double>(pick.colorG),
                    static_cast<double>(pick.colorB));
        ImGui::Text("  normal       %.3f, %.3f, %.3f",
                    static_cast<double>(pick.normalX),
                    static_cast<double>(pick.normalY),
                    static_cast<double>(pick.normalZ));
        ImGui::Text("  depth        %.3f m", static_cast<double>(pick.depth));
        ImGui::Text("  primId       %u", static_cast<unsigned int>(pick.instanceId));
        ImGui::Text("  materialId   %u", static_cast<unsigned int>(pick.materialId));
        ImGui::Text("  baseColor    %.3f, %.3f, %.3f",
                    static_cast<double>(pick.baseColorR),
                    static_cast<double>(pick.baseColorG),
                    static_cast<double>(pick.baseColorB));
        ImGui::Text("  worldPos     %.3f, %.3f, %.3f m",
                    static_cast<double>(pick.worldHitX),
                    static_cast<double>(pick.worldHitY),
                    static_cast<double>(pick.worldHitZ));
        // Tier 1 Hydra-canonical readouts.
        ImGui::Text("  alpha        %.3f", static_cast<double>(pick.alpha));
        ImGui::Text("  elementId    %u", static_cast<unsigned int>(pick.elementId));
        ImGui::Text("  normalEye    %.3f, %.3f, %.3f",
                    static_cast<double>(pick.normalEyeX),
                    static_cast<double>(pick.normalEyeY),
                    static_cast<double>(pick.normalEyeZ));
        ImGui::Text("  worldPosEye  %.3f, %.3f, %.3f m",
                    static_cast<double>(pick.worldPosEyeX),
                    static_cast<double>(pick.worldPosEyeY),
                    static_cast<double>(pick.worldPosEyeZ));
      }
      else
      {
        ImGui::TextDisabled("  (cursor over background or outside viewport)");
      }

      // Save current AOV — pops a Win COM IFileSaveDialog filtered to
      // *.exr; the picked path is latched into _latches.saveAovPath
      // and ViewerMode drains it via TakeSaveAovRequest() to perform
      // the readback + EXR write of the currently-selected raw AOV.
      // Suggested filename comes from the AOV registry (single source
      // of truth shared with HeadlessMode's --save-aov dispatch).
      ImGui::Separator();
      if (ImGui::Button("Save current AOV..."))
      {
        // Suggested filename: shared BuildAovOutputPath helper (used
        // by HeadlessMode's --save-aov dispatch too) so editor
        // suggestions stay in lockstep with the headless naming
        // convention. ASCII-narrow -> wide for the Win COM dialog.
        std::string suggestedName = "pyxis_aov_color.exr";
        if (const AovEntry* entry = FindAovByDebugView(_editorDebugView); entry != nullptr)
        {
          suggestedName = BuildAovOutputPath("pyxis_aov", entry->name);
        }
        std::wstring suggested;
        suggested.reserve(suggestedName.size());
        for (const char ascii : suggestedName)
          suggested.push_back(static_cast<wchar_t>(ascii));
        std::string picked = SaveFilePickerDialog(suggested);
        if (!picked.empty())
          _latches.saveAovPath = std::move(picked);
      }
      if (!_latches.saveAovPath.empty())
      {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.20f, 1.0f),
                           "Pending save: %s", _latches.saveAovPath.c_str());
      }

      // Viewport overlay: when the picker is pinned, draw a small
      // crosshair on top of the rendered image at the pinned UV's
      // screen position. ImGui::GetForegroundDrawList draws on top
      // of every panel + the swapchain blit, so the crosshair is
      // always visible regardless of which panel has focus.
      if (_pickerPinned)
      {
        ImGuiViewport* fgViewport = ImGui::GetMainViewport();
        const ImVec2 origin = fgViewport->WorkPos;
        const ImVec2 size   = fgViewport->WorkSize;
        const ImVec2 center{
            origin.x + _pickerPinnedU * size.x,
            origin.y + _pickerPinnedV * size.y,
        };
        ImDrawList* drawList = ImGui::GetForegroundDrawList(fgViewport);
        const ImU32  crossColor = IM_COL32(255, 158, 66, 255);  // Pyxis orange
        const ImU32  haloColor  = IM_COL32(0, 0, 0, 200);
        const float  arm        = 8.0f;
        const float  gap        = 2.0f;
        drawList->AddLine(ImVec2(center.x - arm, center.y),
                          ImVec2(center.x - gap, center.y), haloColor, 3.0f);
        drawList->AddLine(ImVec2(center.x + gap, center.y),
                          ImVec2(center.x + arm, center.y), haloColor, 3.0f);
        drawList->AddLine(ImVec2(center.x, center.y - arm),
                          ImVec2(center.x, center.y - gap), haloColor, 3.0f);
        drawList->AddLine(ImVec2(center.x, center.y + gap),
                          ImVec2(center.x, center.y + arm), haloColor, 3.0f);
        drawList->AddLine(ImVec2(center.x - arm, center.y),
                          ImVec2(center.x - gap, center.y), crossColor, 1.5f);
        drawList->AddLine(ImVec2(center.x + gap, center.y),
                          ImVec2(center.x + arm, center.y), crossColor, 1.5f);
        drawList->AddLine(ImVec2(center.x, center.y - arm),
                          ImVec2(center.x, center.y - gap), crossColor, 1.5f);
        drawList->AddLine(ImVec2(center.x, center.y + gap),
                          ImVec2(center.x, center.y + arm), crossColor, 1.5f);
      }
    }

    // ---- Camera section ----------------------------------------------
    // FOV (vertical, degrees) and Focal length (mm) are linked via a
    // 24mm full-frame sensor height. Any projection-affecting slider
    // rebuilds projFromView using the GL-convention perspective matrix
    // (raygen does its own Y-flip in NDC mapping). Position +
    // orientation stay out of the editor — FlyCameraController owns
    // them in viewer mode and an editor slider would fight it
    // every frame.
    if (scene.HasCamera())
    {
      if (ImGui::CollapsingHeader("Camera"))
      {
        // Scene-camera combo. List of every camera the StageWalker
        // emitted (fed in via SetSceneCameras). Selecting an entry
        // latches the index → ViewerMode drains it and snaps the
        // FlyCam to that camera's transform on the next frame. The
        // same combo doubles as a record of what the scene authored
        // so the user knows what's available.
        if (!_sceneCameras.empty())
        {
          const int currentIdx = (_sceneCameraSelectedIndex >= 0
                                  && _sceneCameraSelectedIndex
                                       < static_cast<int>(_sceneCameras.size()))
                                     ? _sceneCameraSelectedIndex
                                     : 0;
          const std::string& currentName = _sceneCameras[static_cast<std::size_t>(currentIdx)].name;
          char preview[256];
          std::snprintf(preview, sizeof(preview), "%s", currentName.c_str());
          ImGui::PushItemWidth(260.0f);
          if (ImGui::BeginCombo("Scene Camera", preview))
          {
            for (std::size_t i = 0; i < _sceneCameras.size(); ++i)
            {
              const bool isSelected = (static_cast<int>(i) == currentIdx);
              char itemLabel[256];
              std::snprintf(itemLabel, sizeof(itemLabel), "%s",
                            _sceneCameras[i].name.c_str());
              if (ImGui::Selectable(itemLabel, isSelected))
              {
                _sceneCameraSelectedIndex = static_cast<int>(i);
                _latches.snapToSceneCameraIndex = static_cast<int>(i);
              }
              if (isSelected)
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }
          ImGui::PopItemWidth();
        }

        // FlyCam linear move-speed slider. Logarithmic so a Bistro-
        // scale scene (10–50 m/s feels right) and a unit-cube fixture
        // (~0.5 m/s feels right) both have usable resolution. Angular
        // sensitivity is intentionally NOT exposed — mouse drag at
        // the §29.2 default works across DPI / scale.
        ImGui::PushItemWidth(180.0f);
        ImGui::SliderFloat("Move speed (m/s)", &_moveSpeed, 0.05f, 200.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::PopItemWidth();
        ImGui::Separator();

        CameraDesc cameraDesc = scene.GetCamera();
        bool cameraEdited      = false;
        bool projectionEdited  = false;

        constexpr float SENSOR_HEIGHT_MM = 24.0f;
        constexpr float DEG_PER_RAD      = 57.29577951308232f;
        constexpr float RAD_PER_DEG      =  0.01745329251994329f;
        float fovYDegrees =
            2.0f * std::atan(SENSOR_HEIGHT_MM / (2.0f * cameraDesc.focalLengthMm))
            * DEG_PER_RAD;

        ImGui::PushItemWidth(180.0f);
        if (ImGui::SliderFloat("FOV (deg, vertical)", &fovYDegrees, 5.0f, 130.0f, "%.1f"))
        {
          const float fovYRad = fovYDegrees * RAD_PER_DEG;
          cameraDesc.focalLengthMm = (SENSOR_HEIGHT_MM * 0.5f) / std::tan(fovYRad * 0.5f);
          cameraEdited = true;
          projectionEdited = true;
        }
        if (ImGui::SliderFloat("Focal length (mm)", &cameraDesc.focalLengthMm, 12.0f,
                               200.0f, "%.1f"))
        {
          cameraEdited = true;
          projectionEdited = true;
        }
        if (ImGui::SliderFloat("Focus distance",    &cameraDesc.focusDistance,    0.1f,
                               50.0f, "%.2f"))
          cameraEdited = true;
        if (ImGui::SliderFloat("Near clip",         &cameraDesc.nearClip,         0.01f,
                               1.0f, "%.3f"))
        {
          cameraEdited = true;
          projectionEdited = true;
        }
        if (ImGui::SliderFloat("Far clip",          &cameraDesc.farClip,          10.0f,
                               5000.0f, "%.0f"))
        {
          cameraEdited = true;
          projectionEdited = true;
        }
        ImGui::PopItemWidth();

        if (projectionEdited)
        {
          const float fovYRad = 2.0f * std::atan(
              SENSOR_HEIGHT_MM / (2.0f * cameraDesc.focalLengthMm));
          const float focal   = 1.0f / std::tan(fovYRad * 0.5f);
          const float aspect  = _renderAspect;
          const float nearZ   = cameraDesc.nearClip;
          const float farZ    = cameraDesc.farClip;
          const float nearMinusFar = nearZ - farZ;
          cameraDesc.projFromView = hlslpp::float4x4(
              hlslpp::float4(focal / aspect, 0.0f, 0.0f, 0.0f),
              hlslpp::float4(0.0f,           focal, 0.0f, 0.0f),
              hlslpp::float4(0.0f, 0.0f, farZ / nearMinusFar,
                             nearZ * farZ / nearMinusFar),
              hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f));
        }

        if (cameraEdited)
          scene.SetCamera(cameraDesc);
      }
    }

    // ---- Lights section ----------------------------------------------
    if (ImGui::CollapsingHeader("Lights"))
    {
      const uint32_t lightCount = scene.GetLiveLightCount();
      if (lightCount == 0)
      {
        ImGui::TextDisabled("(no lights authored)");
      }
      else
      {
        if (_editorLightIndex >= lightCount)
          _editorLightIndex = 0;
        char previewLabel[64];
        {
          const LightDesc previewDesc = scene.GetLightDescAt(_editorLightIndex);
          const char* kindLabel = "Distant";
          if (previewDesc.kind == LightDesc::Kind::Dome) kindLabel = "Dome";
          if (previewDesc.kind == LightDesc::Kind::Rect) kindLabel = "Rect";
          std::snprintf(previewLabel, sizeof(previewLabel), "Light #%u  %s",
                        _editorLightIndex, kindLabel);
        }
        if (ImGui::BeginCombo("Light", previewLabel))
        {
          for (uint32_t lightIdx = 0; lightIdx < lightCount; ++lightIdx)
          {
            const LightDesc itemDesc = scene.GetLightDescAt(lightIdx);
            const char* kindLabel = "Distant";
            if (itemDesc.kind == LightDesc::Kind::Dome) kindLabel = "Dome";
            if (itemDesc.kind == LightDesc::Kind::Rect) kindLabel = "Rect";
            char itemLabel[64];
            std::snprintf(itemLabel, sizeof(itemLabel), "Light #%u  %s", lightIdx,
                          kindLabel);
            const bool isSelected = (lightIdx == _editorLightIndex);
            if (ImGui::Selectable(itemLabel, isSelected))
              _editorLightIndex = lightIdx;
            if (isSelected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }

        const LightHandle handle = scene.GetLightHandleAt(_editorLightIndex);
        LightDesc desc = scene.GetLightDescAt(_editorLightIndex);
        bool lightEdited = false;

        float color[3] = {static_cast<float>(desc.color.x),
                          static_cast<float>(desc.color.y),
                          static_cast<float>(desc.color.z)};
        if (ImGui::ColorEdit3("Color", color, ImGuiColorEditFlags_NoInputs))
        {
          desc.color = hlslpp::float3{color[0], color[1], color[2]};
          lightEdited = true;
        }
        if (ImGui::SliderFloat("Intensity", &desc.intensity, 0.0f, 10.0f, "%.2f"))
          lightEdited = true;

        if (lightEdited && handle != LightHandle::Invalid)
          scene.UpdateLight(handle, desc);
      }
    }

    // ---- Materials section -------------------------------------------
    // Click-to-select: drain ViewerMode's pending click latch BEFORE
    // the CollapsingHeader so we can both (a) snap the Material combo
    // index and (b) force-open the header even if the user had
    // collapsed it. Without (b), clicking a sphere would update the
    // combo behind a closed header — silent change, bad UX.
    bool clickForceOpen = false;
    if (_latches.clickInstanceSlot != INSTANCE_ID_NONE)
    {
      const MaterialHandle clickedMat =
          scene.LookupInstanceMaterialBySlot(_latches.clickInstanceSlot);
      if (clickedMat != MaterialHandle::Invalid)
      {
        const uint32_t lookupCount = scene.GetLiveMaterialCount();
        for (uint32_t walkIdx = 0; walkIdx < lookupCount; ++walkIdx)
        {
          if (scene.GetMaterialHandleAt(walkIdx) == clickedMat)
          {
            _editorMaterialIndex = walkIdx;
            clickForceOpen = true;
            break;
          }
        }
      }
      _latches.clickInstanceSlot = INSTANCE_ID_NONE;
    }
    if (clickForceOpen)
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader("Materials"))
    {
      const uint32_t matCount = scene.GetLiveMaterialCount();
      if (matCount == 0)
      {
        ImGui::TextDisabled("(no materials authored)");
      }
      else
      {
        if (_editorMaterialIndex >= matCount)
          _editorMaterialIndex = 0;
        // Helper: extract the final SdfPath segment from a string_view
        // (everything after the last '/'). For "/Foo/Bar/Baz" returns
        // "Baz"; for an empty path returns the synthesized index name.
        auto materialLabel = [&](uint32_t liveIdx, char* buf, std::size_t bufSize) {
          const OpenPBRMaterialDesc itemDesc = scene.GetMaterialDescAt(liveIdx);
          const std::string_view path = itemDesc.sourcePrim;
          if (path.empty())
          {
            std::snprintf(buf, bufSize, "Material #%u", liveIdx);
            return;
          }
          const auto lastSlash = path.find_last_of('/');
          const std::string_view leaf = (lastSlash == std::string_view::npos)
                                            ? path
                                            : path.substr(lastSlash + 1);
          std::snprintf(buf, bufSize, "%.*s", static_cast<int>(leaf.size()),
                        leaf.data());
        };

        char previewLabel[80];
        materialLabel(_editorMaterialIndex, previewLabel, sizeof(previewLabel));
        if (ImGui::BeginCombo("Material", previewLabel))
        {
          for (uint32_t matIdx = 0; matIdx < matCount; ++matIdx)
          {
            char itemLabel[80];
            materialLabel(matIdx, itemLabel, sizeof(itemLabel));
            const bool isSelected = (matIdx == _editorMaterialIndex);
            if (ImGui::Selectable(itemLabel, isSelected))
              _editorMaterialIndex = matIdx;
            if (isSelected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }

        const MaterialHandle handle = scene.GetMaterialHandleAt(_editorMaterialIndex);
        OpenPBRMaterialDesc desc = scene.GetMaterialDescAt(_editorMaterialIndex);
        bool matEdited = false;

        float baseColor[3] = {static_cast<float>(desc.baseColor.x),
                              static_cast<float>(desc.baseColor.y),
                              static_cast<float>(desc.baseColor.z)};
        if (ImGui::ColorEdit3("Base color", baseColor, ImGuiColorEditFlags_NoInputs))
        {
          desc.baseColor = hlslpp::float3{baseColor[0], baseColor[1], baseColor[2]};
          matEdited = true;
        }
        if (ImGui::SliderFloat("Roughness", &desc.roughness, 0.0f, 1.0f, "%.3f"))
          matEdited = true;
        if (ImGui::SliderFloat("Metalness", &desc.metalness, 0.0f, 1.0f, "%.3f"))
          matEdited = true;
        if (ImGui::SliderFloat("Opacity",   &desc.opacity,   0.0f, 1.0f, "%.3f"))
          matEdited = true;
        if (ImGui::SliderFloat("Specular IoR", &desc.specularIor, 1.0f, 3.0f, "%.3f"))
          matEdited = true;

        if (matEdited && handle != MaterialHandle::Invalid)
          scene.UpdateMaterial(handle, desc);
      }
    }
  }
  ImGui::End();
}

}  // namespace pyxis::app
