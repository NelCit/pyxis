// Pyxis renderer — public OpenPBR material descriptor.
//
// Plan §11 + §18.4. The single canonical material POD. All ingest
// adapters (UsdPreviewSurface, MaterialX `open_pbr_surface`,
// RenderMan fallback) translate to this shape; one generic
// closesthit shader consumes it (branchless on `MaterialFlag` bits
// once §11 lands at M5+).
//
// Note that the M3 path-trace box renders the cube against a
// hardcoded grey material with no texture maps; the texture-handle
// fields are reserved here as part of the byte-stable public surface
// (§22.3) so M5+ can populate them without an ABI break.
//
// `sourcePrim` is diagnostics-only: the renderer copies the first 63
// bytes into an internal POD before queueing the mutation across
// threads (§18.5 / §31), so the view never outlives the caller's
// stack. Never hashed — only used in error messages and the AOV
// inspector.

#pragma once

#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <hlsl++.h>

#include <cstdint>
#include <string_view>

namespace pyxis {

struct OpenPBRMaterialDesc {
    enum class Source : uint8_t {
        UsdPreviewSurface,
        MaterialX,
        RenderManFallback,
        Default,
    };

    // Base layer — see §11 OpenPBR §3.1.
    hlslpp::float3 baseColor   = { 0.8f, 0.8f, 0.8f };
    float          baseWeight  = 1.0f;
    float          metalness   = 0.0f;
    float          roughness   = 0.5f;

    // Reserved per §11 (specular / transmission / coat / emission /
    // geometry blocks). M5+ populates these alongside the OpenPBR
    // shader; v1 closesthit only consumes the base layer above.
    float          specularWeight     = 1.0f;
    float          specularIor        = 1.5f;
    float          transmissionWeight = 0.0f;
    float          coatWeight         = 0.0f;
    float          coatRoughness      = 0.0f;
    hlslpp::float3 emissionColor      = { 0.0f, 0.0f, 0.0f };
    float          emissionLuminance  = 0.0f;
    float          opacity            = 1.0f;

    // Texture bindings — opaque handles obtained via
    // GpuScene::AcquireTexture. Invalid means "no texture; use the
    // scalar value above for this lobe."
    TextureHandle baseColorMap     = TextureHandle::Invalid;
    TextureHandle metallicMap      = TextureHandle::Invalid;
    TextureHandle roughnessMap     = TextureHandle::Invalid;
    TextureHandle normalMap        = TextureHandle::Invalid;
    TextureHandle emissionMap      = TextureHandle::Invalid;
    TextureHandle opacityMap       = TextureHandle::Invalid;
    TextureHandle transmissionMap  = TextureHandle::Invalid;
    TextureHandle coatRoughnessMap = TextureHandle::Invalid;

    Source           source = Source::Default;
    std::string_view sourcePrim;   // diagnostics only; not hashed.
};

}  // namespace pyxis
