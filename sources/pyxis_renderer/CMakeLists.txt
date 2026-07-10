# pyxis_renderer — plan §1 / §3.
#
# SHARED. flecs is linked PRIVATE so no Flecs header escapes Public/ (§30.11).
# spdlog is reached through pyxis::Logging::Get() in pyxis_platform; we do
# not link spdlog directly here (§33.10).

add_library(pyxis_renderer SHARED)
add_library(Pyxis::Renderer ALIAS pyxis_renderer)

pyxis_define_module(pyxis_renderer)
pyxis_disable_exceptions_and_rtti(pyxis_renderer)

# ----------------------------------------------------------------------------
# Public surface (M0 slice — narrow). Plan §18.1 lists the eventual
# inventory; M0 ships only what the SceneWorldInit unit test, the platform
# device manager and pyxis_app actually need.
# ----------------------------------------------------------------------------
target_sources(pyxis_renderer PUBLIC
    FILE_SET pyxis_renderer_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/Renderer/RendererApi.h
        Public/Pyxis/Renderer/Forward.h

        # M1 public surface.
        Public/Pyxis/Renderer/Profiler.h
        Public/Pyxis/Renderer/PyxisRenderer.h
        Public/Pyxis/Renderer/Descs/FrameProfile.h
        Public/Pyxis/Renderer/Descs/RendererCreateDesc.h
        Public/Pyxis/Renderer/Descs/RenderSettings.h
        Public/Pyxis/Renderer/Descs/RenderTargets.h

        # M3 public surface — foundation for Expected<T>-returning verbs.
        Public/Pyxis/Renderer/Error.h

        # M3 public Desc PODs. Byte-stable per §18.9 / §22.3 — adding
        # fields requires the trailing _reserved trick; reordering or
        # narrowing types is a major-version break. See each header
        # for milestone-specific notes (M3 only consumes the base
        # block of OpenPBRMaterialDesc, etc.).
        Public/Pyxis/Renderer/Descs/MeshDesc.h
        Public/Pyxis/Renderer/Descs/InstanceDesc.h
        Public/Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h
        Public/Pyxis/Renderer/Descs/CameraDesc.h
        Public/Pyxis/Renderer/Descs/LightDesc.h
        Public/Pyxis/Renderer/Descs/TextureKey.h
        Public/Pyxis/Renderer/Descs/VolumeDesc.h
        Public/Pyxis/Renderer/Descs/GpuSceneCreateDesc.h
        Public/Pyxis/Renderer/Descs/FrameStats.h

        # M3 GpuScene — canonical scene-mutation API. Stub impl in
        # Private/GpuScene/GpuScene.cpp; real bodies land in subsequent
        # M3 commits.
        Public/Pyxis/Renderer/GpuScene.h

        # DLSS Stage 1 (rtx-realtime-alignment-design.md) — status POD
        # returned by PyxisRenderer::GetDlssStatus().
        Public/Pyxis/Renderer/Descs/DlssStatus.h
)

