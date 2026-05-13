// Pyxis USD ingest — OpenSubdiv refinement implementation.

#include "SubdivRefine.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/sdc/options.h>
#include <opensubdiv/sdc/types.h>

#include <pxr/usd/usdGeom/tokens.h>

#include <memory>
#include <mutex>
#include <vector>

namespace pyxis::usd_ingest {

namespace {

// OpenSubdiv's PrimvarRefiner is generic over a per-vertex value
// type. The contract: a default ctor + Clear() + AddWithWeight(src, w).
// Two specialisations: Position (Gf-style float3) + Uv (float2).
//
// Both types are header-only PODs sized to match their channel count;
// no dynamic dispatch (the refiner template-expands once per type).

struct PosVtx {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  void Clear() noexcept { x = 0.0f; y = 0.0f; z = 0.0f; }
  void AddWithWeight(PosVtx const& src, float weight) noexcept {
    x += src.x * weight;
    y += src.y * weight;
    z += src.z * weight;
  }
};

struct UvVtx {
  float u = 0.0f;
  float v = 0.0f;
  void Clear() noexcept { u = 0.0f; v = 0.0f; }
  void AddWithWeight(UvVtx const& src, float weight) noexcept {
    u += src.u * weight;
    v += src.v * weight;
  }
};

// Map UsdGeomMesh::interpolateBoundary token to OpenSubdiv's Sdc
// boundary-interpolation enum. USD's defaults:
//   "edgeAndCorner" → VTX_BOUNDARY_EDGE_AND_CORNER (default; OS too)
//   "edgeOnly"      → VTX_BOUNDARY_EDGE_ONLY
//   "none"          → VTX_BOUNDARY_NONE
OpenSubdiv::Sdc::Options::VtxBoundaryInterpolation
MapBoundaryInterp(const pxr::TfToken& token) noexcept {
  using Interp = OpenSubdiv::Sdc::Options::VtxBoundaryInterpolation;
  if (token == pxr::UsdGeomTokens->none) return Interp::VTX_BOUNDARY_NONE;
  if (token == pxr::UsdGeomTokens->edgeOnly) return Interp::VTX_BOUNDARY_EDGE_ONLY;
  return Interp::VTX_BOUNDARY_EDGE_AND_CORNER;  // default + edgeAndCorner
}

// Map UsdGeomMesh::faceVaryingLinearInterpolation token to Sdc's
// FVar enum. USD defaults to "cornersPlus1"; OpenSubdiv's default is
// FVAR_LINEAR_CORNERS_PLUS1.
OpenSubdiv::Sdc::Options::FVarLinearInterpolation
MapFVarInterp(const pxr::TfToken& token) noexcept {
  using Interp = OpenSubdiv::Sdc::Options::FVarLinearInterpolation;
  if (token == pxr::UsdGeomTokens->none) return Interp::FVAR_LINEAR_NONE;
  if (token == pxr::UsdGeomTokens->cornersOnly) return Interp::FVAR_LINEAR_CORNERS_ONLY;
  if (token == pxr::UsdGeomTokens->cornersPlus2) return Interp::FVAR_LINEAR_CORNERS_PLUS2;
  if (token == pxr::UsdGeomTokens->boundaries) return Interp::FVAR_LINEAR_BOUNDARIES;
  if (token == pxr::UsdGeomTokens->all) return Interp::FVAR_LINEAR_ALL;
  return Interp::FVAR_LINEAR_CORNERS_PLUS1;  // USD default
}

}  // namespace

SubdivRefinedMesh RefineSubdivMesh(
    const pxr::UsdGeomMesh& mesh,
    const pxr::VtArray<pxr::GfVec3f>& srcPoints,
    const pxr::VtArray<int>& srcCounts,
    const pxr::VtArray<int>& srcIndices,
    const pxr::VtArray<pxr::GfVec2f>& srcUvs,
    bool srcUvsAreFaceVarying,
    std::uint32_t maxLevel) noexcept {
  SubdivRefinedMesh out;
  out.refined = false;

  auto& log = Logging::Get();
  using namespace OpenSubdiv;

  // ---- Scheme detection ------------------------------------------------
  pxr::TfToken schemeToken;
  mesh.GetSubdivisionSchemeAttr().Get(&schemeToken);
  Sdc::SchemeType scheme;
  if (schemeToken == pxr::UsdGeomTokens->catmullClark)
    scheme = Sdc::SCHEME_CATMARK;
  else if (schemeToken == pxr::UsdGeomTokens->loop)
    scheme = Sdc::SCHEME_LOOP;
  else
    return out;  // schemeToken == "none" or "bilinear" (bilinear handled by v1's flat path)

  if (srcPoints.empty() || srcCounts.empty() || srcIndices.empty())
    return out;
  if (maxLevel == 0)
    return out;  // caller asked for no refinement — pass-through

  // ---- USD subdiv attribute reads --------------------------------------
  pxr::TfToken boundaryToken;
  mesh.GetInterpolateBoundaryAttr().Get(&boundaryToken);
  pxr::TfToken fvarToken;
  mesh.GetFaceVaryingLinearInterpolationAttr().Get(&fvarToken);

  Sdc::Options options;
  options.SetVtxBoundaryInterpolation(MapBoundaryInterp(boundaryToken));
  options.SetFVarLinearInterpolation(MapFVarInterp(fvarToken));

  // ---- USD → OpenSubdiv crease conversion ------------------------------
  // V2.A.1 follow-up: subdivision creases. UsdGeomMesh authors crease
  // edges + sharp corners as five attributes:
  //   * cornerIndices       int[]   — sharp vertex indices
  //   * cornerSharpnesses   float[] — one weight per corner
  //   * creaseIndices       int[]   — flat list of crease vertex chains
  //   * creaseLengths       int[]   — per-crease vertex count (>= 2)
  //   * creaseSharpnesses   float[] — either ONE weight per crease
  //                                   (uniform) or sum(creaseLengths-1)
  //                                   weights (per-segment).
  //
  // OpenSubdiv's TopologyDescriptor wants creases as flat vertex-INDEX
  // PAIRS — for a chain of L vertices that's 2*(L-1) indices and L-1
  // weights. Translate here so a chain `[v0,v1,v2]` with uniform
  // weight w becomes `[v0,v1, v1,v2]` paired with `[w,w]`.
  //
  // Without this, hard edges on production assets (mechanical parts,
  // architectural trim, character feature lines) refine into smoothly
  // rounded surfaces — visibly wrong vs the authoring intent.
  pxr::VtArray<int>   usdCornerIndices;
  pxr::VtArray<float> usdCornerSharpnesses;
  pxr::VtArray<int>   usdCreaseIndices;
  pxr::VtArray<int>   usdCreaseLengths;
  pxr::VtArray<float> usdCreaseSharpnesses;
  mesh.GetCornerIndicesAttr().Get(&usdCornerIndices);
  mesh.GetCornerSharpnessesAttr().Get(&usdCornerSharpnesses);
  mesh.GetCreaseIndicesAttr().Get(&usdCreaseIndices);
  mesh.GetCreaseLengthsAttr().Get(&usdCreaseLengths);
  mesh.GetCreaseSharpnessesAttr().Get(&usdCreaseSharpnesses);

  std::vector<int>   osCornerIndices;
  std::vector<float> osCornerWeights;
  if (usdCornerIndices.size() == usdCornerSharpnesses.size())
  {
    osCornerIndices.assign(usdCornerIndices.begin(), usdCornerIndices.end());
    osCornerWeights.assign(usdCornerSharpnesses.begin(), usdCornerSharpnesses.end());
  }
  else if (!usdCornerIndices.empty() || !usdCornerSharpnesses.empty())
  {
    log.Warn(log::APP, "SubdivRefine: corner-index/sharpness count mismatch on "
                            + mesh.GetPrim().GetPath().GetString()
                            + " (" + std::to_string(usdCornerIndices.size())
                            + " vs " + std::to_string(usdCornerSharpnesses.size())
                            + "); skipping corners.");
  }

  std::vector<int>   osCreasePairIndices;
  std::vector<float> osCreasePairWeights;
  if (!usdCreaseLengths.empty() && !usdCreaseIndices.empty())
  {
    std::size_t totalLengthSum = 0;
    std::size_t totalSegments  = 0;
    for (const int len : usdCreaseLengths)
    {
      if (len < 2)
      {
        log.Warn(log::APP, "SubdivRefine: crease-length " + std::to_string(len)
                                + " < 2 on " + mesh.GetPrim().GetPath().GetString()
                                + "; skipping creases.");
        osCreasePairIndices.clear();
        osCreasePairWeights.clear();
        totalLengthSum = 0;
        break;
      }
      totalLengthSum += static_cast<std::size_t>(len);
      totalSegments  += static_cast<std::size_t>(len - 1);
    }
    if (totalLengthSum != usdCreaseIndices.size() && totalLengthSum > 0)
    {
      log.Warn(log::APP, "SubdivRefine: crease-index count " + std::to_string(usdCreaseIndices.size())
                              + " != sum(creaseLengths) " + std::to_string(totalLengthSum)
                              + " on " + mesh.GetPrim().GetPath().GetString()
                              + "; skipping creases.");
      totalSegments = 0;
    }
    if (totalSegments > 0)
    {
      // USD authoring allows either one weight per CREASE or one
      // weight per SEGMENT. Detect by sharpness count.
      const bool perSegment = (usdCreaseSharpnesses.size() == totalSegments);
      const bool perCrease  = (usdCreaseSharpnesses.size() == usdCreaseLengths.size());
      if (!perSegment && !perCrease)
      {
        log.Warn(log::APP, "SubdivRefine: crease-sharpness count "
                                + std::to_string(usdCreaseSharpnesses.size())
                                + " is neither per-crease (" + std::to_string(usdCreaseLengths.size())
                                + ") nor per-segment (" + std::to_string(totalSegments)
                                + ") on " + mesh.GetPrim().GetPath().GetString()
                                + "; skipping creases.");
      }
      else
      {
        osCreasePairIndices.reserve(totalSegments * 2);
        osCreasePairWeights.reserve(totalSegments);
        std::size_t indexCursor   = 0;
        std::size_t segmentCursor = 0;
        for (std::size_t creaseIdx = 0; creaseIdx < usdCreaseLengths.size(); ++creaseIdx)
        {
          const auto length = static_cast<std::size_t>(usdCreaseLengths[creaseIdx]);
          for (std::size_t segIdx = 0; segIdx + 1 < length; ++segIdx)
          {
            osCreasePairIndices.push_back(usdCreaseIndices[indexCursor + segIdx]);
            osCreasePairIndices.push_back(usdCreaseIndices[indexCursor + segIdx + 1]);
            const float weight = perSegment
                                     ? usdCreaseSharpnesses[segmentCursor + segIdx]
                                     : usdCreaseSharpnesses[creaseIdx];
            osCreasePairWeights.push_back(weight);
          }
          indexCursor   += length;
          segmentCursor += (length - 1);
        }
      }
    }
  }

  // ---- Validate + copy into stable local buffers -----------------------
  // OpenSubdiv reads through the pointers we hand it during topology
  // refinement. VtArray<int>::cdata() points into reference-counted
  // storage that *should* be stable for the call duration, but the
  // bugprone-dangling-handle lint flagged it and we get consistent
  // "vector subscript out of range" crashes deep inside OS. Copy into
  // owned std::vectors as the safe path; the cost is negligible
  // (~hundreds of ints per mesh).
  std::vector<int> stableCounts(srcCounts.size());
  for (std::size_t i = 0; i < srcCounts.size(); ++i)
    stableCounts[i] = srcCounts[i];
  std::vector<int> stableIndices(srcIndices.size());
  for (std::size_t i = 0; i < srcIndices.size(); ++i)
    stableIndices[i] = srcIndices[i];

  // Validate every fv-index is in range — OS crashes internally with a
  // std::vector OOB when fed an out-of-range vertex reference, with
  // no helpful diagnostic. Cheap up-front check.
  const int numSrcVerts = static_cast<int>(srcPoints.size());
  for (const int idx : stableIndices)
  {
    if (idx < 0 || idx >= numSrcVerts)
    {
      log.Warn(log::APP, "SubdivRefine: fv-index " + std::to_string(idx)
                              + " out of range [0," + std::to_string(numSrcVerts)
                              + ") on " + mesh.GetPrim().GetPath().GetString()
                              + "; skipping refinement");
      return out;
    }
  }
  // Validate face arity: Catmark accepts 3+ verts/face; Loop is
  // strictly triangles. Both reject < 3.
  for (const int faceCount : stableCounts)
  {
    if (faceCount < 3 || (scheme == Sdc::SCHEME_LOOP && faceCount != 3))
    {
      log.Warn(log::APP, "SubdivRefine: invalid face-vertex-count " + std::to_string(faceCount)
                              + " on " + mesh.GetPrim().GetPath().GetString()
                              + "; skipping refinement");
      return out;
    }
  }

  // ---- Serialise OpenSubdiv calls across OpenMP workers --------------
  // PrepareMesh runs under `#pragma omp parallel for`. OpenSubdiv's
  // Far::TopologyRefinerFactory::Create + Vtr internals are not
  // documented as thread-safe for concurrent Create() calls — the
  // library shares internal free-lists/caches across instances.
  // Concurrent Creates manifested as "vector subscript out of range"
  // deep inside OS with no useful stack frame. Cost: 83 lobby subdiv
  // meshes serialise here, ~10ms each = ~830ms wall serial vs sub-
  // 200ms parallel. Acceptable for v2.0 first cut; if we later need
  // it parallel we'll build the refiners in a serial pre-pass and
  // cache results by prim path.
  static std::mutex osMutex;
  const std::lock_guard<std::mutex> osLock(osMutex);

  // ---- Build TopologyDescriptor ----------------------------------------
  Far::TopologyDescriptor desc;
  desc.numVertices = numSrcVerts;
  desc.numFaces = static_cast<int>(stableCounts.size());
  desc.numVertsPerFace = stableCounts.data();
  desc.vertIndicesPerFace = stableIndices.data();
  if (!osCornerIndices.empty())
  {
    desc.numCorners = static_cast<int>(osCornerIndices.size());
    desc.cornerVertexIndices = osCornerIndices.data();
    desc.cornerWeights = osCornerWeights.data();
  }
  if (!osCreasePairWeights.empty())
  {
    desc.numCreases = static_cast<int>(osCreasePairWeights.size());
    desc.creaseVertexIndexPairs = osCreasePairIndices.data();
    desc.creaseWeights = osCreasePairWeights.data();
  }

  // Optional face-varying UV channel descriptor. OpenSubdiv treats each
  // FVar channel independently — we register one for UVs when present.
  Far::TopologyDescriptor::FVarChannel fvarUvChannel{};
  std::vector<int> fvarUvIndices;
  if (srcUvsAreFaceVarying && !srcUvs.empty()
      && static_cast<int>(srcUvs.size()) == static_cast<int>(srcIndices.size()))
  {
    // Identity-indexed FVar channel: indices[i] = i. OpenSubdiv requires
    // the index buffer; the values come later via PrimvarRefiner.
    fvarUvIndices.resize(srcIndices.size());
    for (std::size_t i = 0; i < fvarUvIndices.size(); ++i)
      fvarUvIndices[i] = static_cast<int>(i);
    fvarUvChannel.numValues = static_cast<int>(srcUvs.size());
    fvarUvChannel.valueIndices = fvarUvIndices.data();
    desc.numFVarChannels = 1;
    desc.fvarChannels = &fvarUvChannel;
  }

  // ---- Build TopologyRefiner -------------------------------------------
  // Tell OS to fully validate topology — without this, Create() can
  // succeed on a non-manifold/self-touching cage and later refinement
  // reads OOB inside Vtr internals (manifests as "vector subscript out
  // of range"). With validation on, bad topology returns nullptr here
  // and we fall back to the cage.
  using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
  Factory::Options factoryOptions(scheme, options);
  factoryOptions.validateFullTopology = true;
  const std::unique_ptr<Far::TopologyRefiner> refiner(
      Factory::Create(desc, factoryOptions));
  if (!refiner)
  {
    log.Warn(log::APP, "SubdivRefine: TopologyRefiner::Create failed for "
                            + mesh.GetPrim().GetPath().GetString());
    return out;
  }

  Far::TopologyRefiner::UniformOptions refineOptions(static_cast<int>(maxLevel));
  refineOptions.fullTopologyInLastLevel = true;
  refiner->RefineUniform(refineOptions);

  // ---- Refine vertex positions ----------------------------------------
  // OpenSubdiv's PrimvarRefiner walks src->dst level-by-level. We
  // allocate one big buffer holding ALL levels' verts, then point
  // `src` at the start and step through.
  const int totalVerts = refiner->GetNumVerticesTotal();
  if (totalVerts <= 0 || static_cast<std::size_t>(totalVerts) < srcPoints.size())
  {
    log.Warn(log::APP, "SubdivRefine: bogus totalVerts=" + std::to_string(totalVerts)
                            + " vs srcPoints.size()=" + std::to_string(srcPoints.size())
                            + " for " + mesh.GetPrim().GetPath().GetString());
    return out;
  }
  std::vector<PosVtx> posBuf(static_cast<std::size_t>(totalVerts));
  for (std::size_t i = 0; i < srcPoints.size(); ++i)
  {
    posBuf[i].x = srcPoints[i][0];
    posBuf[i].y = srcPoints[i][1];
    posBuf[i].z = srcPoints[i][2];
  }
  const Far::PrimvarRefiner primvarRefiner(*refiner);
  {
    PosVtx* src = posBuf.data();
    for (int level = 1; level <= static_cast<int>(maxLevel); ++level)
    {
      PosVtx* dst = src + refiner->GetLevel(level - 1).GetNumVertices();
      primvarRefiner.Interpolate(level, src, dst);
      src = dst;
    }
  }

  // ---- Refine face-varying UVs (if present) ----------------------------
  std::vector<UvVtx> uvBuf;
  const bool refinedUvs = (desc.numFVarChannels == 1);
  if (refinedUvs)
  {
    const int totalUvs = refiner->GetNumFVarValuesTotal(0);
    uvBuf.resize(static_cast<std::size_t>(totalUvs));
    for (std::size_t i = 0; i < srcUvs.size(); ++i)
    {
      uvBuf[i].u = srcUvs[i][0];
      uvBuf[i].v = srcUvs[i][1];
    }
    UvVtx* src = uvBuf.data();
    for (int level = 1; level <= static_cast<int>(maxLevel); ++level)
    {
      UvVtx* dst = src + refiner->GetLevel(level - 1).GetNumFVarValues(0);
      primvarRefiner.InterpolateFaceVarying(level, src, dst, 0);
      src = dst;
    }
  }

  // ---- Read back the finest level --------------------------------------
  const Far::TopologyLevel& topo = refiner->GetLevel(static_cast<int>(maxLevel));
  const int numOutVerts = topo.GetNumVertices();
  const int numOutFaces = topo.GetNumFaces();

  // Locate the start of the finest level's vert range in posBuf.
  int posOffset = 0;
  for (int level = 0; level < static_cast<int>(maxLevel); ++level)
    posOffset += refiner->GetLevel(level).GetNumVertices();

  // Defensive bounds check — should never trip given totalVerts ==
  // sum of all levels' GetNumVertices() but a mismatched
  // OpenSubdiv version could in principle return a different shape.
  const std::size_t finalEnd =
      static_cast<std::size_t>(posOffset) + static_cast<std::size_t>(numOutVerts);
  if (numOutVerts < 0 || finalEnd > posBuf.size())
  {
    log.Warn(log::APP, "SubdivRefine: vertex-range OOB (posOffset="
                            + std::to_string(posOffset)
                            + " numOutVerts=" + std::to_string(numOutVerts)
                            + " posBuf=" + std::to_string(posBuf.size())
                            + ") for " + mesh.GetPrim().GetPath().GetString());
    return out;
  }

  out.points.reserve(static_cast<std::size_t>(numOutVerts));
  for (int i = 0; i < numOutVerts; ++i)
  {
    const PosVtx& vtx = posBuf[static_cast<std::size_t>(posOffset) + static_cast<std::size_t>(i)];
    out.points.emplace_back(vtx.x, vtx.y, vtx.z);
  }

  // Topology + UV readback. Catmull-Clark produces all quads at any
  // refined level (each input face splits into N quads where N =
  // original vertex count of the face). Loop produces all tris.
  out.faceVertexCounts.reserve(static_cast<std::size_t>(numOutFaces));
  out.faceVertexIndices.reserve(static_cast<std::size_t>(numOutFaces) * 4u);

  // FVar-channel offset only matters when we *have* an FVar channel.
  // Calling `GetLevel(level).GetNumFVarValues(0)` against a refiner
  // built with `numFVarChannels = 0` reads an OOB entry in OS's
  // internal FVar table — that was the "vector subscript out of range"
  // we were seeing in the positions-only path (logged "pos refinement
  // OK" then died here).
  int uvOffset = 0;
  if (refinedUvs)
  {
    out.faceVaryingUvs.reserve(static_cast<std::size_t>(numOutFaces) * 4u);
    for (int level = 0; level < static_cast<int>(maxLevel); ++level)
      uvOffset += refiner->GetLevel(level).GetNumFVarValues(0);
  }

  for (int faceIdx = 0; faceIdx < numOutFaces; ++faceIdx)
  {
    const Far::ConstIndexArray faceVerts = topo.GetFaceVertices(faceIdx);
    out.faceVertexCounts.push_back(faceVerts.size());
    for (int fvIdx = 0; fvIdx < faceVerts.size(); ++fvIdx)
      out.faceVertexIndices.push_back(faceVerts[fvIdx]);
    if (refinedUvs)
    {
      const Far::ConstIndexArray faceFVar = topo.GetFaceFVarValues(faceIdx, 0);
      for (int fvIdx = 0; fvIdx < faceFVar.size(); ++fvIdx)
      {
        const std::size_t uvIdx =
            static_cast<std::size_t>(uvOffset) + static_cast<std::size_t>(faceFVar[fvIdx]);
        const UvVtx& uvVal = uvBuf[uvIdx];
        out.faceVaryingUvs.emplace_back(uvVal.u, uvVal.v);
      }
    }
  }

  out.refined = true;
  return out;
}

}  // namespace pyxis::usd_ingest
