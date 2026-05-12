// Pyxis USD ingest — analytic-prim tessellators.

#include "AnalyticGeom.h"

#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <cmath>
#include <cstdint>

namespace pyxis::usd_ingest {

namespace {

constexpr float PYXIS_PI = 3.14159265358979323846f;

// Per-axis local frame for cylinder / cone / capsule. USD authors
// `axis` as X / Y / Z; the prim's local space then aligns the
// canonical primitive (which extends along that axis). Returns
// (axisDir, side1, side2) — `axisDir` along the prim's axis, the
// two sides spanning the cross-section plane.
struct AxisFrame {
  pxr::GfVec3f axisDir;
  pxr::GfVec3f side1;
  pxr::GfVec3f side2;
};

[[nodiscard]] AxisFrame ResolveAxisFrame(const pxr::TfToken& axisToken) noexcept
{
  if (axisToken == pxr::UsdGeomTokens->x)
    return AxisFrame{ {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
  if (axisToken == pxr::UsdGeomTokens->y)
    return AxisFrame{ {0, 1, 0}, {1, 0, 0}, {0, 0, 1} };
  return AxisFrame{ {0, 0, 1}, {1, 0, 0}, {0, 1, 0} };  // Z (USD default)
}

// Append one triangle (indices into the `points` array) to the
// result. Triangle list uses faceCounts = [3,3,3,...].
void AppendTriangle(AnalyticGeomResult& out,
                    std::uint32_t idxA, std::uint32_t idxB, std::uint32_t idxC) noexcept
{
  out.faceCounts.push_back(3);
  out.faceIndices.push_back(static_cast<int>(idxA));
  out.faceIndices.push_back(static_cast<int>(idxB));
  out.faceIndices.push_back(static_cast<int>(idxC));
}

void AppendQuad(AnalyticGeomResult& out,
                std::uint32_t idx00, std::uint32_t idx10,
                std::uint32_t idx11, std::uint32_t idx01) noexcept
{
  // CCW from outside: (00, 10, 11) + (00, 11, 01).
  AppendTriangle(out, idx00, idx10, idx11);
  AppendTriangle(out, idx00, idx11, idx01);
}

// Normalise a GfVec3f (zero-safe).
[[nodiscard]] pxr::GfVec3f SafeNormalise(const pxr::GfVec3f& vec) noexcept
{
  const float lenSq = vec[0]*vec[0] + vec[1]*vec[1] + vec[2]*vec[2];
  if (lenSq < 1e-12f)
    return pxr::GfVec3f{0.0f, 1.0f, 0.0f};
  const float invLen = 1.0f / std::sqrt(lenSq);
  return pxr::GfVec3f{vec[0]*invLen, vec[1]*invLen, vec[2]*invLen};
}

}  // namespace

// ============================================================================
// Sphere — lat/long tessellation.
// USD authors `double radius` (default 1). Local origin at centre.
// ============================================================================
AnalyticGeomResult TessellateSphere(const pxr::UsdGeomSphere& sphere) noexcept
{
  AnalyticGeomResult out;

  double radius = 1.0;
  sphere.GetRadiusAttr().Get(&radius);
  if (radius <= 0.0)
    return out;

  constexpr int LAT_SEGMENTS = 32;
  constexpr int LON_SEGMENTS = 32;
  const float radiusF = static_cast<float>(radius);

  // Vertex grid: (LAT_SEGMENTS+1) × (LON_SEGMENTS+1). Top + bottom
  // rows collapse to a pole but we keep distinct UVs by carrying
  // separate longitudinal entries.
  out.points.reserve(static_cast<std::size_t>(LAT_SEGMENTS + 1)
                     * static_cast<std::size_t>(LON_SEGMENTS + 1));
  out.normals.reserve(out.points.capacity());
  out.uvs.reserve(out.points.capacity());

  for (int lat = 0; lat <= LAT_SEGMENTS; ++lat)
  {
    const float vCoord = static_cast<float>(lat) / static_cast<float>(LAT_SEGMENTS);
    const float theta = vCoord * PYXIS_PI;  // 0 (top pole) → π (bottom)
    const float sinTheta = std::sin(theta);
    const float cosTheta = std::cos(theta);
    for (int lon = 0; lon <= LON_SEGMENTS; ++lon)
    {
      const float uCoord = static_cast<float>(lon) / static_cast<float>(LON_SEGMENTS);
      const float phi = uCoord * 2.0f * PYXIS_PI;
      const float sinPhi = std::sin(phi);
      const float cosPhi = std::cos(phi);
      const pxr::GfVec3f normal{sinTheta * cosPhi, cosTheta, sinTheta * sinPhi};
      out.points.emplace_back(normal[0] * radiusF, normal[1] * radiusF, normal[2] * radiusF);
      out.normals.push_back(normal);
      out.uvs.emplace_back(uCoord, 1.0f - vCoord);
    }
  }

  const int stride = LON_SEGMENTS + 1;
  for (int lat = 0; lat < LAT_SEGMENTS; ++lat)
  {
    for (int lon = 0; lon < LON_SEGMENTS; ++lon)
    {
      const auto v00 = static_cast<std::uint32_t>(lat * stride + lon);
      const auto v10 = static_cast<std::uint32_t>(lat * stride + lon + 1);
      const auto v01 = static_cast<std::uint32_t>((lat + 1) * stride + lon);
      const auto v11 = static_cast<std::uint32_t>((lat + 1) * stride + lon + 1);
      AppendQuad(out, v00, v10, v11, v01);
    }
  }
  out.success = true;
  return out;
}

// ============================================================================
// Cube — 24 verts (4 per face × 6 faces) so per-face normals are flat.
// USD authors `double size` (default 2). Local origin at centre.
// ============================================================================
AnalyticGeomResult TessellateCube(const pxr::UsdGeomCube& cube) noexcept
{
  AnalyticGeomResult out;

  double size = 2.0;
  cube.GetSizeAttr().Get(&size);
  if (size <= 0.0)
    return out;

  const float half = static_cast<float>(size) * 0.5f;
  out.points.reserve(24);
  out.normals.reserve(24);
  out.uvs.reserve(24);
  out.faceCounts.reserve(12);
  out.faceIndices.reserve(36);

  // Six face quads, each with its own 4 verts so the face normal is
  // flat (per-face). Order: +X, -X, +Y, -Y, +Z, -Z. Edge vectors are
  // scaled by the full edge length (size = 2·half) so origin + edgeU
  // reaches the opposite corner.
  const float side = static_cast<float>(size);
  const struct
  {
    pxr::GfVec3f normal;
    pxr::GfVec3f origin;  // bottom-left corner
    pxr::GfVec3f edgeU;
    pxr::GfVec3f edgeV;
  } faces[6] = {
      { { 1, 0, 0 }, {  half, -half, -half }, { 0, 0,  side }, { 0,  side, 0 } },  // +X
      { {-1, 0, 0 }, { -half, -half,  half }, { 0, 0, -side }, { 0,  side, 0 } },  // -X
      { { 0, 1, 0 }, { -half,  half, -half }, {  side, 0, 0 }, { 0, 0,  side } },  // +Y
      { { 0,-1, 0 }, { -half, -half,  half }, {  side, 0, 0 }, { 0, 0, -side } },  // -Y
      { { 0, 0, 1 }, { -half, -half,  half }, {  side, 0, 0 }, { 0,  side, 0 } },  // +Z
      { { 0, 0,-1 }, {  half, -half, -half }, { -side, 0, 0 }, { 0,  side, 0 } },  // -Z
  };

  for (const auto& face : faces)
  {
    const auto baseIdx = static_cast<std::uint32_t>(out.points.size());
    const pxr::GfVec3f corner10 = face.origin + face.edgeU;
    const pxr::GfVec3f corner11 = face.origin + face.edgeU + face.edgeV;
    const pxr::GfVec3f corner01 = face.origin + face.edgeV;
    out.points.push_back(face.origin);
    out.points.push_back(corner10);
    out.points.push_back(corner11);
    out.points.push_back(corner01);
    for (int i = 0; i < 4; ++i)
      out.normals.push_back(face.normal);
    out.uvs.emplace_back(0.0f, 0.0f);
    out.uvs.emplace_back(1.0f, 0.0f);
    out.uvs.emplace_back(1.0f, 1.0f);
    out.uvs.emplace_back(0.0f, 1.0f);
    AppendQuad(out, baseIdx + 0u, baseIdx + 1u, baseIdx + 2u, baseIdx + 3u);
  }
  out.success = true;
  return out;
}

// ============================================================================
// Cylinder — radial × axial tessellation + two end caps.
// USD authors: double radius (1), double height (2), token axis (Z).
// ============================================================================
AnalyticGeomResult TessellateCylinder(const pxr::UsdGeomCylinder& cylinder) noexcept
{
  AnalyticGeomResult out;

  double radius = 1.0;
  double height = 2.0;
  pxr::TfToken axisToken = pxr::UsdGeomTokens->z;
  cylinder.GetRadiusAttr().Get(&radius);
  cylinder.GetHeightAttr().Get(&height);
  cylinder.GetAxisAttr().Get(&axisToken);
  if (radius <= 0.0 || height <= 0.0)
    return out;

  constexpr int RADIAL_SEGMENTS = 32;
  const float radiusF = static_cast<float>(radius);
  const float halfHeight = static_cast<float>(height) * 0.5f;
  const AxisFrame frame = ResolveAxisFrame(axisToken);

  out.points.reserve(static_cast<std::size_t>(RADIAL_SEGMENTS + 1) * 2u
                     + 2u
                     + static_cast<std::size_t>(RADIAL_SEGMENTS) * 2u);
  out.normals.reserve(out.points.capacity());
  out.uvs.reserve(out.points.capacity());

  // Side wall — two rings of (RADIAL_SEGMENTS+1) verts (the +1 carries
  // the seam vertex so UVs wrap cleanly).
  for (int ring = 0; ring < 2; ++ring)
  {
    const float vCoord = static_cast<float>(ring);
    const float axialOffset = (ring == 0 ? -halfHeight : halfHeight);
    for (int radial = 0; radial <= RADIAL_SEGMENTS; ++radial)
    {
      const float uCoord = static_cast<float>(radial) / static_cast<float>(RADIAL_SEGMENTS);
      const float phi = uCoord * 2.0f * PYXIS_PI;
      const float cosPhi = std::cos(phi);
      const float sinPhi = std::sin(phi);
      const pxr::GfVec3f radialDir = frame.side1 * cosPhi + frame.side2 * sinPhi;
      out.points.push_back(radialDir * radiusF + frame.axisDir * axialOffset);
      out.normals.push_back(radialDir);
      out.uvs.emplace_back(uCoord, vCoord);
    }
  }
  const int stride = RADIAL_SEGMENTS + 1;
  for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
  {
    const auto v00 = static_cast<std::uint32_t>(radial);
    const auto v10 = static_cast<std::uint32_t>(radial + 1);
    const auto v01 = static_cast<std::uint32_t>(stride + radial);
    const auto v11 = static_cast<std::uint32_t>(stride + radial + 1);
    AppendQuad(out, v00, v10, v11, v01);
  }

  // End caps — fan from a centre vertex.
  for (int cap = 0; cap < 2; ++cap)
  {
    const float axialOffset = (cap == 0 ? halfHeight : -halfHeight);  // +axis (top) then -axis (bottom)
    const pxr::GfVec3f capNormal = (cap == 0 ? frame.axisDir : -frame.axisDir);
    const auto centreIdx = static_cast<std::uint32_t>(out.points.size());
    out.points.push_back(frame.axisDir * axialOffset);
    out.normals.push_back(capNormal);
    out.uvs.emplace_back(0.5f, 0.5f);
    const auto firstRingIdx = static_cast<std::uint32_t>(out.points.size());
    for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
    {
      const float uCoord = static_cast<float>(radial) / static_cast<float>(RADIAL_SEGMENTS);
      const float phi = uCoord * 2.0f * PYXIS_PI;
      const float cosPhi = std::cos(phi);
      const float sinPhi = std::sin(phi);
      const pxr::GfVec3f radialDir = frame.side1 * cosPhi + frame.side2 * sinPhi;
      out.points.push_back(radialDir * radiusF + frame.axisDir * axialOffset);
      out.normals.push_back(capNormal);
      out.uvs.emplace_back(0.5f + cosPhi * 0.5f, 0.5f + sinPhi * 0.5f);
    }
    for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
    {
      const auto vertA = firstRingIdx + static_cast<std::uint32_t>(radial);
      const auto vertB = firstRingIdx + static_cast<std::uint32_t>((radial + 1) % RADIAL_SEGMENTS);
      // Cap fan winding: top cap CCW from above, bottom CCW from
      // below — flip vertex order for the bottom.
      if (cap == 0)
        AppendTriangle(out, centreIdx, vertA, vertB);
      else
        AppendTriangle(out, centreIdx, vertB, vertA);
    }
  }
  out.success = true;
  return out;
}

// ============================================================================
// Cone — like Cylinder but the top ring collapses to the apex.
// ============================================================================
AnalyticGeomResult TessellateCone(const pxr::UsdGeomCone& cone) noexcept
{
  AnalyticGeomResult out;

  double radius = 1.0;
  double height = 2.0;
  pxr::TfToken axisToken = pxr::UsdGeomTokens->z;
  cone.GetRadiusAttr().Get(&radius);
  cone.GetHeightAttr().Get(&height);
  cone.GetAxisAttr().Get(&axisToken);
  if (radius <= 0.0 || height <= 0.0)
    return out;

  constexpr int RADIAL_SEGMENTS = 32;
  const float radiusF = static_cast<float>(radius);
  const float halfHeight = static_cast<float>(height) * 0.5f;
  const AxisFrame frame = ResolveAxisFrame(axisToken);
  const pxr::GfVec3f apex = frame.axisDir * halfHeight;

  out.points.reserve(static_cast<std::size_t>(RADIAL_SEGMENTS) * 2u + 2u);
  out.normals.reserve(out.points.capacity());
  out.uvs.reserve(out.points.capacity());

  // Side — one slanted triangle per radial segment (apex + two base
  // verts). Per-segment unique apex so the apex normal stays slanted
  // along its slice. Base ring is duplicated for the cap below.
  const float slantInvLen = 1.0f / std::sqrt(radiusF * radiusF + halfHeight * 4.0f * halfHeight);
  for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
  {
    const float uCoord  = static_cast<float>(radial) / static_cast<float>(RADIAL_SEGMENTS);
    const float uCoord1 = static_cast<float>(radial + 1) / static_cast<float>(RADIAL_SEGMENTS);
    const float phi  = uCoord  * 2.0f * PYXIS_PI;
    const float phi1 = uCoord1 * 2.0f * PYXIS_PI;
    const float cosPhi  = std::cos(phi);
    const float sinPhi  = std::sin(phi);
    const float cosPhi1 = std::cos(phi1);
    const float sinPhi1 = std::sin(phi1);
    const pxr::GfVec3f base0Dir = frame.side1 * cosPhi  + frame.side2 * sinPhi;
    const pxr::GfVec3f base1Dir = frame.side1 * cosPhi1 + frame.side2 * sinPhi1;
    const pxr::GfVec3f base0Pos = base0Dir * radiusF - frame.axisDir * halfHeight;
    const pxr::GfVec3f base1Pos = base1Dir * radiusF - frame.axisDir * halfHeight;
    // Slant normal: outward radial mixed with slight axial lift.
    // dir = (radius·sideDir, +height/2·axis) projected onto cone surface.
    const pxr::GfVec3f slantNormalMid =
        ((base0Dir + base1Dir) * 0.5f) * (2.0f * halfHeight * slantInvLen)
        + frame.axisDir * (radiusF * slantInvLen);
    const pxr::GfVec3f slantNormalNorm = SafeNormalise(slantNormalMid);

    const auto baseIdx = static_cast<std::uint32_t>(out.points.size());
    out.points.push_back(apex);
    out.points.push_back(base0Pos);
    out.points.push_back(base1Pos);
    out.normals.push_back(slantNormalNorm);
    out.normals.push_back(slantNormalNorm);
    out.normals.push_back(slantNormalNorm);
    out.uvs.emplace_back((uCoord + uCoord1) * 0.5f, 1.0f);
    out.uvs.emplace_back(uCoord , 0.0f);
    out.uvs.emplace_back(uCoord1, 0.0f);
    AppendTriangle(out, baseIdx, baseIdx + 1u, baseIdx + 2u);
  }

  // Base cap — flat disc facing -axis.
  const auto centreIdx = static_cast<std::uint32_t>(out.points.size());
  out.points.push_back(-frame.axisDir * halfHeight);
  out.normals.push_back(-frame.axisDir);
  out.uvs.emplace_back(0.5f, 0.5f);
  const auto firstRingIdx = static_cast<std::uint32_t>(out.points.size());
  for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
  {
    const float uCoord = static_cast<float>(radial) / static_cast<float>(RADIAL_SEGMENTS);
    const float phi = uCoord * 2.0f * PYXIS_PI;
    const float cosPhi = std::cos(phi);
    const float sinPhi = std::sin(phi);
    const pxr::GfVec3f radialDir = frame.side1 * cosPhi + frame.side2 * sinPhi;
    out.points.push_back(radialDir * radiusF - frame.axisDir * halfHeight);
    out.normals.push_back(-frame.axisDir);
    out.uvs.emplace_back(0.5f + cosPhi * 0.5f, 0.5f + sinPhi * 0.5f);
  }
  for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
  {
    const auto vertA = firstRingIdx + static_cast<std::uint32_t>(radial);
    const auto vertB = firstRingIdx + static_cast<std::uint32_t>((radial + 1) % RADIAL_SEGMENTS);
    // Base cap winds CCW when viewed from -axis (i.e. from outside).
    AppendTriangle(out, centreIdx, vertB, vertA);
  }
  out.success = true;
  return out;
}

// ============================================================================
// Capsule — cylinder body + two hemispherical caps. USD: radius + height
// (height is the *cylinder* part; total length = height + 2·radius).
// ============================================================================
AnalyticGeomResult TessellateCapsule(const pxr::UsdGeomCapsule& capsule) noexcept
{
  AnalyticGeomResult out;

  double radius = 0.5;
  double height = 1.0;
  pxr::TfToken axisToken = pxr::UsdGeomTokens->z;
  capsule.GetRadiusAttr().Get(&radius);
  capsule.GetHeightAttr().Get(&height);
  capsule.GetAxisAttr().Get(&axisToken);
  if (radius <= 0.0 || height < 0.0)
    return out;

  constexpr int RADIAL_SEGMENTS = 32;
  constexpr int CAP_SEGMENTS = 16;  // latitudinal segments PER hemisphere
  const float radiusF = static_cast<float>(radius);
  const float halfHeight = static_cast<float>(height) * 0.5f;
  const AxisFrame frame = ResolveAxisFrame(axisToken);

  // Helper to push one ring (RADIAL_SEGMENTS+1 verts) at a given
  // axial offset + radial scale + axial normal contribution. Returns
  // the index of the first vert in the new ring.
  auto pushRing = [&](float axialOffset, float radialScale, float normalAxialPart,
                      float vCoord) -> std::uint32_t
  {
    const auto firstIdx = static_cast<std::uint32_t>(out.points.size());
    for (int radial = 0; radial <= RADIAL_SEGMENTS; ++radial)
    {
      const float uCoord = static_cast<float>(radial) / static_cast<float>(RADIAL_SEGMENTS);
      const float phi = uCoord * 2.0f * PYXIS_PI;
      const float cosPhi = std::cos(phi);
      const float sinPhi = std::sin(phi);
      const pxr::GfVec3f radialDir = frame.side1 * cosPhi + frame.side2 * sinPhi;
      const pxr::GfVec3f pos =
          radialDir * (radiusF * radialScale) + frame.axisDir * axialOffset;
      const pxr::GfVec3f normalUnnorm = radialDir * radialScale + frame.axisDir * normalAxialPart;
      out.points.push_back(pos);
      out.normals.push_back(SafeNormalise(normalUnnorm));
      out.uvs.emplace_back(uCoord, vCoord);
    }
    return firstIdx;
  };

  auto stitchRings = [&](std::uint32_t lowerFirst, std::uint32_t upperFirst)
  {
    for (int radial = 0; radial < RADIAL_SEGMENTS; ++radial)
    {
      const auto v00 = lowerFirst + static_cast<std::uint32_t>(radial);
      const auto v10 = lowerFirst + static_cast<std::uint32_t>(radial + 1);
      const auto v01 = upperFirst + static_cast<std::uint32_t>(radial);
      const auto v11 = upperFirst + static_cast<std::uint32_t>(radial + 1);
      AppendQuad(out, v00, v10, v11, v01);
    }
  };

  // Bottom hemisphere — CAP_SEGMENTS latitudinal rings from south
  // pole up to the bottom of the cylinder. lat=0 is the south pole.
  std::vector<std::uint32_t> ringFirsts;
  for (int lat = 0; lat <= CAP_SEGMENTS; ++lat)
  {
    const float tParam = static_cast<float>(lat) / static_cast<float>(CAP_SEGMENTS);
    // tParam=0 → south pole, tParam=1 → bottom-of-cylinder.
    const float theta = (1.0f - tParam) * 0.5f * PYXIS_PI;  // π/2 → 0
    const float radialScale = std::cos(theta);
    const float axialOffset = -halfHeight - radiusF * std::sin(theta);
    const float normalAxial = -std::sin(theta);
    const float vCoord = tParam * 0.25f;  // bottom hemi maps to v in [0, 0.25]
    ringFirsts.push_back(pushRing(axialOffset, radialScale, normalAxial, vCoord));
  }

  // Cylinder body — one ring at the top of the cylinder section.
  ringFirsts.push_back(pushRing(halfHeight, 1.0f, 0.0f, 0.75f));

  // Top hemisphere — CAP_SEGMENTS rings from the top of cylinder up
  // to the north pole.
  for (int lat = 1; lat <= CAP_SEGMENTS; ++lat)
  {
    const float tParam = static_cast<float>(lat) / static_cast<float>(CAP_SEGMENTS);
    const float theta = tParam * 0.5f * PYXIS_PI;
    const float radialScale = std::cos(theta);
    const float axialOffset = halfHeight + radiusF * std::sin(theta);
    const float normalAxial = std::sin(theta);
    const float vCoord = 0.75f + tParam * 0.25f;
    ringFirsts.push_back(pushRing(axialOffset, radialScale, normalAxial, vCoord));
  }

  // Stitch every adjacent pair of rings.
  for (std::size_t i = 1; i < ringFirsts.size(); ++i)
    stitchRings(ringFirsts[i - 1], ringFirsts[i]);

  out.success = true;
  return out;
}

// ============================================================================
// BasisCurves — ribbon strip per curve. We pre-tessellate at ingest
// time with a single fixed orientation (world-up cross tangent) since
// we don't have a camera at ingest. Per-frame re-orient is post-v2.
// ============================================================================
AnalyticGeomResult TessellateBasisCurves(const pxr::UsdGeomBasisCurves& curves,
                                          const pxr::GfVec3f& worldUp) noexcept
{
  AnalyticGeomResult out;

  pxr::VtArray<pxr::GfVec3f> srcPoints;
  pxr::VtArray<int>          curveVertexCounts;
  pxr::VtArray<float>        widths;
  curves.GetPointsAttr().Get(&srcPoints);
  curves.GetCurveVertexCountsAttr().Get(&curveVertexCounts);
  curves.GetWidthsAttr().Get(&widths);
  if (srcPoints.empty() || curveVertexCounts.empty())
    return out;

  // Width semantics: per UsdGeom spec the widths primvar can be
  // constant (one value), uniform (per-curve), varying or vertex
  // (per CV). Without primvar API plumbing we accept the two common
  // forms: empty → 1.0 fallback; size 1 → constant; size == srcPoints
  // → per-vertex. Anything else → constant of widths[0].
  const auto widthAt = [&](std::size_t globalVtxIdx) -> float
  {
    if (widths.empty())
      return 1.0f;
    if (widths.size() == srcPoints.size())
      return widths[globalVtxIdx];
    return widths[0];
  };

  const pxr::GfVec3f worldUpNorm = SafeNormalise(worldUp);

  out.points.reserve(srcPoints.size() * 2u);
  out.normals.reserve(srcPoints.size() * 2u);
  out.uvs.reserve(srcPoints.size() * 2u);

  // For each curve: walk its CVs, compute tangent per vertex, expand
  // to two ribbon-edge verts at ±0.5·width along the side direction.
  // Side direction = normalise(tangent × worldUp). When tangent ∥
  // worldUp, fall back to a stable perpendicular.
  std::size_t cvOffset = 0;
  for (const int curveLen : curveVertexCounts)
  {
    if (curveLen < 2)
    {
      cvOffset += static_cast<std::size_t>(curveLen);
      continue;
    }
    const auto ribbonStart = static_cast<std::uint32_t>(out.points.size());
    for (int i = 0; i < curveLen; ++i)
    {
      const std::size_t globalIdx = cvOffset + static_cast<std::size_t>(i);
      const pxr::GfVec3f vertexPos = srcPoints[globalIdx];

      pxr::GfVec3f tangent;
      if (i == 0)
        tangent = srcPoints[globalIdx + 1u] - vertexPos;
      else if (i == curveLen - 1)
        tangent = vertexPos - srcPoints[globalIdx - 1u];
      else
        tangent = srcPoints[globalIdx + 1u] - srcPoints[globalIdx - 1u];
      tangent = SafeNormalise(tangent);

      pxr::GfVec3f side = pxr::GfCross(tangent, worldUpNorm);
      if (side.GetLengthSq() < 1e-6f)
        side = pxr::GfCross(tangent, pxr::GfVec3f{1.0f, 0.0f, 0.0f});
      side = SafeNormalise(side);
      const pxr::GfVec3f normal = SafeNormalise(pxr::GfCross(side, tangent));

      const float halfWidth = widthAt(globalIdx) * 0.5f;
      out.points.push_back(vertexPos - side * halfWidth);
      out.points.push_back(vertexPos + side * halfWidth);
      out.normals.push_back(normal);
      out.normals.push_back(normal);
      const float vCoord = static_cast<float>(i) / static_cast<float>(curveLen - 1);
      out.uvs.emplace_back(0.0f, vCoord);
      out.uvs.emplace_back(1.0f, vCoord);
    }
    // Stitch ribbon quads — (curveLen - 1) quads per curve.
    for (int i = 0; i < curveLen - 1; ++i)
    {
      const auto v00 = ribbonStart + static_cast<std::uint32_t>(i * 2);
      const auto v10 = ribbonStart + static_cast<std::uint32_t>(i * 2 + 1);
      const auto v01 = ribbonStart + static_cast<std::uint32_t>((i + 1) * 2);
      const auto v11 = ribbonStart + static_cast<std::uint32_t>((i + 1) * 2 + 1);
      AppendQuad(out, v00, v10, v11, v01);
    }
    cvOffset += static_cast<std::size_t>(curveLen);
  }

  if (out.points.empty())
    return out;
  out.success = true;
  return out;
}

// ============================================================================
// Points — billboard quad per point in the worldUp plane.
// ============================================================================
AnalyticGeomResult TessellatePoints(const pxr::UsdGeomPoints& points,
                                     const pxr::GfVec3f& worldUp) noexcept
{
  AnalyticGeomResult out;

  pxr::VtArray<pxr::GfVec3f> srcPoints;
  pxr::VtArray<float>        widths;
  points.GetPointsAttr().Get(&srcPoints);
  points.GetWidthsAttr().Get(&widths);
  if (srcPoints.empty())
    return out;

  const auto widthAt = [&](std::size_t globalVtxIdx) -> float
  {
    if (widths.empty())
      return 1.0f;
    if (widths.size() == srcPoints.size())
      return widths[globalVtxIdx];
    return widths[0];
  };

  // Stable side + up vectors in the world frame. The billboard plane
  // is perpendicular to worldUp. (Pillar B can re-orient per-frame.)
  const pxr::GfVec3f worldUpNorm = SafeNormalise(worldUp);
  pxr::GfVec3f sideRef = pxr::GfCross(worldUpNorm, pxr::GfVec3f{0.0f, 0.0f, 1.0f});
  if (sideRef.GetLengthSq() < 1e-6f)
    sideRef = pxr::GfCross(worldUpNorm, pxr::GfVec3f{1.0f, 0.0f, 0.0f});
  sideRef = SafeNormalise(sideRef);
  const pxr::GfVec3f upRef = SafeNormalise(pxr::GfCross(sideRef, worldUpNorm));
  const pxr::GfVec3f billboardNormal = worldUpNorm;

  out.points.reserve(srcPoints.size() * 4u);
  out.normals.reserve(srcPoints.size() * 4u);
  out.uvs.reserve(srcPoints.size() * 4u);

  for (std::size_t i = 0; i < srcPoints.size(); ++i)
  {
    const pxr::GfVec3f& centre = srcPoints[i];
    const float halfWidth = widthAt(i) * 0.5f;
    const pxr::GfVec3f sideOffset = sideRef * halfWidth;
    const pxr::GfVec3f upOffset = upRef * halfWidth;
    const auto baseIdx = static_cast<std::uint32_t>(out.points.size());
    out.points.push_back(centre - sideOffset - upOffset);
    out.points.push_back(centre + sideOffset - upOffset);
    out.points.push_back(centre + sideOffset + upOffset);
    out.points.push_back(centre - sideOffset + upOffset);
    for (int j = 0; j < 4; ++j)
      out.normals.push_back(billboardNormal);
    out.uvs.emplace_back(0.0f, 0.0f);
    out.uvs.emplace_back(1.0f, 0.0f);
    out.uvs.emplace_back(1.0f, 1.0f);
    out.uvs.emplace_back(0.0f, 1.0f);
    AppendQuad(out, baseIdx + 0u, baseIdx + 1u, baseIdx + 2u, baseIdx + 3u);
  }
  out.success = true;
  return out;
}

// ============================================================================
// NURBS patch — cubic Bezier path (the common production case).
// USD authors: int uVertexCount, vVertexCount; int uOrder, vOrder;
// double[] uKnots, vKnots; point3f[] points (uVertexCount * vVertexCount).
// Pyxis handles uOrder==4, vOrder==4, uVertexCount==4, vVertexCount==4
// today — the cubic Bezier patch case. Other configurations return
// success=false so the caller falls back to the M20 detect-warn-skip
// path.
// ============================================================================
namespace {

// Cubic Bernstein basis evaluated at t. The four B(t,i) sum to 1 for
// all t — useful invariant when sanity-checking the tessellator.
struct CubicBernstein {
  float b0;
  float b1;
  float b2;
  float b3;
};

[[nodiscard]] CubicBernstein BernsteinCubic(float tParam) noexcept
{
  const float oneMinusT = 1.0f - tParam;
  const float omt2 = oneMinusT * oneMinusT;
  const float tSq  = tParam * tParam;
  return CubicBernstein{
      .b0 = omt2 * oneMinusT,          // (1-t)^3
      .b1 = 3.0f * tParam * omt2,      // 3 t (1-t)^2
      .b2 = 3.0f * tSq * oneMinusT,    // 3 t^2 (1-t)
      .b3 = tSq * tParam,              // t^3
  };
}

}  // namespace

AnalyticGeomResult TessellateNurbsPatch(const pxr::UsdGeomNurbsPatch& patch) noexcept
{
  AnalyticGeomResult out;

  int uVertexCount = 0;
  int vVertexCount = 0;
  int uOrder = 0;
  int vOrder = 0;
  pxr::VtArray<pxr::GfVec3f> controlPoints;
  patch.GetUVertexCountAttr().Get(&uVertexCount);
  patch.GetVVertexCountAttr().Get(&vVertexCount);
  patch.GetUOrderAttr().Get(&uOrder);
  patch.GetVOrderAttr().Get(&vOrder);
  patch.GetPointsAttr().Get(&controlPoints);

  // Only the cubic Bezier 4x4 case is handled in v2 first cut. Other
  // configurations (higher-order, larger control nets, rational with
  // pointWeights, trim curves) are deferred to a follow-up.
  if (uOrder != 4 || vOrder != 4 || uVertexCount != 4 || vVertexCount != 4
      || controlPoints.size() != 16u)
    return out;

  // Convert the control net to a flat row-major 4x4 array for fast
  // Bernstein-weighted sums.
  pxr::GfVec3f net[4][4];
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col)
      net[row][col] = controlPoints[static_cast<std::size_t>(row) * 4u
                                     + static_cast<std::size_t>(col)];

  constexpr int TESSELLATION = 16;  // 16x16 quads = 256 triangles per patch
  const int gridDim = TESSELLATION + 1;

  out.points.reserve(static_cast<std::size_t>(gridDim) * static_cast<std::size_t>(gridDim));
  out.normals.reserve(out.points.capacity());
  out.uvs.reserve(out.points.capacity());

  // Evaluate the Bezier surface on a uniform (u, v) grid + a simple
  // central-difference normal estimate.
  for (int vIdx = 0; vIdx < gridDim; ++vIdx)
  {
    const float vCoord = static_cast<float>(vIdx) / static_cast<float>(TESSELLATION);
    const CubicBernstein bernV = BernsteinCubic(vCoord);
    for (int uIdx = 0; uIdx < gridDim; ++uIdx)
    {
      const float uCoord = static_cast<float>(uIdx) / static_cast<float>(TESSELLATION);
      const CubicBernstein bernU = BernsteinCubic(uCoord);
      const float bvArr[4] = {bernV.b0, bernV.b1, bernV.b2, bernV.b3};
      const float buArr[4] = {bernU.b0, bernU.b1, bernU.b2, bernU.b3};
      pxr::GfVec3f pos{0.0f, 0.0f, 0.0f};
      for (int row = 0; row < 4; ++row)
      {
        for (int col = 0; col < 4; ++col)
        {
          pos += net[row][col] * (bvArr[row] * buArr[col]);
        }
      }
      out.points.push_back(pos);
      // Flat default normal — replaced below via cross-product of
      // adjacent-vertex deltas after the full grid is built.
      out.normals.emplace_back(0.0f, 0.0f, 1.0f);
      out.uvs.emplace_back(uCoord, vCoord);
    }
  }

  // Compute per-vertex normals via cross-product of grid-neighbour
  // deltas. Edge / corner vertices use the nearest interior pair.
  for (int vIdx = 0; vIdx < gridDim; ++vIdx)
  {
    for (int uIdx = 0; uIdx < gridDim; ++uIdx)
    {
      const int uPrev = (uIdx == 0) ? 0 : uIdx - 1;
      const int uNext = (uIdx == gridDim - 1) ? uIdx : uIdx + 1;
      const int vPrev = (vIdx == 0) ? 0 : vIdx - 1;
      const int vNext = (vIdx == gridDim - 1) ? vIdx : vIdx + 1;
      const auto vRow = static_cast<std::size_t>(vIdx) * static_cast<std::size_t>(gridDim);
      const auto vPrevRow = static_cast<std::size_t>(vPrev) * static_cast<std::size_t>(gridDim);
      const auto vNextRow = static_cast<std::size_t>(vNext) * static_cast<std::size_t>(gridDim);
      const pxr::GfVec3f uTangent =
          out.points[vRow + static_cast<std::size_t>(uNext)]
          - out.points[vRow + static_cast<std::size_t>(uPrev)];
      const pxr::GfVec3f vTangent =
          out.points[vNextRow + static_cast<std::size_t>(uIdx)]
          - out.points[vPrevRow + static_cast<std::size_t>(uIdx)];
      out.normals[vRow + static_cast<std::size_t>(uIdx)] =
          SafeNormalise(pxr::GfCross(uTangent, vTangent));
    }
  }

  // Stitch quads.
  for (int vIdx = 0; vIdx < TESSELLATION; ++vIdx)
  {
    for (int uIdx = 0; uIdx < TESSELLATION; ++uIdx)
    {
      const auto vRow0  = static_cast<std::uint32_t>(vIdx)     * static_cast<std::uint32_t>(gridDim);
      const auto vRow1  = static_cast<std::uint32_t>(vIdx + 1) * static_cast<std::uint32_t>(gridDim);
      const auto v00 = vRow0 + static_cast<std::uint32_t>(uIdx);
      const auto v10 = vRow0 + static_cast<std::uint32_t>(uIdx + 1);
      const auto v01 = vRow1 + static_cast<std::uint32_t>(uIdx);
      const auto v11 = vRow1 + static_cast<std::uint32_t>(uIdx + 1);
      AppendQuad(out, v00, v10, v11, v01);
    }
  }

  out.success = true;
  return out;
}

}  // namespace pyxis::usd_ingest