# ----------------------------------------------------------------------------
# Private — SceneWorld and the Flecs phase pipeline (§8 / §30.11).
# ----------------------------------------------------------------------------
target_sources(pyxis_renderer PRIVATE
    # M3 — Error / PYXIS_ERROR implementation (foundation for Expected<T>).
    Private/Errors/Error.cpp

    # M3 — GpuScene canonical scene-mutation API. PIMPL'd per §18.9;
    # mesh upload + BLAS build + TLAS rebuild are wired here at M3,
    # materials + textures fill in at M5+. Per-verb bodies are split
    # into sibling files; Internal.h holds the shared `struct Impl`, with
    # the Flecs component PODs (Components.h) and the gpuscene_detail
    # hashing/packing/buffer helpers (Detail.h) split into sibling headers.
    Private/GpuScene/Internal.h
    Private/GpuScene/Components.h
    Private/GpuScene/Detail.h
    Private/GpuScene/GpuScene.cpp
    Private/GpuScene/Mesh.cpp
    Private/GpuScene/Material.cpp
    Private/GpuScene/Texture.cpp
    Private/GpuScene/Instance.cpp
    Private/GpuScene/Light.cpp
    Private/GpuScene/Volume.cpp
    Private/GpuScene/Commit.cpp
    Private/GpuScene/DdsParser.h
    Private/GpuScene/DdsParser.cpp
    # V2.A.14 — opt-in BCn (BC1/BC4/BC5) encoder. Split out of Commit.cpp
    # so the unit-test harness can call the helper without dragging in
    # stb_image / tinyexr / GpuScene::Impl.
    Private/GpuScene/BcEncoder.h
    Private/GpuScene/BcEncoder.cpp
    # Mesh UV tight-packing helper. Split out of Commit.cpp so the unit
    # test (MeshUvPackV2) can pin the gMeshUvs 8-byte stride contract
    # directly without dragging in GpuScene::Impl.
    Private/GpuScene/MeshUvPack.h
    Private/GpuScene/MeshUvPack.cpp
    # V2.A.14 — single-TU home for stb_dxt's STB_DXT_IMPLEMENTATION.
    # SKIP_LINTING below (mirrors pyxis_app/Private/StbImageWrite.cpp).
    Private/GpuScene/StbDxt.cpp

    # M1 — RenderGraph + Passes + Profiler + Renderer.
    Private/Diagnostics/CommandListMarker.h

    Private/RenderGraph/IRenderPass.h
    Private/RenderGraph/PassContext.h
    Private/RenderGraph/RenderGraph.h
    Private/RenderGraph/RenderGraph.cpp

    Private/Passes/SceneBindings.h
    Private/Passes/SceneBindings.cpp
    # DLSS Stage 2a — the Halton jitter sequence shared by SceneBindings
    # (uploads it for the raygen) and DlssPass (feeds it to Streamline).
    Private/Passes/CameraJitter.h
    # RTX-alignment design (rtx-realtime-alignment-design.md), "unexposed
    # intensity" clamp fix — the shared exposure-scale formula
    # TonemapPass::Execute and SceneBindings::Update both consume (header-
    # only, no .cpp: two small pure inline functions).
    Private/Passes/ExposureMath.h
    Private/Passes/RaytracedGBufferPass.h
    Private/Passes/RaytracedGBufferPass.cpp
    # RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
    # the five Phase A signal passes, registered between RaytracedGBuffer
    # and CompositePass (PyxisRenderer.cpp).
    Private/Passes/DirectLightingPass.h
    Private/Passes/DirectLightingPass.cpp
    Private/Passes/IndirectDiffusePass.h
    Private/Passes/IndirectDiffusePass.cpp

    # RTX-alignment (rtx-realtime-alignment-design.md, 2026-07-07) — SHARC
    # world-space radiance cache Resolve pass + owner of the cache buffers.
    Private/Passes/SharcResolvePass.h
    Private/Passes/SharcResolvePass.cpp
    Private/Passes/AmbientOcclusionPass.h
    Private/Passes/AmbientOcclusionPass.cpp
    Private/Passes/ReflectionsPass.h
    Private/Passes/ReflectionsPass.cpp
    Private/Passes/TranslucencyPass.h
    Private/Passes/TranslucencyPass.cpp
    # RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
    # the 4-pass denoiser chain, registered between TranslucencyPass and
    # CompositePass (PyxisRenderer.cpp). DenoiserResources is the small
    # cross-frame ping-pong history helper DenoiseTemporalPass owns.
    Private/Passes/DenoiserResources.h
    Private/Passes/DenoiserResources.cpp
    Private/Passes/DenoiseShadowPass.h
    Private/Passes/DenoiseShadowPass.cpp
    Private/Passes/DenoiseTemporalPass.h
    Private/Passes/DenoiseTemporalPass.cpp
    Private/Passes/DenoiseHistoryFixPass.h
    Private/Passes/DenoiseHistoryFixPass.cpp
    Private/Passes/DenoiseAtrousPass.h
    Private/Passes/DenoiseAtrousPass.cpp
    # Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
    # 2026-07-06), work item 1 — AO denoiser, runs alongside the 4-pass
    # chain above (own ping-pong, not DenoiserResources; see its file
    # header).
    Private/Passes/DenoiseAoPass.h
    Private/Passes/DenoiseAoPass.cpp
    # RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
    # CompositePass replaces the retired RaytracedLightingPass megakernel,
    # recombining the five signal passes' outputs into the final radiance.
    Private/Passes/CompositePass.h
    Private/Passes/CompositePass.cpp
    # RTX-alignment design (rtx-realtime-alignment-design.md), "KEY FINDING
    # (2026-07-06): no true accumulation buffer" — AccumulationPass,
    # registered between CompositePass and DlssPass (PyxisRenderer.cpp): a
    # true progressive running-mean average of the raw composite radiance,
    # replacing the denoiser's capped EMA as the convergence mechanism at
    # high accumulationFrames.
    Private/Passes/AccumulationPass.h
    Private/Passes/AccumulationPass.cpp
    Private/Passes/TonemapPass.h
    Private/Passes/TonemapPass.cpp
    Private/Passes/AutoExposurePass.h
    Private/Passes/AutoExposurePass.cpp
    # RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
    # TaaPass, registered between AutoExposurePass and TonemapPass
    # (PyxisRenderer.cpp).
    Private/Passes/TaaPass.h
    Private/Passes/TaaPass.cpp
    Private/Passes/SsaaResolvePass.h
    Private/Passes/SsaaResolvePass.cpp
    Private/Passes/BlitToSrgbPass.h
    Private/Passes/BlitToSrgbPass.cpp

    Private/Profiler/Profiler.cpp

    # DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
    # stance" + "DLSS scope includes upscaling") — capability-probe
    # scaffold (DLL discovery + Streamline symbol resolution). Stage 2a
    # adds real device interop (Initialize/slSetVulkanInfo/
    # slIsFeatureSupported, DlssProvider.cpp) and the per-frame evaluate
    # path (GetOptimalRenderResolution/Evaluate/ReleaseResources,
    # DlssProviderFrame.cpp — split out per the "long methods split
    # per-phase" convention once Stage 2a grew the class past one file).
    # Owned by PyxisRenderer.
    Private/Dlss/DlssProvider.h
    Private/Dlss/DlssProviderImpl.h
    Private/Dlss/DlssProvider.cpp
    Private/Dlss/DlssProviderFrame.cpp
    # DLSS Stage 2a — the render-graph pass evaluating DLSS Super
    # Resolution (renderRes composite -> displayRes upscale), registered
    # between CompositePass and AutoExposurePass (PyxisRenderer.cpp).
    Private/Passes/DlssPass.h
    Private/Passes/DlssPass.cpp

    Private/Renderers/PyxisRenderer.cpp

    # M5 — stb_image IMPLEMENTATION TU. The renderer's TextureCache
    # decodes 8/16-bit PNG/JPG/etc via stb_image; the IMPLEMENTATION
    # macro must land in EXACTLY one TU per binary. Same pattern
    # pyxis_app uses for stb_image_write (its --screenshot path).
    # Single-source-of-truth lives at Private/Materials/StbImageRead.cpp.
    Private/Materials/StbImageRead.cpp
    Private/Materials/MaterialFlag.h

    # Scene/ — the §8 Flecs scene primitives GpuScene builds on (RFC 0009).
    # The §30.11 phase pipeline (Phases.*), the light handle table
    # (HandleBimap.*), and the Dirty<T> tag components are owned + driven by
    # GpuScene's internal SceneWorld; the former parallel app-side facade
    # (World/SceneWorldFacade), its no-op System_* stubs, the QueryCache stub,
    # and the unused component mirrors were retired in RFC 0009's follow-up.
    Private/Scene/Phases.h
    Private/Scene/Phases.cpp
    Private/Scene/HandleBimap.h
    Private/Scene/HandleBimap.cpp
    Private/Scene/Components/Dirty.h
)

