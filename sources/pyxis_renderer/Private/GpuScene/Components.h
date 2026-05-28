// Pyxis renderer — GpuScene Flecs component POD structs.
//
// Split out of Internal.h (RFC 0009 follow-up). One entity per logical scene
// object; these are the POD components (§30.11) the GpuScene-owned SceneWorld
// stores. Non-POD / variable-length per-entity data lives in the slot-indexed
// side tables on GpuScene::Impl, referenced by handle. PRIVATE header — only
// files under Private/GpuScene/ may include it (§18.9).

#pragma once

#include <Pyxis/Renderer/GpuScene.h>  // LightDesc / OpenPBRMaterialDesc / TextureKey

#include <nvrhi/nvrhi.h>  // nvrhi::Format

#include <cstdint>

namespace pyxis {

// RFC 0009 P1 — per-light Flecs component. Stores the encoded handle (so the editor
// accessors return it directly) + the canonical LightDesc (no mirror struct: the
// existing PackLightGpu consumes it unchanged → byte-identical light buffer). The
// sort key for deterministic packing is HandleBimap::SlotIndex(handle).
struct GpuLightComponent
{
  uint32_t  handle = 0;
  LightDesc desc{};
};

// RFC 0009 P2 — per-material Flecs component. The OpenPBRMaterialDesc + its FNV1a
// hash. `desc.sourcePrim` is a string_view (POD — no owning container, so this is a
// valid §30.11 component); its backing owned string lives in the slot-indexed
// `materialSourcePrims` table on Impl (variable-length data → a GpuScene table).
struct GpuMaterialComponent
{
  OpenPBRMaterialDesc desc{};
  std::uint64_t       descHash = 0;
};

// RFC 0009 P3 — per-texture Flecs component (POD metadata only; §30.11). The GPU
// resource (nvrhi::TextureHandle), the transient decoded pixels, and the owned
// resolvedPath string are NOT POD, so they live in the slot-indexed side tables on
// Impl (textureResources / texturePixelData / textureResolvedPaths) — the §30.11
// "variable-length data in a GpuScene table, referenced by handle" rule. The decode
// writes the side tables (slot-indexed → thread-safe under the OpenMP decode); the
// component metadata (dims/format/bindlessSlot) is written serially. keyCopy's
// resolvedPath view points into textureResolvedPaths[slot].
struct GpuTextureComponent
{
  TextureKey     keyCopy{};
  std::uint64_t  keyHash        = 0;
  std::uint32_t  bindlessSlot   = 0;
  std::uint32_t  width          = 0;
  std::uint32_t  height         = 0;
  nvrhi::Format  format         = nvrhi::Format::UNKNOWN;
};

// RFC 0009 P4 — per-mesh Flecs component (POD identity). The heavy CPU/GPU mesh data
// (positions/indices/attributes/buffers/BLAS/counts) is non-POD or hot-path, so it
// lives in the slot-indexed `meshResources` side table; this component carries only
// the content hash so meshes are queryable by identity (and DestroyMesh can drop the
// dedup-map entry). The DirtyTopology tag marks an entity needing (re)upload + BLAS.
struct GpuMeshComponent
{
  std::uint64_t descHash = 0;
};

// RFC 0009 P5 — per-instance marker component (the data lives in the slot-indexed
// `instanceResources` side table — same thin-key shape as meshes/textures).
struct GpuInstanceComponent
{
};

// RFC 0009 P6 — per-volume marker component (data in the slot-indexed
// `volumeResources` side table; V2.A.5 stub — most volumes are detect-warn-skipped).
struct GpuVolumeComponent
{
};

// RFC 0009 P5 — Flecs relationships an instance entity carries: (Instance, MeshOf,
// meshEntity) and (Instance, MaterialOf, materialEntity). They make "which instances
// reference this mesh/material" a query — the basis for refcounted BLAS sharing +
// orphan detection (§30.11 "prefer pair relationships"). The TLAS/side-table build
// still resolves by handle (byte-identical); these pairs are additive metadata.
struct MeshOf
{
};
struct MaterialOf
{
};

}  // namespace pyxis