# ----------------------------------------------------------------------------
# Dependencies.
# ----------------------------------------------------------------------------
find_package(flecs CONFIG REQUIRED)

# hlslpp is header-only and lives in our public Descs (MeshDesc /
# InstanceDesc / CameraDesc use float3 / float4x4). Linked PUBLIC so
# consumers (pyxis_app, pyxis_unit_tests, future pyxis_hydra) see the
# include path.
pyxis_thirdparty_require_hlslpp()

# Flecs is PRIVATE — never propagated to dependents (plan §49). hlslpp
# is PUBLIC — it leaks through Public/Descs/* and consumers must see it.
# resources/shaders/ is on the private include path so C++ TUs can
# `#include "ShaderInterop.slang"` and pick up the shared structs
# (CameraUniforms, HitInfo, ...) directly. The Slang file branches on
# __cplusplus, so the same header satisfies both the slangc shader
# build and the renderer's C++ side without a hand-maintained C++
# shadow.
target_include_directories(pyxis_renderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/resources/shaders
)

# DLSS Stage 2a (rtx-realtime-alignment-design.md) — vendored Streamline
# MIT headers (thirdparty/streamline/, LICENSE.txt alongside). Header-only
# / GetProcAddress-based (Private/Dlss/DlssProvider.cpp) — no import lib,
# no link dependency; builds identically whether or not the proprietary
# DLLs (_local/dlss/, never committed) are staged. Mirrors the identical
# include added to pyxis_platform's CMakeLists.txt (Device/
# DlssDeviceExtensions.cpp, the pre-device-creation half of this hookup).
target_include_directories(pyxis_renderer PRIVATE
    ${CMAKE_SOURCE_DIR}/thirdparty/streamline/include
)

# stb_image is header-only; vcpkg's `Stb` config exports the include
# dir as Stb_INCLUDE_DIR (no link target). pyxis_app already pulls
# this for stb_image_write; pyxis_renderer pulls it independently
# for stb_image (decode side).
find_package(Stb REQUIRED)
target_include_directories(pyxis_renderer PRIVATE ${Stb_INCLUDE_DIR})

# tinyexr — HDR / EXR decode for M7 dome environment maps. pyxis_app
# already links it for EXR output; the renderer pulls it independently
# for the input / decode side.
find_package(tinyexr CONFIG REQUIRED)

# libtiff — TIFF decode (LZW/packbits/none via the lean default-features-off
# port). stb_image has no TIFF support; production USD + the OpenPBR Playground
# ship LZW 8-bit TIFF maps. TextureCache (Commit.cpp) calls TIFFReadRGBAImage.
find_package(TIFF REQUIRED)

# OpenMP — fans the per-texture decode + gamma-aware mip build + BCn
# encode across the pool in GpuScene::Impl::UploadPendingTextures
# (Commit.cpp). That pass dominated first-frame load on the World Lobby
# (~16 s of decode alone over 142 textures).
#
# Use plain `/openmp`, NOT `/openmp:llvm`. On clang-cl the OpenMP runtime
# is *always* libomp (the LLVM 4.5+ runtime) regardless of suffix — the
# `:llvm` / `:experimental` selectors are cl.exe-isms. Worse, clang-cl's
# `/openmp:llvm` is a recognised-but-quirky form that does NOT define the
# `_OPENMP` feature macro, so any `#ifdef _OPENMP`-guarded pragma compiles
# out and the loop runs serially. `/openmp` defines `_OPENMP` and links
# libomp — exactly what we want.
# Compile-only: clang-cl's `/openmp` embeds a `/defaultlib:libomp`
# directive in every object (passing `/openmp` to lld-link directly fails —
# it is not a linker flag). lld-link still needs LLVM's lib dir on the
# search path to resolve that directive, and the matching libomp.dll must
# sit next to the binary at runtime. Both are derived from CMAKE_LINKER
# (`.../LLVM/bin/lld-link.exe`).
target_compile_options(pyxis_renderer PRIVATE /openmp)
get_filename_component(_pyxis_llvm_bin  "${CMAKE_LINKER}"     DIRECTORY)
get_filename_component(_pyxis_llvm_root "${_pyxis_llvm_bin}"  DIRECTORY)
target_link_libraries(pyxis_renderer PRIVATE "${_pyxis_llvm_root}/lib/libomp.lib")
add_custom_command(TARGET pyxis_renderer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_llvm_bin}/libomp.dll" "$<TARGET_FILE_DIR:pyxis_renderer>"
    COMMENT "Copying libomp.dll (OpenMP runtime) next to pyxis_renderer"
    VERBATIM)

# Tracy — optional CPU/GPU profiling backend. OFF by default (plan
# §34.2: "Tracy (optional, off by default)"); enable per-build via
# `cmake -DPYXIS_TRACY=ON`. When ON, the renderer links against the
# vcpkg-built Tracy::TracyClient SHARED library and the Profiler's
# RAII scopes feed Tracy zones in addition to the in-process ring.
# When OFF, the Tracy include is unreachable and the zone macros
# compile to nothing (TRACY_ENABLE undefined) — zero overhead.
option(PYXIS_TRACY "Enable the Tracy profiler client (CPU + GPU zones)." OFF)
if(PYXIS_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_compile_definitions(pyxis_renderer PRIVATE PYXIS_TRACY=1 TRACY_ENABLE=1)
    target_link_libraries(pyxis_renderer PRIVATE Tracy::TracyClient)
    message(STATUS "Pyxis: Tracy profiler enabled (Tracy::TracyClient linked).")
endif()

# stb_image's IMPLEMENTATION TU has missing-field-initializer
# patterns inside the third-party header (same as stb_image_write
# in pyxis_app). Disable the warning + clang-tidy on that single
# file rather than relax it project-wide.
set_source_files_properties(Private/Materials/StbImageRead.cpp PROPERTIES
    COMPILE_OPTIONS    "-Wno-missing-field-initializers"
    SKIP_LINTING       ON)

# V2.A.14 — stb_dxt's STB_DXT_IMPLEMENTATION sentinel is a
# third-party macro name (not the §30.2 PYXIS_SCREAM convention)
# that clang-tidy's readability-identifier-naming check trips on
# even with NOLINTNEXTLINE. SKIP_LINTING on the dedicated single-TU
# home keeps the check enabled everywhere else.
set_source_files_properties(Private/GpuScene/StbDxt.cpp PROPERTIES
    SKIP_LINTING ON)

# DLSS Stage 2a — the vendored sl_helpers_vk.h (thirdparty/streamline/,
# NVIDIA's own MIT header) initializes VkPhysicalDeviceVulkan12/13Features
# with a single-field braced-init (`{ VK_STRUCTURE_TYPE_... }`), which
# trips -Wmissing-field-initializers under /W4. Scoped to the two TUs that
# pull it in via DlssProviderImpl.h (not project-wide) — same "third-party
# code, not ours to fix" precedent as StbImageRead.cpp above.
set_source_files_properties(
    Private/Dlss/DlssProvider.cpp
    Private/Dlss/DlssProviderFrame.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers"
)

target_link_libraries(pyxis_renderer
    PUBLIC
        Pyxis::Platform
        pyxis_hlslpp
    PRIVATE
        $<TARGET_NAME_IF_EXISTS:flecs::flecs_static>
        $<TARGET_NAME_IF_EXISTS:flecs::flecs>
        unofficial::tinyexr::tinyexr
        TIFF::TIFF
)

# Debug-only: PYXIS_DEBUG_TOOLS toggles Flecs Explorer (§30.11).
target_compile_definitions(pyxis_renderer PRIVATE
    $<$<BOOL:${PYXIS_DEBUG_TOOLS}>:PYXIS_DEBUG_TOOLS=1>
)

# OPTIONAL NRD denoiser backend (RTX-alignment 2026-07-10, default OFF —
# proprietary NVIDIA RTX SDKs license; fetched into the build tree only,
# never vendored/installed. See _cmake/Thirdparty.cmake's PYXIS_WITH_NRD
# block for the licensing rationale). When ON, the NRD static lib + headers
# become available and PYXIS_WITH_NRD=1 is defined; runtime integration is
# staged behind that define (an absent define compiles the builtin chain
# byte-identically).
if(PYXIS_WITH_NRD)
    pyxis_thirdparty_require_nrd()
    target_link_libraries(pyxis_renderer PRIVATE NRD)
    target_compile_definitions(pyxis_renderer PRIVATE PYXIS_WITH_NRD=1)

    # NrdProvider — creates the nrd::Instance and translates its pipeline/
    # resource contract onto NVRHI (compute shaders, binding layouts,
    # permanent/transient pool textures) + the Stage 2 per-frame Evaluate()
    # path. NrdDenoisePass (Stage 3a) is the thin IRenderPass driving that
    # Evaluate(). All four files need <NRD.h>/<NRDDescs.h>/<NRDSettings.h>,
    # only available once pyxis_thirdparty_require_nrd() above has fetched
    # them, so they are only ever added to the target's source list here —
    # never compiled when PYXIS_WITH_NRD=OFF. See Private/Nrd/NrdProvider.h's
    # file comment for the staging plan (graph registration is the follow-up).
    target_sources(pyxis_renderer PRIVATE
        Private/Nrd/NrdProvider.h
        Private/Nrd/NrdProvider.cpp
        Private/Passes/NrdDenoisePass.h
        Private/Passes/NrdDenoisePass.cpp
    )
endif()

# Versioning (§22.2): pyxis_renderer.dll is the canonical owner of the
# generated Version.h / Version.cpp. Version.h is PYXIS_RENDERER_API
# decorated, so it lives here, not in pyxis_platform.
include(Version)
pyxis_generate_version_header(pyxis_renderer)
pyxis_generate_version_source(pyxis_renderer)

# Shaders — the two RT pipelines (P4 pass split) compiled by Slang at
# build time (plan §10 / §23). The .spv lands in
# <bin>/Resources/shaders/ and is loaded by RaytracedGBufferPass /
# RaytracedLightingPass at startup. Shader files are named after the
# pass that owns them; shared modules (camera_ray / dome_sample /
# shading / raytraced_anyhit) are NOT compiled standalone — they are
# #included by the entry files and listed under INCLUDES.
include(Slang)
# Every shader lists EVERY .slang module it (transitively) #includes
# under INCLUDES so a module edit recompiles the dependents (Slang.cmake
# appends INCLUDES to the custom command's DEPENDS — P2 finding).
set(PYXIS_SHADER_INTEROP    ${CMAKE_SOURCE_DIR}/resources/shaders/ShaderInterop.slang)
set(PYXIS_SHADER_CAMERA_RAY ${CMAKE_SOURCE_DIR}/resources/shaders/camera_ray.slang)
set(PYXIS_SHADER_DOME       ${CMAKE_SOURCE_DIR}/resources/shaders/dome_sample.slang)
set(PYXIS_SHADER_SHADING    ${CMAKE_SOURCE_DIR}/resources/shaders/shading.slang)
# Q2 (openpbr-complete-design.md) — the OpenPBR closure module included by
# shading.slang; listed on every entry whose include chain reaches it so a
# closure edit recompiles the lighting raygen + closesthit.
set(PYXIS_SHADER_OPENPBR    ${CMAKE_SOURCE_DIR}/resources/shaders/openpbr_material.slang)
set(PYXIS_SHADER_ANYHIT     ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_anyhit.slang)
# RTX-alignment design (rtx-realtime-alignment-design.md), Phase B — the
# shared PCG32 RNG + cosine-hemisphere sampling module. Listed under
# INCLUDES on every entry whose source #includes it (direct_lighting /
# indirect_diffuse / ambient_occlusion / reflections — every pass with a
# stochastic estimator); NOT compiled standalone, same convention as the
# other shared modules above.
set(PYXIS_SHADER_RNG        ${CMAKE_SOURCE_DIR}/resources/shaders/rng.slang)
# RTX-alignment (rtx-realtime-alignment-design.md, 2026-07-07) — the SHARC
# world-space radiance-cache module (clean-room). #included by indirect_diffuse
# (query+update) and sharc_resolve (compute); NOT compiled standalone.
set(PYXIS_SHADER_RADIANCE_CACHE ${CMAKE_SOURCE_DIR}/resources/shaders/radiance_cache.slang)

# --- RT pipeline A: RaytracedGBufferPass (extended G-buffer, RTX-alignment
# WP2-core rewrite) — ONE shader file carrying all four entry points
# (rtx-realtime-alignment-design.md "Full shader-rewrite mandate": one
# pass = one shader file, all entries inside). slangc is invoked once per
# entry point (`-entry <name>`) against the SAME source; each invocation
# only emits the SPIR-V reachable from that entry, so the four .spv stay
# as small/thin as the pre-WP2 per-stage files. The raygen also reuses
# EvaluateSurfaceMaterial (shading.slang) for its primary-hit material
# eval, so this pass pulls the full scene-table include chain (camera_ray
# + dome_sample + raytraced_anyhit + openpbr_material via shading.slang)
# exactly like RaytracedLightingPass's raygen does below.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_gbuffer.slang
    ENTRY_POINT RayGenMain
    STAGE       raygen
    OUTPUT      raytraced_gbuffer_raygen.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_gbuffer.slang
    ENTRY_POINT ClosestHitMain
    STAGE       closesthit
    OUTPUT      raytraced_gbuffer_closesthit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_gbuffer.slang
    ENTRY_POINT MissMain
    STAGE       miss
    OUTPUT      raytraced_gbuffer_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_gbuffer.slang
    ENTRY_POINT AnyHitMain
    STAGE       anyhit
    OUTPUT      raytraced_gbuffer_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
)

# --- RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals
# — the five Phase A signal passes, ONE shader file each carrying every
# entry point (the "Full shader-rewrite mandate"). Registered between the
# RaytracedGBuffer and RaytracedLighting pipelines below.

# DirectLightingPass — RayGenMain, ShadowMissMain, AnyHitMain. Pulls the
# full scene-table chain (shading.slang) for EvaluateSurfaceMaterial +
# EvaluateOpenPBRDirectSplit, plus rng.slang (Phase B RIS estimator).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/direct_lighting.slang
    ENTRY_POINT RayGenMain
    STAGE       raygen
    OUTPUT      direct_lighting_raygen.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/direct_lighting.slang
    ENTRY_POINT ShadowMissMain
    STAGE       miss
    OUTPUT      direct_lighting_shadow_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/direct_lighting.slang
    ENTRY_POINT AnyHitMain
    STAGE       anyhit
    OUTPUT      direct_lighting_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)

# IndirectDiffusePass — RayGenMain, ClosestHitMain, MissMain,
# ShadowMissMain, AnyHitMain (Phase B: real path-traced bounce — grew from
# the Phase A RayGen-only pipeline).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/indirect_diffuse.slang
    ENTRY_POINT RayGenMain
    STAGE       raygen
    OUTPUT      indirect_diffuse_raygen.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/indirect_diffuse.slang
    ENTRY_POINT ClosestHitMain
    STAGE       closesthit
    OUTPUT      indirect_diffuse_closesthit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/indirect_diffuse.slang
    ENTRY_POINT MissMain
    STAGE       miss
    OUTPUT      indirect_diffuse_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/indirect_diffuse.slang
    ENTRY_POINT ShadowMissMain
    STAGE       miss
    OUTPUT      indirect_diffuse_shadow_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/indirect_diffuse.slang
    ENTRY_POINT AnyHitMain
    STAGE       anyhit
    OUTPUT      indirect_diffuse_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)

# AmbientOcclusionPass — RayGenMain, MissMain, AnyHitMain. Deliberately
# independent of shading.slang / openpbr_material.slang (RTAO needs only
# geometry) — camera_ray + the shared alpha-test anyhit module + rng.slang
# (Phase B stochastic cosine-hemisphere occlusion).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/ambient_occlusion.slang
    ENTRY_POINT RayGenMain
    STAGE       raygen
    OUTPUT      ambient_occlusion_raygen.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/ambient_occlusion.slang
    ENTRY_POINT MissMain
    STAGE       miss
    OUTPUT      ambient_occlusion_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/ambient_occlusion.slang
    ENTRY_POINT AnyHitMain
    STAGE       anyhit
    OUTPUT      ambient_occlusion_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)

# ReflectionsPass — RayGenMain, ClosestHitMain, MissMain (primary
# reflection ray) + AoAnyHitMain, AoMissMain (occlusion-aware-ambient
# follow-up: the short AO ray ClosestHitMain fires at the reflection hit,
# rtx-realtime-alignment-design.md 2026-07-06 — see reflections.slang's
# file header for the two-hit-group / two-miss-shader SBT layout).
# rng.slang added (Phase B GGX VNDF importance-sampled reflection
# direction, and now the AO ray's cosine-hemisphere sample).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/reflections.slang
    ENTRY_POINT RayGenMain
    STAGE       raygen
    OUTPUT      reflections_raygen.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/reflections.slang
    ENTRY_POINT ClosestHitMain
    STAGE       closesthit
    OUTPUT      reflections_closesthit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/reflections.slang
    ENTRY_POINT MissMain
    STAGE       miss
    OUTPUT      reflections_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/reflections.slang
    ENTRY_POINT AnyHitMain
    STAGE       anyhit
    OUTPUT      reflections_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/reflections.slang
    ENTRY_POINT AoAnyHitMain
    STAGE       anyhit
    OUTPUT      reflections_ao_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/reflections.slang
    ENTRY_POINT AoMissMain
    STAGE       miss
    OUTPUT      reflections_ao_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RNG} ${PYXIS_SHADER_RADIANCE_CACHE}
)

# TranslucencyPass — RayGenMain, ClosestHitMain, MissMain, AnyHitMain
# (reuses the existing megakernel machinery verbatim — see the pass's own
# file headers).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/translucency.slang
    ENTRY_POINT RayGenMain
    STAGE       raygen
    OUTPUT      translucency_raygen.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/translucency.slang
    ENTRY_POINT ClosestHitMain
    STAGE       closesthit
    OUTPUT      translucency_closesthit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/translucency.slang
    ENTRY_POINT MissMain
    STAGE       miss
    OUTPUT      translucency_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RADIANCE_CACHE}
)
# ShadowMissMain — required by the reused ShadeSurfaceHit's internal
# shadow/AO rays (hardcoded MissShaderIndex = 1); see the entry's own
# doc comment in translucency.slang for how this was discovered
# (an OOB shader-table read crashed the GPU without it).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/translucency.slang
    ENTRY_POINT ShadowMissMain
    STAGE       miss
    OUTPUT      translucency_shadow_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RADIANCE_CACHE}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/translucency.slang
    ENTRY_POINT AnyHitMain
    STAGE       anyhit
    OUTPUT      translucency_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
                ${PYXIS_SHADER_RADIANCE_CACHE}
)

# --- RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
# the RaytracedLightingPass megakernel is RETIRED (its raygen + miss are
# deleted); CompositePass (below, a compute pass) replaces it as the
# linearColor / colorHdr / alpha writer. raytraced_lighting_closesthit.slang
# + raytraced_lighting_shadow_miss.slang + raytraced_lighting_anyhit.slang
# are KEPT (compiled here, unchanged) per the owner's WP2-final directive —
# they are the reference megakernel machinery TranslucencyPass's own
# ClosestHitMain/ShadowMissMain/AnyHitMain (translucency.slang) were ported
# from; nothing in the current graph loads their .spv at runtime, so this
# block is intentionally orphaned build output, not dead source.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_lighting_closesthit.slang
    ENTRY_POINT main
    STAGE       closesthit
    OUTPUT      raytraced_lighting_closesthit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
                ${PYXIS_SHADER_SHADING} ${PYXIS_SHADER_OPENPBR} ${PYXIS_SHADER_ANYHIT}
)
# M9-fidelity shadow rays — second miss shader stitched into the
# shader binding table at miss-index 1. The shared shading traces a
# shadow ray per direct light with RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
# RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH; this miss writes
# payload.color=1.0 (=visible) iff no opaque hit blocks the path.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_lighting_shadow_miss.slang
    ENTRY_POINT main
    STAGE       miss
    OUTPUT      raytraced_lighting_shadow_miss.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raytraced_lighting_anyhit.slang
    ENTRY_POINT main
    STAGE       anyhit
    OUTPUT      raytraced_lighting_anyhit.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_ANYHIT}
)
# CompositePass — ONE compute entry, recombines the five signal passes'
# outputs into the final linear radiance (WP2-final). Needs camera_ray
# (BuildCameraRay for the miss-pixel dome background) + dome_sample
# (SampleDomeBackground) but NOT shading/openpbr/anyhit — it does no
# material re-evaluation or TraceRay of its own.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/composite.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      composite.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP} ${PYXIS_SHADER_CAMERA_RAY} ${PYXIS_SHADER_DOME}
)
# Display transform (compute) — P3 pass split. The 11-way debug-view
# branch (exposure + Narkowicz ACES on COLOR) extracted from raygen;
# reads the fp32 linearColor scratch + the raw AOVs, writes the BGRA8
# display target. Runs every frame, before the SSAA resolve.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/tonemap.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      tonemap.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# Auto-exposure log-luminance reduction (compute). Optional pass between
# lighting and tonemap: reduces the fp32 linearColor to a geometric-mean
# luminance (legacy mode, deterministic fixed-point integer atomics) or a
# 64-bucket log-luminance histogram (histogram mode, RTX-alignment design
# Phase C) into a stats buffer the tonemap reads. No-op when
# RenderSettings::autoExposure is off.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/auto_exposure.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      auto_exposure.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
# histogram auto-exposure's second dispatch: single-thread reduction of the
# 64-bucket histogram `main` (above) built, to one median log2(luminance).
# Same source file (one pass = one shader file, per the WP2 "Full
# shader-rewrite mandate"), a second entry point / .spv.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/auto_exposure.slang
    ENTRY_POINT ResolveHistogram
    STAGE       compute
    OUTPUT      auto_exposure_resolve.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)

# --- RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
# the 4-pass denoiser chain (SIGMA-class shadow filter + ReLAX-class
# temporal/history-fix/à-trous) and TaaPass. All screen-space COMPUTE
# passes (ONE shader file / ONE compute entry each, dispatched multiple
# times per frame by their owning C++ pass where the frozen spec calls for
# it) — no SceneBindings, so only ShaderInterop.slang is needed.

# DenoiseShadowPass — dispatched twice per frame (narrow then wide radius)
# against the SAME compiled shader; see DenoiseShadowPass::Execute.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/denoise_shadow.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      denoise_shadow.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# DenoiseTemporalPass — ReLAX-class reprojection + accumulation.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/denoise_temporal.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      denoise_temporal.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# DenoiseHistoryFixPass — widens spatial support for low-history pixels.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/denoise_history_fix.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      denoise_history_fix.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# DenoiseAtrousPass — dispatched 5 times per frame (ping-pong, dilating
# step size) against the SAME compiled shader; see
# DenoiseAtrousPass::Execute.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/denoise_atrous.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      denoise_atrous.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# SharcResolvePass — SHARC radiance-cache Resolve (RTX-alignment design,
# 2026-07-07). Folds each cell's per-frame radiance sum into a cross-frame
# running mean. Compute; includes radiance_cache.slang (the shared cache module,
# NOT compiled standalone).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/sharc_resolve.slang
    ENTRY_POINT ResolveMain
    STAGE       compute
    OUTPUT      sharc_resolve.spv
    INCLUDES    ${PYXIS_SHADER_RADIANCE_CACHE}
)
# TaaPass — jittered-history reprojection + neighborhood clamp, runs
# before TonemapPass on the linear pre-tonemap color.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/taa.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      taa.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
# 2026-07-06), work item 1 — DenoiseAoPass: temporal accumulation + small
# edge-aware spatial pre-filter for AmbientOcclusionPass's raw gAo, folded
# into ONE dispatch (one pass = one shader).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/denoise_ao.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      denoise_ao.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)

# RTX-alignment design (rtx-realtime-alignment-design.md), "KEY FINDING
# (2026-07-06): no true accumulation buffer" — AccumulationPass: a true
# progressive running-mean average of CompositePass's raw linear radiance,
# registered between CompositePass and DlssPass (PyxisRenderer.cpp).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/accumulate.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      accumulate.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)

# DLSS Stage 2a (rtx-realtime-alignment-design.md) — DlssPass's own tiny
# depth-conversion dispatch: linear view-space gViewZ -> the standard
# normalized-device depth Streamline's DLSS-SR plugin actually requires
# (kBufferTypeDepth). See that shader's own file comment for the full
# citation/formula.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/dlss_depth_convert.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      dlss_depth_convert.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)

# Viewer SSAA box-downsample resolve (compute). Reads the super-res
# color AOV, writes the backbuffer-resolution resolve. Used by
# ViewerMode when SSAA > 1 (ImGui toggle / factor). Renamed from
# ssaa_downsample at P3 (shader files are named after their pass).
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/ssaa_resolve.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      ssaa_resolve.spv
    INCLUDES    ${PYXIS_SHADER_INTEROP}
)
# Viewer present blit (compute): linear -> sRGB OETF into the present target.
# Split out of SsaaResolvePass so each pass has one job; runs every viewer frame.
# (Declares its cbuffer locally — no ShaderInterop include.)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/blit_to_srgb.slang
    ENTRY_POINT main
    STAGE       compute
    OUTPUT      blit_to_srgb.spv
)

# OPTIONAL NRD denoiser backend, Stage 2 (RTX-alignment 2026-07-10) —
# NrdProvider's own G-buffer -> NRD-input-encoding pack pre-pass
# (nrd_pack.slang's PackMain, dispatched by NrdProvider::DispatchPack
# immediately before the RELAX_DIFFUSE_SPECULAR dispatch chain). Compiled
# only when PYXIS_WITH_NRD=ON, matching NrdProvider.h/.cpp's own
# target_sources gating above — both are absent from the target entirely
# when the option is OFF.
if(PYXIS_WITH_NRD)
    pyxis_compile_slang_shader(
        TARGET      pyxis_renderer
        SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/nrd_pack.slang
        ENTRY_POINT PackMain
        STAGE       compute
        OUTPUT      nrd_pack.spv
        INCLUDES    ${PYXIS_SHADER_INTEROP}
    )
endif()
