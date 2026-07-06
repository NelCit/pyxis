// Pyxis renderer — shared first-hit surface shading (P4 pass split).
//
// The shading megakernel body extracted VERBATIM from the pre-split
// closesthit.slang into a parameterized function so BOTH consumers stay
// in lockstep by construction (§33.7 byte-identical contract):
//
//   * raytraced_lighting_raygen.slang — shades the PRIMARY hit it reads
//     from the visibility buffer (instanceId / primitiveIndex / bary /
//     hitT), reconstructing worldHit = origin + hitT·dir and sourcing
//     the object→world transform from gInstanceInfo (P2 guaranteed the
//     packed transform carries bit-identical floats to the TLAS
//     instance desc, hence to ObjectToWorld3x4()).
//   * raytraced_lighting_closesthit.slang — the retained megakernel,
//     now serving ONLY secondary rays (transparency continuation +
//     reflections); it forwards the hit-stage intrinsics
//     (InstanceID() / PrimitiveIndex() / attribs.barycentrics /
//     RayTCurrent() / WorldRayDirection() / ObjectToWorld3x4() /
//     WorldToObject3x4()) into the same function.
//
// Trace-dependent inputs are parameters; every constant / epsilon /
// clamp / branch order is character-identical to the pre-split
// closesthit. Shadow / AO / reflection rays TraceRay from whichever
// stage invoked this (legal in raygen and closesthit alike); the
// recursion-budget gates take `recursionDepth` as a parameter so they
// evaluate exactly as they did against payload.recursionDepth (first
// hit: depth 0).
//
// Resource declarations live HERE (one place per translation unit);
// gCamera comes via camera_ray.slang, the dome/light resources via
// dome_sample.slang, gMaterials/gInstanceInfo via raytraced_anyhit.slang
// (RTX-alignment WP2 rewrite — dedupes the two declarations that used
// to live separately here AND in raytraced_anyhit.slang). Binding
// numbers are the canonical SceneBindings Set-0 slots
// (Private/Passes/SceneBindings.h), shared verbatim by every RT
// pipeline that includes this module — today RaytracedLightingPass
// (via raytraced_lighting_{raygen,closesthit}.slang) and
// RaytracedGBufferPass (via raytraced_gbuffer.slang, which reuses
// EvaluateSurfaceMaterial below for its primary-hit material eval).

#ifndef PYXIS_SHADING_SLANG
#define PYXIS_SHADING_SLANG

#include "ShaderInterop.slang"
#include "camera_ray.slang"   // gCamera (binding 0) — pixelSpreadRadians for ray-cone LOD
#include "dome_sample.slang"  // gLights (5) + gDomeEnvMap (10) + gDomeSampler (11) + PYXIS_PI/TWO_PI
#include "raytraced_anyhit.slang"  // gMaterials (2) + gInstanceInfo (3) + alpha-test helpers
#include "openpbr_material.slang"  // Q2 — the complete OpenPBR closure (pure math, spec-constant gated)

// Shadow / AO / reflection rays trace against the same TLAS the
// primary rays traversed. Same binding-set entry as the pre-split
// pipeline — no new layout slot.
[[vk::binding(1, 0)]]  RaytracingAccelerationStructure     gTlas;
[[vk::binding(4, 0)]]  StructuredBuffer<MeshInfoGpu>        gMeshInfo;
[[vk::binding(9, 0)]]  StructuredBuffer<float4>             gMeshFaceNormals;
[[vk::binding(12, 0)]] SamplerState                         gBindlessSampler;
[[vk::binding(7, 0)]]  StructuredBuffer<float2>             gMeshUvs;
[[vk::binding(6, 0)]]  StructuredBuffer<uint>               gMeshIndices;
[[vk::binding(13, 0)]] Texture2D                            gBindlessTextures[BINDLESS_TEXTURES_CAP];
[[vk::binding(8, 0)]]  StructuredBuffer<VertexAttribGpu>    gMeshVertexAttribs;

// USD st → image-space V flip (Seahorn root-cause close-out, RTX-
// alignment design "Measured gap" notes). UsdGeomPrimvar `st` puts
// (0,0) at the image's LOWER-left (UsdUVTexture / MDL uv convention);
// image files store rows top-first and Pyxis's TextureCache uploads
// them in file order, so a spec-correct lookup must flip V at the
// image boundary. Pyxis historically sampled raw `st` — invisible on
// tileable/noise content (wood, terrazzo: statistically v-symmetric)
// but catastrophic on bake atlases, where un-flipped island UVs land
// in the dilation gutters BETWEEN chart rows (= the Seahorn vase's
// striped sector; verified empirically: the flip turns the striped
// back view into the same coherent glaze ovrtx renders, with authored
// UVs byte-verified against pxr ComputeFlattened). The flip lives
// HERE — after the MDL texture_rotate/scale/translate transforms,
// which operate in MDL's own bottom-left UV space — not at ingest.
// Golden impact: every textured fixture rebakes (tracked debt).
float2 StToImageUv(float2 uv) {
    return float2(uv.x, 1.0 - uv.y);
}

// Per-texture LOD-aware sample. Picks a mip level from the ray-cone
// footprint (radius at the hit, in world units) × the texture's own
// dimensions: log2(coneRadius × textureSize) approximates the per-
// pixel texel-space stride. Clamped to >= 0 so near hits sample
// mip 0. Single-channel `coneRadius` argument is shared across all
// per-hit Sample calls; per-texture variation comes from the
// per-texture GetDimensions query.
float3 SampleByCone(uint texIdx, float2 uv, float coneRadius) {
    float texW;
    float texH;
    gBindlessTextures[texIdx].GetDimensions(texW, texH);
    const float texSize = sqrt(texW * texH);
    const float lod = max(0.0, log2(max(coneRadius * texSize, 1.0)));
    return gBindlessTextures[texIdx].SampleLevel(gBindlessSampler, StToImageUv(uv), lod).rgb;
}

float4 SampleByConeRGBA(uint texIdx, float2 uv, float coneRadius) {
    float texW;
    float texH;
    gBindlessTextures[texIdx].GetDimensions(texW, texH);
    const float texSize = sqrt(texW * texH);
    const float lod = max(0.0, log2(max(coneRadius * texSize, 1.0)));
    return gBindlessTextures[texIdx].SampleLevel(gBindlessSampler, StToImageUv(uv), lod);
}

// Anisotropic sample via hardware SampleGrad. `ddxUv` / `ddyUv` are the
// UV-space footprint axes (∂uv/∂screenX-equivalent, ∂uv/∂screenY-
// equivalent) the shading derives from the ray-cone ellipse +
// surface tangent frame. The HW builds the anisotropic footprint from
// the two gradient vectors and runs N-tap anisotropic filtering — sharp
// along the minor axis, filtered along the stretched major axis. This
// is the NVIDIA RTX texture-LOD approach and removes grazing-angle
// moiré/streaking that isotropic mip selection (SampleByCone) can't.
// Fragmented-UV footprint guard (Seahorn root-cause close-out, RTX-
// alignment design "Measured gap" notes). The ray-cone → UV-gradient
// derivation assumes a locally continuous parametrization; on meshes
// whose primvar is authored as thousands of disjoint per-quad islands
// (Omniverse bake-tool output — Seahorn: ~5120 islands, avg extent
// 0.017 UV), neighbouring-texel-density estimates produce enormous
// and misoriented gradient ellipses, and hardware anisotropic
// filtering then streaks across unrelated atlas charts (verified
// empirically: bypassing the gradient path removes the artifact; the
// authored UVs and the ingest stream are byte-exact vs USD ground
// truth). Clamping each gradient axis to a bounded texel span is an
// exact no-op for continuous UVs — a 64-texel footprint at the
// clamp means the true gradients are already far past any useful
// anisotropic ratio — and bounds the damage on fragmented islands to
// sub-chart scale instead of atlas scale.
static const float MAX_GRAD_FOOTPRINT_TEXELS = 64.0;

void ClampGradFootprint(uint texIdx, inout float2 ddxUv, inout float2 ddyUv) {
    float texW;
    float texH;
    gBindlessTextures[texIdx].GetDimensions(texW, texH);
    const float maxUvSpan = MAX_GRAD_FOOTPRINT_TEXELS / max(sqrt(texW * texH), 1.0);
    const float lenX = length(ddxUv);
    const float lenY = length(ddyUv);
    if (lenX > maxUvSpan)
        ddxUv *= maxUvSpan / lenX;
    if (lenY > maxUvSpan)
        ddyUv *= maxUvSpan / lenY;
}

float3 SampleByGrad(uint texIdx, float2 uv, float2 ddxUv, float2 ddyUv) {
    ClampGradFootprint(texIdx, ddxUv, ddyUv);
    // StToImageUv negates the V axis; the gradients live in the same
    // UV space, so their V components flip sign with it.
    return gBindlessTextures[texIdx].SampleGrad(gBindlessSampler, StToImageUv(uv),
                                                float2(ddxUv.x, -ddxUv.y),
                                                float2(ddyUv.x, -ddyUv.y)).rgb;
}

float4 SampleByGradRGBA(uint texIdx, float2 uv, float2 ddxUv, float2 ddyUv) {
    ClampGradFootprint(texIdx, ddxUv, ddyUv);
    return gBindlessTextures[texIdx].SampleGrad(gBindlessSampler, StToImageUv(uv),
                                                float2(ddxUv.x, -ddxUv.y),
                                                float2(ddyUv.x, -ddyUv.y));
}

// PYXIS_PI / PYXIS_TWO_PI come from dome_sample.slang (identical
// literals to the pre-split closesthit's own copies). The PI/INV_PI
// constants the BSDF used to keep here moved with the BSDF into
// openpbr_material.slang (OPENPBR_PI / OPENPBR_INV_PI).

// V2.B — ray recursion budget. Every secondary TraceRay (reflection,
// AO, shadow) is gated on `recursionDepth + 1u < MAX_RAY_RECURSION`,
// and the pipeline's maxRecursionDepth (RaytracedLightingPass) is set
// to the same value. This matches the plan_for_v2.md §V2.B doctrine
// (direct + shadows + 1 reflection bounce = depth 3) exactly.
//
// Note: opacity TRANSMISSION no longer spends this budget. An earlier
// follow-up traced a recursive continuation ray straight through each
// transparent surface, which forced the cap up to 6 (a glass pane is 2
// surfaces; two panes = 4, plus a reflection + a shadow level). That
// see-through is composited FRONT-TO-BACK by the lighting raygen's
// transmission loop at recursion depth 0 — see
// raytraced_lighting_raygen.slang — so stacked transparent layers are
// unbounded and free of recursion cost, and the budget is the
// documented 3.
// P5 (design D2): a Vulkan specialization constant — the C++ side
// (RaytracedLightingPass::MAX_RAY_RECURSION) is the single source of
// truth feeding BOTH this value and the pipeline's maxRecursionDepth,
// killing the literal-3-duplicated-shader+C++ hazard. The default
// below matches it; the driver folds the provided value at
// pipeline-creation time.
[[vk::constant_id(SPEC_ID_MAX_RAY_RECURSION)]] const uint MAX_RAY_RECURSION = 3u;

// P5 (design D2) — cap of the reflection branch's front-to-back
// transmission composite below (per-segment LOOP iterations, not
// hardware recursion). Hoisted from a ShadeSurfaceHit local to module
// scope as a specialization constant; same value (16), now C++-fed
// (RaytracedLightingPass::REFL_MAX_SEGMENTS).
[[vk::constant_id(SPEC_ID_REFL_MAX_SEGMENTS)]] const uint REFL_MAX_SEGMENTS = 16u;

// Q2 (openpbr-complete-design.md): the Schlick/GGX subset that lived
// here (SchlickF0 / SchlickFresnel / GgxSpecular) is RETIRED — every
// lobe evaluation now goes through the complete OpenPBR closure in
// openpbr_material.slang (EvaluateOpenPBRDirect / EvaluateOpenPBRAmbient
// / OpenPBRReflectionWeight / OpenPBRTransmittance / OpenPBREmission).
// This kernel keeps ONLY orchestration: hit reconstruction, the
// UV/LOD/texture/normal pipeline, the light loop, AO/shadow/reflection
// rays and the front-to-back composite contract with the raygen.

// Affine inverse of a row-major 3×4 object→world transform (upper 3×3
// linear part; translation in the last column — column-vector
// convention, v' = M·v). Used ONLY by the lighting raygen to feed the
// `worldToObject` parameter below: raygen has no WorldToObject3x4()
// intrinsic. The megakernel closesthit keeps passing the hardware
// WorldToObject3x4() — the shader-computed inverse may differ from the
// driver's in final ULPs, which only MATERIAL_FLAG_OBJECT_PROJECTION
// materials can observe (no byte-equal golden fixture authors one; the
// World Lobby suite that does is RMSE-tolerant).
float3x4 InvertAffine3x4(float3x4 m) {
    const float3 r0 = m[0].xyz;
    const float3 r1 = m[1].xyz;
    const float3 r2 = m[2].xyz;
    const float3 x = cross(r1, r2);
    const float3 y = cross(r2, r0);
    const float3 z = cross(r0, r1);
    const float det = dot(r0, x);
    const float invDet = (abs(det) > 1e-12) ? (1.0 / det) : 0.0;
    // A⁻¹ = adj(A)ᵀ / det: rows of the inverse gather the component-i
    // entries of the row cross-products above.
    const float3 i0 = float3(x.x, y.x, z.x) * invDet;
    const float3 i1 = float3(x.y, y.y, z.y) * invDet;
    const float3 i2 = float3(x.z, y.z, z.z) * invDet;
    const float3 t = float3(m[0].w, m[1].w, m[2].w);
    return float3x4(float4(i0, -dot(i0, t)),
                    float4(i1, -dot(i1, t)),
                    float4(i2, -dot(i2, t)));
}

// The fields the two callers consume from a shaded surface hit. The
// remaining HitInfo fields (hitT / instanceId / elementId / worldHit)
// are inputs the caller already holds.
struct SurfaceShadeResult {
    float3 emitted;        // → payload.color (surface-emitted radiance)
    float3 transmittance;  // → payload.transmittance (light let through from behind)
    float3 normalWorld;    // → payload.normalWorld (shading normal, post normal-map)
    float3 baseColor;      // → payload.baseColor (post-texture albedo)
    uint   materialId;     // → payload.materialId (material slot)
};

// RTX-alignment design (rtx-realtime-alignment-design.md), WP2-core —
// the material+geometry evaluation subset of the pre-split closesthit
// body, extracted into its OWN function so RaytracedGBufferPass's
// raygen (raytraced_gbuffer.slang) can evaluate the primary hit's
// material ONCE, bit-identically to what ShadeSurfaceHit below computes
// for the SAME hit (same function, same inputs — not a duplicated
// reimplementation). Everything LIGHTING-dependent (the per-light loop,
// shadow rays, reflection trace, front-to-back composite) stays in
// ShadeSurfaceHit, which calls this first. `emission` is filled in here
// too (§"M9 emission" below) — it depends only on the material + UV/LOD
// pipeline, not on the light loop, so hoisting it above the loop changes
// nothing about its value (independent floating-point expressions;
// see the design's determinism note on reusing shared functions).
struct SurfaceMaterialEval {
    OpenPBRSurfaceParams pbr;    // complete, incl. pbr.emission (self-emission, pre coat tint)
    float3 baseColor;            // post-texture albedo
    float3 normalWorld;          // final shading normal (post normal-map, horizon-clamped)
    uint   materialId;           // material slot
    float  opacity;              // mat.opacity, RAW (pbr.geometryOpacity is saturate() of this;
                                  // ShadeSurfaceHit's final composite needs the unsaturated value)
};

// Evaluate the material + UV/LOD/normal pipeline for one surface hit —
// the pre-split closesthit body, verbatim, with the trace-dependent
// inputs parameterized:
//   instanceId      = InstanceID()                 (or VisibilityGpu.instanceId)
//   primitiveIndex  = PrimitiveIndex()             (or VisibilityGpu.primitiveIndex)
//   bary            = attribs.barycentrics         (or VisibilityGpu.bary, f16-packed)
//   worldHit        = WorldRayOrigin() + RayTCurrent() * WorldRayDirection()
//                                                  (or origin + hitT·dir, reconstructed)
//   rayDir          = WorldRayDirection()          (or the BuildCameraRay direction)
//   hitT            = RayTCurrent()                (or VisibilityGpu.hitT)
//   objectToWorld   = ObjectToWorld3x4()           (or gInstanceInfo[id].objectToWorld)
//   worldToObject   = WorldToObject3x4()           (or InvertAffine3x4(objectToWorld))
SurfaceMaterialEval EvaluateSurfaceMaterial(uint instanceId, uint primitiveIndex, float2 bary,
                                            float3 worldHit, float3 rayDir, float hitT,
                                            float3x4 objectToWorld, float3x4 worldToObject) {
    // §15: instanceId is the per-instance slot. The packed instance
    // record resolves it to a material slot (0 = "no material assigned"
    // → sentinel grey material at materials[0]) + the mesh slot whose
    // packed MeshInfoGpu record carries every side-table offset.
    const InstanceInfoGpu instInfo = gInstanceInfo[instanceId];
    const uint materialId = instInfo.materialSlot;
    const OpenPBRMaterialGPU mat = gMaterials[materialId];

    // M8a UV pipeline: interpolate per-vertex UV via barycentrics,
    // then bindless-sample the baseColor texture (when authored) so
    // textured materials actually pick up their texels instead of
    // collapsing to the scalar fallback.
    //
    //   mi = gMeshInfo[meshSlot]
    //   v0/1/2 = gMeshIndices[mi.indexOffset + primitiveIndex*3 + {0,1,2}]
    //   uv0/1/2 = gMeshUvs[mi.uvOffset + v_i]
    //   uv = uv0*(1-bary.x-bary.y) + uv1*bary.x + uv2*bary.y
    // bary is the (u,v) pair the closesthit input attribute carries
    // (attribs.barycentrics); w0 = 1 - u - v is the third weight.
    const uint meshSlot = instInfo.meshSlot;
    const MeshInfoGpu mi = gMeshInfo[meshSlot];
    const uint indexBase = mi.indexOffset + primitiveIndex * 3u;
    const uint v0 = gMeshIndices[indexBase + 0u];
    const uint v1 = gMeshIndices[indexBase + 1u];
    const uint v2 = gMeshIndices[indexBase + 2u];
    const uint uvBase = mi.uvOffset;
    const float2 uv0 = gMeshUvs[uvBase + v0];
    const float2 uv1 = gMeshUvs[uvBase + v1];
    const float2 uv2 = gMeshUvs[uvBase + v2];
    const float  w0   = 1.0 - bary.x - bary.y;
    float2 uv   = uv0 * w0 + uv1 * bary.x + uv2 * bary.y;

    // V2.A.24 — per-material UV transform (OmniPBR texture_scale /
    // texture_rotate / texture_translate). This reproduces, verbatim,
    // MDL base::rotation_translation_scale() composed with
    // base::transform_coordinate() (NVIDIA/MDL-SDK base.mdl), which is
    // what OmniPBR.mdl wires its three texture_* params through:
    //
    //   M = rotate * scale            (transform_coordinate does M·uv)
    //   scale[col3] = translate - 0.5  (translate applied BEFORE rotate)
    //   rotate[col3] = +0.5            (rotation pivots about UV centre)
    //
    // Expanding for the Z-only rotation OmniPBR uses (degrees→radians
    // happens CPU-side in PackMaterialGpu):
    //   a = scaleX*u + translateX - 0.5
    //   b = scaleY*v + translateY - 0.5
    //   u' =  cos*a + sin*b + 0.5
    //   v' = -sin*a + cos*b + 0.5
    // i.e. scale (multiply: scale↑ ⇒ more tiling) → translate → rotate
    // about (0.5,0.5). transform_coordinate is column-vector (v'=M·v),
    // matching Pyxis §10. Gated on the flag so identity-UV pays nothing.
    // NOTE on handedness: Pyxis samples `st` without a V-flip whereas
    // OmniPBR's `st` origin is bottom-left; a reflection about v=0.5
    // conjugates rotation θ→-θ. If a non-90° rotation ever appears
    // mirrored vs the OmniPBR reference, negating uvRotateRadians (one
    // sign) is the fix — 90° tiling textures (the lobby oak) are
    // grain-direction-identical under either sign.
    // V2.B.* — OmniPBR project_uvw: world/object-space planar projection.
    // Derive UV from the WORLD hit position instead of the mesh UVs, with
    // the texture U axis along world-up so directional detail (wood grain)
    // reads consistently regardless of the asset's per-mesh unwrap. The
    // projection plane is chosen from the dominant face-normal axis
    // (triplanar-style, single dominant axis). texture_scale tiles it;
    // texture_rotate/translate are skipped (the projection already orients
    // the texture). Replaces the mesh-UV transform when the flag is set.
    // RFC 0005 — planar projection. World space projects from the world hit
    // position; object space transforms the hit (and face normal) into the
    // instance's local frame first, so a tiled prop placed at many
    // transforms projects identically in each (OmniPBR world_or_object=1).
    const bool worldProj  = (mat.flags & MATERIAL_FLAG_WORLD_PROJECTION) != 0u;
    const bool objectProj = (mat.flags & MATERIAL_FLAG_OBJECT_PROJECTION) != 0u;
    const bool planarProj = worldProj || objectProj;
    if (planarProj) {
        const float3 hitW = worldHit;
        const float3 faceNormalLocal =
            gMeshFaceNormals[mi.faceOffset + primitiveIndex].xyz;
        float3 hp;
        float3 fnP;
        if (objectProj) {
            // Project in the instance's local frame: transform the hit by
            // WorldToObject; the stored face normal is ALREADY object-space.
            hp  = mul(worldToObject, float4(hitW, 1.0)).xyz;
            fnP = abs(normalize(faceNormalLocal));
        } else {
            const float3x4 o2wp = objectToWorld;
            const float3x3 w3   = float3x3(o2wp[0].xyz, o2wp[1].xyz, o2wp[2].xyz);
            hp  = hitW;
            fnP = abs(normalize(mul(w3, faceNormalLocal)));
        }
        float2 wuv;
        if (fnP.y >= fnP.x && fnP.y >= fnP.z) wuv = float2(hp.x, hp.z);  // floor/ceiling
        else if (fnP.x >= fnP.z)              wuv = float2(hp.y, hp.z);  // wall ⟂X: U=up
        else                                  wuv = float2(hp.y, hp.x);  // wall ⟂Z: U=up
        uv = wuv * float2(mat.uvScaleX, mat.uvScaleY);
    } else if ((mat.flags & MATERIAL_FLAG_HAS_UV_TRANSFORM) != 0u) {
        const float cosR = cos(mat.uvRotateRadians);
        const float sinR = sin(mat.uvRotateRadians);
        const float a = mat.uvScaleX * uv.x + mat.uvTranslateX - 0.5;
        const float b = mat.uvScaleY * uv.y + mat.uvTranslateY - 0.5;
        uv = float2( cosR * a + sinR * b + 0.5,
                    -sinR * a + cosR * b + 0.5);
    }

    // M9-fidelity ray-cone LOD. Per-pixel angular spread × ray
    // distance gives the cone radius at the hit; combined with the
    // texture's own dimensions (queried per Sample) it picks the
    // right mip level without ray differentials. Distance is the
    // dominant aliasing cause; we skip the |cos(view, normal)|
    // grazing-angle correction (would need the not-yet-computed
    // smooth normal upstream of the baseColor sample). Ref:
    // Akenine-Möller, Ray Tracing Gems Ch 20.
    const float coneRadius = gCamera.pixelSpreadRadians * hitT;

    // V2.B.* — UV-density-aware LOD. The cone radius is a WORLD-space
    // length; SampleByCone needs the footprint in UV space to pick the
    // mip (uvFootprint × texSize = texel footprint). The conversion is
    // texels-per-world = (UV-per-object-length) / (world-per-object-
    // length): the first factor is the per-triangle texel density
    // packed into gMeshFaceNormals[].w (sqrt(uvArea/objArea), set at
    // CreateMesh), the second is the object→world scale read off the
    // instance transform's first row. Without this, heavily-tiled
    // surfaces (a World Lobby wall tiles ~2900×) select a too-low mip
    // and alias into herringbone moiré even with a full mip chain.
    // Density 0 (degenerate tri / no UVs) → fall back to coneRadius,
    // i.e. the previous "1 UV unit per world unit" assumption.
    const uint   faceDensitySlot = mi.faceOffset + primitiveIndex;
    const float  faceTexelDensity = gMeshFaceNormals[faceDensitySlot].w;
    const float3x4 instanceXform = objectToWorld;
    const float  objToWorldScale = max(length(instanceXform[0].xyz), 1e-6);
    // The per-material UV transform (texture_scale) multiplies the UV
    // AFTER the mesh-density was measured, so it multiplies the texel
    // footprint too. A material that tiles its texture (e.g. felt /
    // fabric with texture_scale = (50,50)) minifies 50× harder than the
    // raw mesh UVs imply; without this factor the LOD picks a far-too-
    // low mip and the texture aliases into noise/herringbone even up
    // close. Area scale = |uvScaleX · uvScaleY|; sqrt gives the linear
    // footprint multiplier. Rotation/translation don't change footprint.
    float matTileScale = 1.0;
    if ((mat.flags & MATERIAL_FLAG_HAS_UV_TRANSFORM) != 0u)
        matTileScale = sqrt(max(abs(mat.uvScaleX * mat.uvScaleY), 1e-6));

    // Grazing-angle anisotropy. At a glancing view the per-pixel texel
    // footprint is stretched along the surface; an isotropic cone radius
    // badly under-estimates it and the texture aliases into diagonal
    // streaks / moiré (felt + tiled walls seen edge-on). True fix is
    // anisotropic filtering, but a deterministic isotropic proxy is to
    // divide the footprint by |N·V| — as the view grazes (N·V → 0) the
    // footprint grows and a higher (blurrier) mip is selected, trading
    // sharpness for no aliasing. Clamp to 0.1 so it boosts at most ~10×
    // (~3.3 mip levels) and never blows up to a 1×1 mip. Uses the
    // per-triangle face normal (already fetched for the density slot).
    const float3x3 worldFromObj3x3 = float3x3(instanceXform[0].xyz,
                                              instanceXform[1].xyz,
                                              instanceXform[2].xyz);
    const float3 faceNormalW = normalize(mul(worldFromObj3x3,
                                             gMeshFaceNormals[faceDensitySlot].xyz));
    const float  ndotvLod = max(abs(dot(faceNormalW, normalize(-rayDir))), 0.1);
    const float  uvFootprint = ((faceTexelDensity > 0.0)
                                   ? coneRadius * faceTexelDensity / objToWorldScale
                                   : coneRadius) * matTileScale / ndotvLod;

    // V2.B.* — anisotropic texture footprint → hardware SampleGrad (the
    // NVIDIA RTX texture-LOD approach). The ray cone is an isotropic disk
    // of radius coneRadius ⟂ to the ray; projected onto the surface at a
    // grazing angle it's an ELLIPSE — minor axis = coneRadius, major axis
    // = coneRadius/|N·V| along the in-plane view direction. Converting the
    // two ellipse axes to UV gradients (via the tangent frame + texel
    // density) and feeding SampleGrad gives true anisotropic filtering:
    // sharp across the minor axis, filtered along the stretched major
    // axis → no grazing moiré WITHOUT over-blurring (which the isotropic
    // SampleByCone / `/|N·V|` proxy can't). Falls back to a square
    // isotropic footprint when the mesh authored no tangents.
    float2 ddxUv;
    float2 ddyUv;
    if (planarProj) {
        // World-projected UV (meters × scale): the texel footprint per
        // pixel = cone radius (world m) × tile scale, grazing-boosted.
        // Isotropic is fine here — world-projected surfaces are flat
        // panels, not the grazing-critical case the tangent-frame aniso
        // targets, and the mesh tangent frame doesn't apply to this UV.
        const float3 vw  = normalize(-rayDir);
        const float  nd  = max(abs(dot(faceNormalW, vw)), 0.1);
        const float  fpw = coneRadius * max(abs(mat.uvScaleX), abs(mat.uvScaleY)) / nd;
        ddxUv = float2(fpw, 0.0);
        ddyUv = float2(0.0, fpw);
    } else {
        const uint   tanBase  = mi.vertexAttribOffset;
        const float4 tan0     = gMeshVertexAttribs[tanBase + v0].tangent;
        const float3 tanLocal = tan0.xyz * w0
                              + gMeshVertexAttribs[tanBase + v1].tangent.xyz * bary.x
                              + gMeshVertexAttribs[tanBase + v2].tangent.xyz * bary.y;
        if (dot(tanLocal, tanLocal) > 1e-8 && faceTexelDensity > 0.0) {
            float3 tanW = normalize(mul(worldFromObj3x3, tanLocal));
            tanW = normalize(tanW - faceNormalW * dot(tanW, faceNormalW));  // Gram-Schmidt
            const float3 bitW  = cross(faceNormalW, tanW) * tan0.w;          // handedness
            const float3 viewW = normalize(-rayDir);
            const float  ndv   = clamp(abs(dot(faceNormalW, viewW)), 0.1, 1.0);
            float3 majorDir = viewW - faceNormalW * dot(viewW, faceNormalW); // in-plane view
            const float majLenSq = dot(majorDir, majorDir);
            majorDir = (majLenSq > 1e-8) ? majorDir * rsqrt(majLenSq) : tanW;
            const float3 minorDir = cross(faceNormalW, majorDir);
            const float3 majorVec = majorDir * (coneRadius / ndv);  // stretched
            const float3 minorVec = minorDir * coneRadius;
            // world → UV gradient: project onto tangent/bitangent, scale
            // by texels-per-world (density/objScale) and the material tile
            // factor. (matTileScale already folds the texture_scale tiling.)
            const float invWpu = matTileScale * faceTexelDensity / objToWorldScale;
            ddxUv = float2(dot(majorVec, tanW), dot(majorVec, bitW)) * invWpu;
            ddyUv = float2(dot(minorVec, tanW), dot(minorVec, bitW)) * invWpu;
        } else {
            ddxUv = float2(uvFootprint, 0.0);  // isotropic square footprint
            ddyUv = float2(0.0, uvFootprint);
        }
    }


    // Bindless baseColor sample, gated on the §11.6 HasBaseColorMap
    // flag the C++ side sets when the material's resolved
    // baseColorTex slot is valid. LOD computed via SampleByCone
    // (M9-fidelity ray-cone footprint) so distant surfaces use mip
    // levels instead of mip 0 (LOD-0-pinned aliasing was the dominant
    // texture artefact pre-M9-fidelity).
    // baseColor scalar carries the tint: for textured materials the C++
    // side sets it to diffuse_tint (default white); for untextured ones
    // it's diffuse_color_constant × diffuse_tint. The texture, when
    // present, is the albedo and the tint MULTIPLIES it — OmniPBR/MDL
    // convention. Without the multiply, a white felt weave texture
    // (felt_white_diff.jpg) under a green diffuse_tint rendered white/
    // grey instead of green.
    float3 baseColor = float3(mat.baseColorR, mat.baseColorG, mat.baseColorB);
    if ((mat.flags & MATERIAL_FLAG_HAS_BASE_COLOR_MAP) != 0u
        && mat.baseColorTex < BINDLESS_TEXTURES_CAP) {
        const float3 texSample = SampleByGrad(mat.baseColorTex, uv, ddxUv, ddyUv);
        // 2026-07-05 RTX-alignment — MDL OmniPBR / "Base" library
        // "Albedo Adjustments" (OmniPBR_ClearCoat.mdl: gain + lift
        // applied to the decoded texel in linear space, THEN a
        // desaturate-toward-luminance lerp, all BEFORE the diffuse_tint
        // multiply above). Every World Lobby "Base/*" material (Oak,
        // OakDark, Terrazzo, Paint_Satin) and several OmniPBR leaf
        // materials (the Modern reception "White" fixture,
        // albedo_brightness = 0.07) author non-default values here;
        // skipping this step was the dominant per-material albedo
        // brightness/saturation error measured against ovrtx (2-14x
        // too bright). Rec.709 luma weights match MDL's
        // base::mono_luminance. Defaults (brightness=1, add=0,
        // desaturation=0) make this an exact no-op.
        float3 graded = texSample * mat.albedoBrightness + mat.albedoAdd;
        const float mono = dot(graded, float3(0.2126, 0.7152, 0.0722));
        graded = lerp(graded, float3(mono, mono, mono), saturate(mat.albedoDesaturation));
        baseColor *= graded;
    }

    // 2026-07-06 RTX-alignment round 2 (materials chapter) — OmniPBR
    // `ao_to_diffuse`: blends the ORM texture's AO channel (.r) into
    // baseColor. Reuses whichever ORM-bound slot is valid
    // (roughnessTex/metallicTex both point at the same ORM texture
    // when OmniPBR's ORM packing is used — see TranslateMdl in
    // UsdShadeToOpenPBR.cpp) rather than adding a dedicated AO texture
    // slot. Gated on aoToDiffuse > 0 so the default (no-op) case skips
    // the extra sample; default 0 is the MDL function default, so
    // every material that doesn't author it — everything measured in
    // World Lobby — round-trips byte-identically.
    if (mat.aoToDiffuse > 0.0) {
        uint aoTex = BINDLESS_TEXTURES_CAP;
        if ((mat.flags & MATERIAL_FLAG_HAS_ROUGHNESS_MAP) != 0u
            && mat.roughnessTex < BINDLESS_TEXTURES_CAP) {
            aoTex = mat.roughnessTex;
        } else if ((mat.flags & MATERIAL_FLAG_HAS_METALLIC_MAP) != 0u
            && mat.metallicTex < BINDLESS_TEXTURES_CAP) {
            aoTex = mat.metallicTex;
        }
        if (aoTex < BINDLESS_TEXTURES_CAP) {
            const float aoSample = SampleByGradRGBA(aoTex, uv, ddxUv, ddyUv).r;
            baseColor = lerp(baseColor, baseColor * aoSample, saturate(mat.aoToDiffuse));
        }
    }

    // M9-fidelity: roughness + metallic from per-material textures.
    // Both default to the scalar in the material; per-texture sample
    // blends toward the scalar by the OmniPBR constant/texture
    // influence weight (PackMaterialGpu sets the matching
    // MATERIAL_FLAG_HAS_*_MAP bit when the texture handle is valid).
    // Roughness samples the .g channel (the GLTF / OpenPBR convention
    // for ORM packed maps); metallic samples .b. UsdPreviewSurface
    // authoring can split them across separate textures, in which
    // case the .g read still hits the data channel for a single-
    // channel roughness map.
    //
    // 2026-07-06 RTX-alignment round 2 — previously the sampled value
    // unconditionally won once a texture was bound (an implicit
    // influence=1). OmniPBR's `metallic_texture_influence` /
    // `reflection_roughness_texture_influence` lerp the constant
    // against the sample instead; the World Lobby's `rh_table_short`
    // "Paint_Satin" authors metallic_texture_influence=0 (a non-metal
    // satin paint that still binds an ORM texture for its roughness/AO
    // channels) — the old binary logic read the ORM's metal mask at
    // full strength regardless, rendering the tabletop chrome-white.
    // Defaults (1, 1) reproduce the old unconditional-override
    // behaviour exactly, so every other measured material is unchanged.
    float roughness = mat.roughness;
    float metalness = mat.metalness;
    if ((mat.flags & MATERIAL_FLAG_HAS_ROUGHNESS_MAP) != 0u
        && mat.roughnessTex < BINDLESS_TEXTURES_CAP) {
        const float roughnessSample = SampleByGradRGBA(mat.roughnessTex, uv, ddxUv, ddyUv).g;
        roughness = lerp(mat.roughness, roughnessSample, saturate(mat.reflectionRoughnessTextureInfluence));
    }
    if ((mat.flags & MATERIAL_FLAG_HAS_METALLIC_MAP) != 0u
        && mat.metallicTex < BINDLESS_TEXTURES_CAP) {
        const float metalnessSample = SampleByGradRGBA(mat.metallicTex, uv, ddxUv, ddyUv).b;
        metalness = lerp(mat.metalness, metalnessSample, saturate(mat.metallicTextureInfluence));
    }
    roughness = clamp(roughness, 0.04, 1.0);  // floor avoids singular GGX
    metalness = saturate(metalness);

    // M9 smooth shading: try the per-vertex normal path first. The
    // vertex-normal flat buffer is per-mesh-vertex (3 normals per
    // triangle vertex, addressed via the same gMeshIndices triple
    // we used for UVs above). Barycentric-interpolate, normalize.
    // Meshes that authored no normals contribute zero entries
    // (M9 StageWalker fills with float4(0,0,0,0)) — detected via the
    // squared magnitude of the interpolated vector. The M7 face-
    // normal path is the fallback so unauthored meshes still light.
    //
    // ObjectToWorld3x4() returns a column-major 3x4 affine; its
    // upper-3x3 transforms direction vectors. For a uniform-scale
    // + rotation transform this is correct as-is; non-uniform scale
    // needs the inverse-transpose of the upper 3x3 (M11 polish).
    const float3x3 worldFromObject3x3 = float3x3(
        objectToWorld[0].xyz,
        objectToWorld[1].xyz,
        objectToWorld[2].xyz);

    const uint vertexNormalBase = mi.vertexAttribOffset;
    const float3 nv0 = gMeshVertexAttribs[vertexNormalBase + v0].normal.xyz;
    const float3 nv1 = gMeshVertexAttribs[vertexNormalBase + v1].normal.xyz;
    const float3 nv2 = gMeshVertexAttribs[vertexNormalBase + v2].normal.xyz;
    const float3 smoothLocal = nv0 * w0 + nv1 * bary.x + nv2 * bary.y;
    const float  smoothLenSq = dot(smoothLocal, smoothLocal);

    float3 nLocal;
    if (smoothLenSq > 1e-6) {
        nLocal = smoothLocal * rsqrt(smoothLenSq);
    } else {
        // Mesh authored no normals — fall back to face normal.
        const uint faceOffset = mi.faceOffset;
        nLocal = gMeshFaceNormals[faceOffset + primitiveIndex].xyz;
    }
    float3 nWorld = normalize(mul(worldFromObject3x3, nLocal));

    // Face-forward the shading normal: flip it to the side the ray
    // arrived from. Interior environments (a room / lobby modeled as a
    // box) author OUTWARD-pointing wall normals, so the visible inner
    // face — the one a primary ray hits from inside the room — has a
    // normal pointing AWAY from the eye. Without this flip, every
    // light's NdotL goes negative → clamps to 0 → the wall renders
    // black despite a perfectly-loaded baseColor texture. (This was the
    // "black Oak paneling" symptom: the baseColor AOV showed correct
    // wood grain, but the lit pass zeroed it because the wall normals
    // pointed into the wall.) Standard ray-tracer "facing normal"
    // convention — correct for single-sided geometry viewed from either
    // side and for double-sided materials alike. rayDir (the TraceRay
    // direction) points FROM eye TO surface, so a positive dot means
    // the normal shares the ray's direction (faces away from the eye)
    // → flip.
    if (dot(nWorld, rayDir) > 0.0)
        nWorld = -nWorld;

    // Keep the face-forward GEOMETRIC normal around: the normal-mapped
    // shading normal below can tilt past the view horizon (strong
    // bump_factor, grazing angles), which would send the mirror-
    // reflection bounce ray INTO the surface → the reflection reads as
    // black. We re-clamp the shading normal against this geometric
    // reference after normal mapping. (This was the "normal map present
    // → no reflection, remove it → reflection returns" symptom.)
    const float3 geoNormal = nWorld;

    // M9 normal mapping. When the material binds a normal texture,
    // sample it in tangent space and transform to world via the TBN
    // matrix built from the per-vertex tangent (MikkTSpace) + the
    // per-vertex normal we just computed + sign-bit-derived bitangent
    // (`bitangent = sign × cross(N, T)`). Tangents stored as float4:
    // xyz = unit tangent in object space, w = handedness sign.
    //
    // Three guards skip the TBN sample (shading keeps the
    // smooth-interp normal as the shading normal):
    //   1. Material doesn't flag HAS_NORMAL_MAP (no normal texture).
    //   2. mat.normalTex is the INVALID_BINDLESS_TEXTURE sentinel.
    //   3. Per-vertex tangent magnitude is zero (mesh authored no
    //      tangents — typical for cube fixtures + meshes without
    //      UVs / normals where MikkTSpace can't generate them).
    if ((mat.flags & MATERIAL_FLAG_HAS_NORMAL_MAP) != 0u
        && mat.normalTex < BINDLESS_TEXTURES_CAP) {
        const uint   tangentBase = mi.vertexAttribOffset;
        const float4 tv0 = gMeshVertexAttribs[tangentBase + v0].tangent;
        const float4 tv1 = gMeshVertexAttribs[tangentBase + v1].tangent;
        const float4 tv2 = gMeshVertexAttribs[tangentBase + v2].tangent;
        const float3 tangentLocal = tv0.xyz * w0 + tv1.xyz * bary.x + tv2.xyz * bary.y;
        const float  tangentLenSq = dot(tangentLocal, tangentLocal);
        if (tangentLenSq > 1e-6) {
            // Per-MikkTSpace doc: averaging tangent.w across vertices
            // is invalid (handedness shouldn't lerp), so take the
            // first vertex's sign — same lossy-but-consistent
            // collapse used for the per-vertex tangent slot itself.
            const float  handedness = tv0.w;
            const float3 tangentWorld_raw =
                normalize(mul(worldFromObject3x3, tangentLocal * rsqrt(tangentLenSq)));
            // Gram-Schmidt re-orthonormalise T against N (cheap
            // safety net — interp may have de-orthogonalised).
            const float3 tangentWorld =
                normalize(tangentWorld_raw - dot(tangentWorld_raw, nWorld) * nWorld);
            const float3 bitangentWorld = cross(nWorld, tangentWorld) * handedness;

            // Sample the tangent-space normal map and unpack XY to
            // [-1,1]. **Z is reconstructed from XY**, not read from the
            // blue channel: the normal map is BC5-compressed when
            // texture compression is on (BC5 stores only R+G), so the
            // sampled .b is 0 and using it would point the normal
            // backward (z = 0*2-1 = -1) — which the shading-normal clamp
            // then discards, making the normal map silently do NOTHING.
            // Reconstructing z = sqrt(1 - x² - y²) is exact for unit
            // tangent-space normals and works for BC5 + uncompressed
            // alike. (This was the "normal map has no effect" symptom
            // once --compress-textures defaulted on.)
            const float3 nSample = SampleByGrad(mat.normalTex, uv, ddxUv, ddyUv);
            float2 nXY = nSample.xy * 2.0 - 1.0;

            // V2.A.24 — tangent-handedness flips (OmniPBR
            // flip_tangent_u / flip_tangent_v). The DirectX-vs-OpenGL
            // green-channel convention; Omniverse content commonly
            // authors flip_tangent_v = 1, so without flipping the V
            // (green) channel the bump lighting is inverted.
            if ((mat.flags & MATERIAL_FLAG_FLIP_TANGENT_U) != 0u)
                nXY.x = -nXY.x;
            if ((mat.flags & MATERIAL_FLAG_FLIP_TANGENT_V) != 0u)
                nXY.y = -nXY.y;

            // V2.A.23 — normal-map strength (OmniPBR bump_factor): scale
            // the tangent-space deviation, then reconstruct Z from the
            // scaled XY so a stronger bump tilts the normal more.
            nXY *= mat.normalStrength;
            const float nZ = sqrt(saturate(1.0 - dot(nXY, nXY)));
            float3 nTangent = normalize(float3(nXY, nZ));

            // Transform tangent-space normal to world space.
            nWorld = normalize(nTangent.x * tangentWorld
                             + nTangent.y * bitangentWorld
                             + nTangent.z * nWorld);
        }
    }

    // Shading-normal clamp against the geometric normal. A strong
    // bump_factor (or a grazing view) can tilt the normal-mapped
    // `nWorld` past the view horizon — i.e. it ends up pointing away
    // from the eye. Left unclamped, the mirror reflection's
    // `reflect(rayDir, nWorld)` then bounces INTO the surface and the
    // reflection reads as black, which is why a glossy material showed
    // reflections with its normal map removed but none with it on. We
    // bend the shading normal back just enough to keep the
    // reflected view vector on the geometric surface's visible side
    // (Frostbite "shading normal adaptation", simplified): when the
    // reflection would go below the geometric horizon, project the
    // shading normal onto the safe hemisphere. Cheap + deterministic.
    {
        const float3 viewW = normalize(-rayDir);
        const float vdotn = dot(viewW, nWorld);
        if (vdotn < 1e-3) {
            // Reflect the shading normal back across the view-aligned
            // plane so V·N becomes non-negative, preserving the bump's
            // tangential character while keeping the bounce above the
            // surface. Falls back to the geometric normal if the
            // perturbation was extreme.
            const float3 corrected = normalize(nWorld + viewW * (1e-3 - vdotn));
            nWorld = (dot(corrected, geoNormal) > 0.0) ? corrected : geoNormal;
        }
    }

    // ---- Q2 OpenPBR-complete: gather the material parameter set ONCE,
    // post-texturing (rows 0-14 of OpenPBRMaterialGPU; see the row map
    // in ShaderInterop.slang). ALL BSDF math lives in
    // openpbr_material.slang — this kernel only orchestrates.
    //
    // Anisotropy tangent frame (gate ANISOTROPY): the MikkTSpace
    // per-vertex tangent, orthonormalized against the FINAL shading
    // normal (post normal-map) so the stretched NDF stays in the
    // shading tangent plane. Zero when the mesh authored no tangents —
    // the closure then takes the spec-endorsed isotropic α = r² path.
    float3 anisoTangentW   = float3(0.0, 0.0, 0.0);
    float3 anisoBitangentW = float3(0.0, 0.0, 0.0);
    {
        const uint   atBase = mi.vertexAttribOffset;
        const float4 atv0 = gMeshVertexAttribs[atBase + v0].tangent;
        const float3 atLocal = atv0.xyz * w0
                             + gMeshVertexAttribs[atBase + v1].tangent.xyz * bary.x
                             + gMeshVertexAttribs[atBase + v2].tangent.xyz * bary.y;
        const float atLenSq = dot(atLocal, atLocal);
        if (atLenSq > 1e-6) {
            const float3 atWorldRaw =
                normalize(mul(worldFromObject3x3, atLocal * rsqrt(atLenSq)));
            const float3 atOrtho = atWorldRaw - dot(atWorldRaw, nWorld) * nWorld;
            const float atOrthoLenSq = dot(atOrtho, atOrtho);
            if (atOrthoLenSq > 1e-8) {
                anisoTangentW   = atOrtho * rsqrt(atOrthoLenSq);
                // First-vertex handedness — same lossy-but-consistent
                // collapse the normal-map TBN uses (MikkTSpace doc:
                // never interpolate the sign).
                anisoBitangentW = cross(nWorld, anisoTangentW) * atv0.w;
            }
        }
    }

    OpenPBRSurfaceParams pbr;
    pbr.baseColor = baseColor;
    pbr.baseWeight = saturate(mat.baseWeight);
    pbr.baseMetalness = metalness;
    pbr.baseDiffuseRoughness = saturate(mat.baseDiffuseRoughness);
    pbr.specularWeight = max(0.0, mat.specularWeight);   // spec range [0, ∞)
    pbr.specularColor = saturate(float3(mat.specularColorR, mat.specularColorG,
                                        mat.specularColorB));
    pbr.specularRoughness = roughness;                   // post-texture, [0.04, 1]
    pbr.specularRoughnessAnisotropy = saturate(mat.specularRoughnessAnisotropy);
    pbr.specularIor = max(1e-2, mat.specularIor);
    pbr.transmissionWeight = saturate(mat.transmissionWeight);
    pbr.transmissionColor = saturate(float3(mat.transmissionColorR,
                                            mat.transmissionColorG,
                                            mat.transmissionColorB));
    pbr.subsurfaceWeight = saturate(mat.subsurfaceWeight);
    pbr.subsurfaceColor = saturate(float3(mat.subsurfaceColorR, mat.subsurfaceColorG,
                                          mat.subsurfaceColorB));
    pbr.coatWeight = saturate(mat.coatWeight);
    pbr.coatColor = saturate(float3(mat.coatColorR, mat.coatColorG, mat.coatColorB));
    pbr.coatRoughness = saturate(mat.coatRoughness);
    pbr.coatIor = max(1e-2, mat.coatIor);
    pbr.coatDarkening = saturate(mat.coatDarkening);
    pbr.fuzzWeight = saturate(mat.fuzzWeight);
    pbr.fuzzColor = saturate(float3(mat.fuzzColorR, mat.fuzzColorG, mat.fuzzColorB));
    pbr.fuzzRoughness = saturate(mat.fuzzRoughness);
    pbr.geometryOpacity = saturate(mat.opacity);
    pbr.thinWalled = (mat.flags & MATERIAL_FLAG_THIN_WALLED) != 0u;
    pbr.tangentWorld = anisoTangentW;
    pbr.bitangentWorld = anisoBitangentW;

    // M9 emission. Emissive materials (UsdPreviewSurface authored
    // `emissiveColor` non-zero, MaterialFlag::EMISSIVE bit set by
    // PackMaterialGpu) add their authored radiance directly to the
    // output — independent of NdotL, distance falloff, or any
    // incoming light. Per-material emissionTex (when authored)
    // multiplies the constant emissionColor pointwise; no UV-set
    // indirection beyond what the baseColor sample already did.
    // Hoisted above the (lighting-only) caller's light loop — WP2-core
    // moved this block up from its pre-split position (it never
    // depended on the light loop, only on `mat` / `uv` / `ddxUv` /
    // `ddyUv`, all already in scope) so EvaluateSurfaceMaterial can
    // return a complete `pbr` (with `emission` filled) to BOTH callers.
    float3 emissionRaw = float3(0.0, 0.0, 0.0);
    if ((mat.flags & MATERIAL_FLAG_EMISSIVE) != 0u) {
        emissionRaw = float3(mat.emissionR, mat.emissionG, mat.emissionB)
                      * mat.emissionLuminance;
        if ((mat.flags & MATERIAL_FLAG_HAS_EMISSION_MAP) != 0u
            && mat.emissionTex < BINDLESS_TEXTURES_CAP) {
            const float3 sampledEmission =
                SampleByGrad(mat.emissionTex, uv, ddxUv, ddyUv);
            emissionRaw *= sampledEmission;
        }
    }
    pbr.emission = emissionRaw;

    SurfaceMaterialEval result;
    result.pbr = pbr;
    result.baseColor = baseColor;
    result.normalWorld = nWorld;
    result.materialId = materialId;
    result.opacity = mat.opacity;
    return result;
}

// Shade one surface hit — calls EvaluateSurfaceMaterial above (bit-
// identical material/geometry eval, shared with RaytracedGBufferPass's
// raygen), then runs the per-light loop + shadow rays + deterministic
// mirror reflection + front-to-back composite exactly as the pre-split
// closesthit did. `recursionDepth` gates the reflection branch's ray
// budget (V2.B) — the ONE input EvaluateSurfaceMaterial doesn't need
// (it does no tracing).
SurfaceShadeResult ShadeSurfaceHit(uint instanceId, uint primitiveIndex, float2 bary,
                                   float3 worldHit, float3 rayDir, float hitT,
                                   float3x4 objectToWorld, float3x4 worldToObject,
                                   uint recursionDepth) {
    const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
        instanceId, primitiveIndex, bary, worldHit, rayDir, hitT, objectToWorld, worldToObject);
    OpenPBRSurfaceParams pbr = surf.pbr;
    float3 baseColor          = surf.baseColor;
    float3 nWorld             = surf.normalWorld;
    const uint materialId     = surf.materialId;
    const float opacity       = surf.opacity;
    // Q2 — the boxed closure's EDF row: emission sits BELOW the coat, so
    // the coat absorbs it (L_e = lerp(1, T_coat, C)·E). See
    // EvaluateSurfaceMaterial above for where `pbr.emission` is filled.
    const float3 emission = OpenPBREmission(pbr);

    // M7-simple lighting + NdotL Lambert for direct lights. Loop
    // over every authored light, accumulate the per-light Lambert
    // contribution. The no-lights case (RaytracedLightingPass binds a
    // single intensity=0 sentinel) leaves `directSum` zero → falls
    // through to baseColor-only emission, preserving M5/M6 byte-equal.
    //
    // M8a — physical light/exposure pairing (Omniverse RTX convention,
    // shared by RenderMan / Karma / Storm / Cycles). USD authors
    // intensities in physical units (60000 nits for a bright DiskLight;
    // 12000 for a sky dome) and pairs them with heavily negative
    // UsdGeomCamera::exposure stops (e.g. -10 = 1/1024×) to compress
    // back to display range. Two pieces are needed for that pairing
    // to read correctly:
    //   1. Distance attenuation (1/r²) on every non-Distant, non-Dome
    //      light. Without it a 60000-intensity light at 30m reads as
    //      bright as the same light at 1m, blowing out the whole scene
    //      no matter what camera exposure compensates with.
    //   2. The camera's 2^exposure factor applied in the TonemapPass,
    //      which we already do (TonemapUniforms.exposure × ACES tonemap).
    //
    // - Distant: NO falloff (parallel rays, infinitely far). lightDir
    //   = -L.direction (the world-space direction the light's photons
    //   travel; NdotL uses the negated form pointing FROM surface TO
    //   light source).
    // - Rect / Disk / Sphere / Cylinder: 1/r² falloff from the light's
    //   centre. lightDir = normalize(L.position - hitWorld). The
    //   distance² is clamped to 0.01 m² (= 10cm minimum apparent
    //   distance) to avoid the singularity for surfaces directly on a
    //   light. Treating area lights as a point at centre is M7-simple;
    //   real area sampling lands in M7-full's NEE pass.
    // - Dome: uniform ambient (no NdotL — hemispheric integration
    //   is what the user fills in at M7-full alongside the full BSDF).
    //   No distance falloff — dome is at infinity.
    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    // Q2: one accumulator per transport path — the per-light closure
    // (EvaluateOpenPBRDirect folds diffuse + specular) and the dome
    // ambient (EvaluateOpenPBRAmbient folds the energy chain that used
    // to live in the final composite: baseColor × (1 − metalness)).
    float3 directLight = float3(0.0, 0.0, 0.0);
    float3 ambientSum  = float3(0.0, 0.0, 0.0);
    float  totalContribution = 0.0;
    const float3 hitWorld = worldHit;
    // M9-fidelity: view direction (FROM surface TO eye) for the GGX
    // microfacet half-vector + Schlick Fresnel.
    const float3 viewDir = normalize(-rayDir);
    for (uint i = 0; i < lightCount; ++i) {
        LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;  // sentinel / disabled
        const float3 Lcol = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
        if (L.kind == LIGHT_KIND_DOME) {
            // V2.B follow-up — deterministic dome irradiance via the
            // "fast diffuse IBL" trick: sample the dome HDRI at the
            // surface normal direction with a high mip bias. Mip 6 of
            // an 8K HDRI ≈ 128×64 — heavily box-filtered, approximates
            // the cosine-weighted hemisphere convolution the proper
            // irradiance integral
            //   E(N) = ∫_hemisphere(N) radiance(ω) · max(0, N·ω) dω
            // computes exactly. Trades exactness for one texture
            // sample per pixel, no RNG, no noise — fits the
            // deterministic-raytracer doctrine. Captures the dome
            // HDRI's directional structure (warm at horizon, cool at
            // zenith, etc.) so vertical surfaces get the side of the
            // dome they're facing instead of the previous zero
            // contribution from `dot(N, up)`.
            //
            // The 1/π factor is the Lambertian BRDF normalization:
            // the final composition below multiplies `ambientSum` by
            // `baseColor`, which together form the diffuse BRDF
            // `baseColor / π` × irradiance.
            //
            // Dome rotation: mirror the miss shader's inverse Y-rot
            // so a tinted/rotated dome's structure lines up between
            // the background pixels and the lit-surface samples.
            float3 sampleDir = nWorld;
            if (abs(L.domeRotationYRadians) > 1e-4) {
                const float cosR = cos(-L.domeRotationYRadians);
                const float sinR = sin(-L.domeRotationYRadians);
                sampleDir = float3(cosR * nWorld.x + sinR * nWorld.z,
                                    nWorld.y,
                                    -sinR * nWorld.x + cosR * nWorld.z);
            }
            const float u = atan2(sampleDir.x, sampleDir.z) / PYXIS_TWO_PI + 0.5;
            const float v = acos(clamp(sampleDir.y, -1.0, 1.0)) / PYXIS_PI;
            // Mip-bias for the diffuse-IBL approximation. We'd like
            // mip 6 (~128×64 for an 8K HDRI), but the 1×1 fallback
            // texture only has mip 0 — sampling beyond the last mip
            // level is undefined behavior in Vulkan (different bytes
            // per frame, breaks the deterministic-raytracer contract).
            // Query the texture's actual mip count and clamp.
            float domeWidth;
            float domeHeight;
            float domeMipLevels;
            gDomeEnvMap.GetDimensions(0, domeWidth, domeHeight, domeMipLevels);
            const float targetMip = min(6.0, max(0.0, domeMipLevels - 1.0));
            const float3 domeRadiance =
                gDomeEnvMap.SampleLevel(gDomeSampler, float2(u, v), targetMip).rgb;

            // Dome shadow + ambient occlusion. Shadow ray in the
            // surface's "up" direction (the normal) with a short TMax
            // (2 m world-space, matching the renderer's metres
            // convention): nearby occluders darken the dome
            // contribution; distant ceilings don't (so an indoor lobby
            // doesn't go uniformly black under a high ceiling). The
            // single ray gives a one-tap hemispheric occlusion estimate
            // — darkens corners and surfaces tucked under furniture,
            // standard "deterministic AO from sky" Whitted-style.
            // Shadow_miss writes payload.color = 1 when the ray
            // misses (sky visible); the pre-trace color stays 0 if it
            // hits (occluded). All deterministic — no RNG, fixed
            // direction per pixel.
            // Recursion-budget gate: this AO ray is a TraceRay, so it
            // must respect the pipeline maxRecursionDepth (=3) just like
            // the reflection/continuation rays. Without it, a surface
            // already deep in the chain (e.g. seen via a glass
            // transmission ray, then reflected) would issue a TraceRay
            // past the cap — undefined behavior that returned garbage and
            // made reflections vanish behind translucent surfaces. When
            // out of budget, assume unoccluded (visibility 1).
            float aoVisibility = 1.0;
            if (recursionDepth + 1u < MAX_RAY_RECURSION)
            {
                RayDesc aoRay;
                aoRay.Origin    = hitWorld + nWorld * 1e-3;
                aoRay.Direction = nWorld;
                aoRay.TMin      = 1e-3;
                aoRay.TMax      = 2.0;  // 2 m AO range
                HitInfo aoPayload;
                aoPayload.color           = float3(0.0, 0.0, 0.0);
                aoPayload.hitT            = -1.0;
                aoPayload.normalWorld     = float3(0.0, 0.0, 0.0);
                aoPayload.instanceId      = 0xFFFFFFFFu;
                aoPayload.baseColor       = float3(0.0, 0.0, 0.0);
                aoPayload.materialId      = 0xFFFFFFFFu;
                aoPayload.worldHit        = float3(0.0, 0.0, 0.0);
                aoPayload.elementId       = 0xFFFFFFFFu;
                aoPayload.recursionDepth  = recursionDepth + 1u;
                aoPayload.transmittance   = float3(0.0, 0.0, 0.0);
                TraceRay(gTlas,
                         RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                           | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         0xFFu,
                         /*RayContributionToHitGroupIndex*/ 0,
                         /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                         /*MissShaderIndex*/ 1,
                         aoRay,
                         aoPayload);
                aoVisibility = aoPayload.color.x;
            }

            // Q2: the dome-diffuse response (Lambert 1/π, the
            // (1 − metalness) gate, EON ambient albedo, dielectric
            // albedo-scaling, coat absorption/darkening, fuzz layer)
            // lives in the closure module now. AO stays a multiplier
            // computed here (TraceRay belongs to this kernel).
            ambientSum += EvaluateOpenPBRAmbient(pbr, nWorld, viewDir,
                                                 Lcol * domeRadiance, aoVisibility);
            totalContribution += L.intensity;
            continue;
        }
        float3 lightDir;
        float falloff = 1.0;  // Distant defaults to no attenuation
        if (L.kind == LIGHT_KIND_DISTANT) {
            // Parallel rays from an infinitely-far source — no falloff.
            lightDir = -float3(L.dirX, L.dirY, L.dirZ);
        } else {
            // PR5 — every position-based kind (Rect, Cylinder, Geometry,
            // Portal + future Disk / Sphere when added) shares this path:
            // point at `L.position` with 1/r² falloff. Per-shape area
            // sampling is M7-full NEE; v2.0 treats area lights as a
            // point at their centroid (for GeometryLight this is the
            // emitter's AABB centroid resolved at ingest — see
            // EmitLight's V2.A.19 / PR5 branch).
            const float3 toLight = float3(L.posX, L.posY, L.posZ) - hitWorld;
            const float  distSq  = dot(toLight, toLight);
            // Soft minimum on r² avoids the 1/0 singularity for
            // surfaces touching a light. 0.01 m² ≈ 10cm — closer than
            // that, the falloff caps at 100×.
            falloff  = 1.0 / max(distSq, 0.01);
            lightDir = toLight * rsqrt(max(distSq, 1e-12));
        }
        const float NdotL = max(0.0, dot(nWorld, lightDir));

        // M9-fidelity UsdLuxShapingAPI cone. cosOuter == 0 → no cone,
        // skip. Cone axis derives per-kind: Distant lights point along
        // L.dir (the photons-travel direction = AWAY from the
        // illuminated surface), so the spotlight axis FROM light TO
        // surface is +L.dir. Rect/Disk/Sphere "face" the rect normal
        // = normalize(cross(axisU, axisV)); the spotlight axis from
        // light TO surface is -that-normal (the rect emits toward its
        // -Z, away from the front face).
        float coneAttenuation = 1.0;
        if (L.shapingConeCosOuter > 1e-4) {
            float3 coneAxis;
            if (L.kind == LIGHT_KIND_RECT) {
                // Rects "face" their plane normal = cross(U, V).
                const float3 axisU = float3(L.axisUx, L.axisUy, L.axisUz);
                const float3 axisV = float3(L.axisVx, L.axisVy, L.axisVz);
                coneAxis = normalize(cross(axisU, axisV));
            } else {
                // Distant + the other position-based kinds (Cylinder,
                // Geometry, Portal) all carry their direction in L.dir.
                // GeometryLight's dir stays at the LightDesc default
                // (0,-1,0) so the cone gate is effectively a no-op
                // there — fine, since v2.0 GeometryLight doesn't
                // author UsdLuxShapingAPI on the typical fixture.
                coneAxis = float3(L.dirX, L.dirY, L.dirZ);
            }
            // Vector FROM light TO surface. Distant treats L.dir as the
            // axis it's projecting along; the position-based kinds
            // derive it from -lightDir (lightDir points FROM surface TO
            // light, so its negation points FROM light TO surface).
            const float3 lightToSurface = (L.kind == LIGHT_KIND_DISTANT)
                                              ? float3(L.dirX, L.dirY, L.dirZ)
                                              : -lightDir;
            const float cosAngle = dot(normalize(lightToSurface), coneAxis);
            coneAttenuation = smoothstep(L.shapingConeCosOuter,
                                         L.shapingConeCosInner,
                                         cosAngle);
        }

        // M9-fidelity shadow ray. Skip when the surface is back-
        // facing the light (NdotL <= 0 contributes nothing anyway)
        // or when the cone attenuation has zeroed the contribution
        // — both cases would multiply the result by 0 below, so the
        // TraceRay would be wasted bandwidth.
        //
        // Ray-flag combo: SKIP_CLOSEST_HIT_SHADER (we don't want to
        // recursively shade — that would also blow past
        // maxRecursionDepth=2) + ACCEPT_FIRST_HIT_AND_END_SEARCH
        // (any opaque blocker is enough — no need to keep traversing
        // for the closest one). The hit-group's anyhit still fires
        // and may IgnoreHit() for translucent materials (M9 stub).
        //
        // Origin offset: push the ray start along the normal by
        // 1e-3 to avoid self-intersection from the surface that
        // generated this shading invocation. TMin also takes
        // the same eps as a belt-and-braces measure.
        // TMax: for finite-distance lights, cap at the distance to
        // the light's centre so geometry behind the light doesn't
        // shadow it; Distant lights use 1e30 (effectively infinity).
        // miss-shader-index = 1 (= ShadowMissMain) writes
        // payload.color = (1,1,1) iff the path is unoccluded.
        // The shadow ray is a TraceRay → gate on the same recursion
        // budget as AO/reflection/continuation (pipeline maxRecursionDepth
        // = 3). A surface reached through a glass transmission ray then
        // reflected sits at the depth limit; firing a shadow ray there
        // would exceed the cap (undefined behavior). Out of budget →
        // assume lit (visibility 1).
        float visibility = 1.0;
        if (NdotL > 0.0 && coneAttenuation > 0.0
            && recursionDepth + 1u < MAX_RAY_RECURSION) {
            float lightDist = 1e30;
            // Position-based kinds cap the shadow ray at the distance
            // to the light's centre. Distant lights stay at 1e30
            // (effectively infinite). Symmetric with the falloff
            // branch above — both gates fire on `kind != DISTANT`.
            if (L.kind != LIGHT_KIND_DISTANT) {
                const float3 toLight = float3(L.posX, L.posY, L.posZ) - hitWorld;
                lightDist = length(toLight);
            }
            RayDesc shadowRay;
            shadowRay.Origin    = hitWorld + nWorld * 1e-3;
            shadowRay.Direction = lightDir;
            shadowRay.TMin      = 1e-3;
            shadowRay.TMax      = max(1e-3, lightDist - 1e-3);
            HitInfo shadowPayload;
            shadowPayload.color           = float3(0.0, 0.0, 0.0);  // pre-trace = occluded
            shadowPayload.hitT            = -1.0;
            shadowPayload.normalWorld     = float3(0.0, 0.0, 0.0);
            shadowPayload.instanceId      = 0xFFFFFFFFu;
            shadowPayload.baseColor       = float3(0.0, 0.0, 0.0);
            shadowPayload.materialId      = 0xFFFFFFFFu;
            shadowPayload.worldHit        = float3(0.0, 0.0, 0.0);
            shadowPayload.elementId       = 0xFFFFFFFFu;
            shadowPayload.recursionDepth  = recursionDepth + 1;  // M29
            shadowPayload.transmittance   = float3(0.0, 0.0, 0.0);
            TraceRay(gTlas,
                     RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                       | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                     0xFFu,
                     /*RayContributionToHitGroupIndex*/ 0,
                     /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                     /*MissShaderIndex*/ 1,
                     shadowRay,
                     shadowPayload);
            visibility = shadowPayload.color.x;
        }

        // Q2 — the complete OpenPBR closure per light (EON diffuse +
        // exact-Fresnel dielectric / F82 metal GGX + coat + fuzz +
        // subsurface/transmission energy accounting), evaluated in
        // openpbr_material.slang. Returns BSDF × NdotL × lightColor;
        // the scalar attenuation chain (1/r² falloff, shaping cone,
        // shadow visibility) stays here, exactly as the pre-Q2 split.
        const float3 lightContribution =
            EvaluateOpenPBRDirect(pbr, nWorld, viewDir, lightDir, Lcol);
        directLight += lightContribution * (falloff * coneAttenuation * visibility);
        // pre-NdotL, pre-falloff totalContribution drives only the
        // "any-lights / fall through to emissive baseColor" sentinel
        // below. Adding the raw intensity (rather than the attenuated
        // value) keeps that test stable regardless of distance —
        // otherwise a far-away scene with no nearby lights would
        // collapse to baseColor-only emission and lose dome/ambient.
        totalContribution += L.intensity;
    }

    // Q2 composite: EvaluateOpenPBRDirect folds diffuse + specular per
    // light (with the spec's energy accounting), EvaluateOpenPBRAmbient
    // folds the dome chain (incl. the historical baseColor ×
    // (1 − metalness) gate). The no-lights case falls through to raw
    // baseColor — preserves the M5/M6 byte-equal unlit fixtures.
    float3 lit = (totalContribution > 0.0)
        ? (directLight + ambientSum)
        : baseColor;

    // V2.B.1 — inline RT specular reflection. **Deterministic mirror
    // direction**, no Monte Carlo sampling. Per plan_for_v2.md Pillar
    // B: "1-bounce ray-traced specular reflections … no denoiser, no
    // TAA. Roughness-blended fallback to dome envmap on rough surfaces
    // keeps reflections clean without ML denoising." This is the
    // smallest step toward that target: the bounce ray always points
    // at the geometric mirror direction, so the reflection is
    // pixel-stable + byte-identical across re-renders. Rough metals
    // currently see a chrome-like sharp reflection; the
    // roughness-mip envmap blend that softens that into the
    // plan-prescribed "blurred envmap on rough surfaces" is the
    // follow-up.
    //
    // Conditions:
    //  * material is metallic-ish OR roughness is low enough that a
    //    specular contribution is meaningful (roughness < 0.5).
    //  * recursion depth headroom remains (RT pipeline maxRecursion
    //    is 2 in the §M29 budget; +1 keeps us under the cap).
    //
    // What was here before (and why it's gone): M30 / V2.B.1's first
    // cut sampled a microfacet half-vector via Smith-GGX VNDF (Heitz
    // 2018) using two per-pixel pseudo-random scalars and reflected
    // V around that. Mathematically a one-sample Monte Carlo estimate
    // of the GGX-cone integral — visually a noisy speckle on rough
    // surfaces because the integral hadn't been averaged. v2's
    // pipeline doctrine is "no noise, no denoiser, no accumulator";
    // mirror direction + future envmap blur is the noise-free
    // replacement.
    // Reflection is captured SEPARATELY (not folded into `lit`) so the
    // translucent composite below can layer it as a Fresnel term that
    // survives even when opacity → 0. Folding it into `lit` and then
    // scaling by opacity (the old code) deleted the reflection on glass
    // and any see-through surface.
    float3 reflColorOut  = float3(0.0, 0.0, 0.0);
    float3 reflWeightOut = float3(0.0, 0.0, 0.0);
    if (recursionDepth + 1u < MAX_RAY_RECURSION
        && (pbr.baseMetalness > 0.05 || pbr.specularRoughness < 0.5))
    {
        // Geometric mirror direction. rayDir (the TraceRay direction)
        // is FROM eye TO surface; `reflect(d, n)` returns the mirror
        // across `n` (which is the surface normal). For roughness > 0
        // we keep the mirror direction unchanged today — the
        // cone-spreading via envmap-mip blur lands in the follow-up.
        // Self-intersection is avoided by pushing the origin out by
        // 1e-3 along the shading normal, same epsilon shadow rays use
        // above.
        const float3 reflDir = normalize(reflect(rayDir, nWorld));

        RayDesc reflRay;
        reflRay.Origin    = hitWorld + nWorld * 1e-3;
        reflRay.Direction = reflDir;
        reflRay.TMin      = 1e-3;
        reflRay.TMax      = 10000.0;

        // Front-to-back transmission composite of the reflected ray.
        // Without this, the reflection reads only the frontmost reflected
        // surface's emitted radiance: a translucent surface (glass) seen
        // IN a mirror returns its near-black emitted term and drops
        // everything BEHIND it, so glass-in-a-mirror rendered black. This
        // mirrors the lighting raygen's primary transmission loop
        // (raytraced_lighting_raygen.slang) but runs at the reflection
        // ray's recursion depth. Each segment re-traces past the previous
        // translucent hit by LOOP iteration (not hardware recursion), so
        // the §V2.B depth cap of 3 is untouched; stacked transparent
        // layers in a reflection composite exactly as they do for the
        // primary ray. 0.999 / 1e-3 thresholds match the primary loop and
        // the anyhit translucency gate. REFL_MAX_SEGMENTS is the P5
        // module-scope specialization constant (declared next to
        // MAX_RAY_RECURSION above; value unchanged at 16).
        float3 reflRadiance   = float3(0.0, 0.0, 0.0);
        float3 reflThroughput = float3(1.0, 1.0, 1.0);
        [loop]
        for (uint reflSeg = 0u; reflSeg < REFL_MAX_SEGMENTS; ++reflSeg)
        {
            HitInfo reflPayload;
            reflPayload.color           = float3(0.0, 0.0, 0.0);
            reflPayload.hitT            = -1.0;
            reflPayload.normalWorld     = float3(0.0, 0.0, 0.0);
            reflPayload.instanceId      = 0xFFFFFFFFu;
            reflPayload.baseColor       = float3(0.0, 0.0, 0.0);
            reflPayload.materialId      = 0xFFFFFFFFu;
            reflPayload.worldHit        = float3(0.0, 0.0, 0.0);
            reflPayload.elementId       = 0xFFFFFFFFu;
            reflPayload.recursionDepth  = recursionDepth + 1u;
            reflPayload.transmittance   = float3(0.0, 0.0, 0.0);

            TraceRay(gTlas, RAY_FLAG_NONE, 0xFFu, 0, 0, 0, reflRay, reflPayload);

            reflRadiance += reflThroughput * reflPayload.color;

            // Miss (envmap already in color) or opaque hit: nothing more
            // lies behind this surface in the reflection.
            if (reflPayload.hitT < 0.0)
                break;
            const float3 reflT = reflPayload.transmittance;
            if (max(max(reflT.x, reflT.y), reflT.z) < 1e-3)
                break;
            reflThroughput *= reflT;

            // Advance just past this translucent hit and re-trace what
            // lies behind it along the same reflected direction.
            const float3 reflHitPos =
                reflRay.Origin + reflRay.Direction * reflPayload.hitT;
            reflRay.Origin = reflHitPos + reflRay.Direction * 1e-3;
            reflRay.TMin   = 1e-3;
        }

        // Q2 — the deterministic mirror WEIGHT comes from the closure
        // module: dielectric exact-Fresnel (modulated-IOR η′ chain,
        // specular_color top-side tint) blended with the metal F82 by
        // metalness, × the (1 − r′)² glossy fade now evaluated at the
        // COAT-ROUGHENED r′, with the coat's own mirror lobe layered on
        // top and the fuzz throughput over everything. Fresnel is
        // evaluated at the surface normal (H = N → HdotV = NdotV) —
        // exact for smooth surfaces, the same deterministic
        // approximation as before for rough ones until the envmap blur
        // lands. The glossy fade keeps its pre-Q2 rationale: the bounce
        // ray is a single sharp mirror sample (no stochastic cone, per
        // the no-noise doctrine), so rough surfaces fade the mirror
        // instead of showing a crisp — and physically wrong — image.
        reflWeightOut = OpenPBRReflectionWeight(pbr, nWorld, viewDir);
        reflColorOut  = reflRadiance;
    }

    // V2.B — layered glass/translucency composite. Two transport paths
    // stay SEPARATE so a transparent surface still reflects:
    //   * reflection: a Fresnel-weighted term (reflWeightOut) layered ON
    //     TOP, independent of opacity.
    //   * transmission: the light from BEHIND that the surface lets
    //     through. This shading does NOT trace a continuation ray for it
    //     any more — it reports a per-channel `transmittance` and the
    //     lighting raygen composites what's behind front-to-back
    //     (re-tracing at recursion depth 0). That keeps stacked
    //     transparent layers free of recursion cost and reproduces the
    //     old result exactly.
    //
    // The pre-Q2 single-surface composite was
    //   emitted       = reflWeight·refl + (1-reflWeight)·lit·opacity + emission
    //   transmittance = (1-reflWeight)·(1-opacity)        [opacity-only]
    // Q2 keeps the emitted shape (reflection layered on top,
    // independent of opacity, so glass keeps its reflection) but the
    // surface transmittance reported to the raygen is now the spec's
    // own straight-through formula (§"Opacity / Transparency"):
    //   T_pbr = 1 − α(1 − (1 − M)·T_dielectric)
    // with transmission_color × transmission_weight × (1 − reflWeight)
    // folded into T_dielectric (see OpenPBRTransmittance). Notes:
    //   - Opaque (α = 1, transmission_weight = 0) → T_pbr = 0 EXACTLY,
    //     so the raygen / reflection segment loops break after their
    //     first iteration exactly as before (1e-3 thresholds intact).
    //   - geometry_opacity composes per the spec's f_PBR presence mix:
    //     the (1 − α) pass-through leg takes NO Fresnel; emission and
    //     the lit surface scale by α (emission previously didn't —
    //     spec-correct now, and unobservable for α = 1 materials).
    //   - Metal gates transmission via (1 − M) (metal slab is opaque).
    const float3 oneMinusRefl = float3(1.0, 1.0, 1.0) - reflWeightOut;
    const float3 emitted = reflWeightOut * reflColorOut
                         + oneMinusRefl * lit * opacity
                         + emission * opacity;

    SurfaceShadeResult result;
    result.emitted = emitted;
    result.transmittance = OpenPBRTransmittance(pbr, reflWeightOut);
    result.normalWorld = nWorld;
    result.baseColor   = baseColor;
    result.materialId  = materialId;
    return result;
}

// RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
// per-light selection + attenuation, extracted so DirectLightingPass and
// ReflectionsPass share the exact same light-loop math instead of each
// re-deriving it (the "reuse/extract shared helpers rather than
// duplicating math" mandate). APPENDED after ShadeSurfaceHit — that
// function's own inline per-light loop (above) is left byte-for-byte
// untouched, so the existing megakernel's golden-tested output can never
// pick up a codegen difference from this addition.
//
// Returns false for Dome lights (ambient — IndirectDiffusePass's domain,
// not this loop's) and disabled/sentinel lights. On true, `lightDir` /
// `lightColor` are ready for EvaluateOpenPBRDirect / EvaluateOpenPBRDirectSplit,
// and `attenuation` folds distance falloff × shaping-cone × (optional)
// shadow visibility — multiply the closure's return by it, exactly as
// ShadeSurfaceHit's own loop does.
//
// `traceShadow` selects whether an occlusion ray is fired: DirectLightingPass
// passes true (one shadow ray per light, "today's" flags/bias); ReflectionsPass
// passes false for its Phase-A single-bounce hit (deliberately cheap — no
// shadow rays there, see reflections.slang). When false, `attenuation` omits
// the visibility factor entirely (assumed 1.0) — NOT a "trace and ignore the
// result" shortcut, so ReflectionsPass never pays for the TraceRay it doesn't
// want.
bool EvaluateDirectLightSample(LightGpu L, float3 hitWorld, float3 nWorld,
                               uint recursionDepth, bool traceShadow,
                               out float3 lightDir, out float3 lightColor,
                               out float attenuation, out float occluderHitT) {
    lightDir = float3(0.0, 0.0, 0.0);
    lightColor = float3(0.0, 0.0, 0.0);
    attenuation = 0.0;
    // 0.0 = "unshadowed" sentinel (DirectLightingPass's dominant-light
    // guide channel — see OcclusionPayload's doc comment in
    // ShaderInterop.slang); only overwritten inside the traceShadow branch
    // below when the shadow ray actually hits an occluder.
    occluderHitT = 0.0;
    if (L.intensity <= 0.0 || L.kind == LIGHT_KIND_DOME)
        return false;

    lightColor = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
    float falloff = 1.0;
    if (L.kind == LIGHT_KIND_DISTANT) {
        lightDir = -float3(L.dirX, L.dirY, L.dirZ);
    } else {
        const float3 toLight = float3(L.posX, L.posY, L.posZ) - hitWorld;
        const float  distSq  = dot(toLight, toLight);
        falloff  = 1.0 / max(distSq, 0.01);
        lightDir = toLight * rsqrt(max(distSq, 1e-12));
    }
    const float NdotL = max(0.0, dot(nWorld, lightDir));

    // Same UsdLuxShapingAPI cone gate as ShadeSurfaceHit's loop.
    float coneAttenuation = 1.0;
    if (L.shapingConeCosOuter > 1e-4) {
        float3 coneAxis;
        if (L.kind == LIGHT_KIND_RECT) {
            const float3 axisU = float3(L.axisUx, L.axisUy, L.axisUz);
            const float3 axisV = float3(L.axisVx, L.axisVy, L.axisVz);
            coneAxis = normalize(cross(axisU, axisV));
        } else {
            coneAxis = float3(L.dirX, L.dirY, L.dirZ);
        }
        const float3 lightToSurface = (L.kind == LIGHT_KIND_DISTANT)
                                          ? float3(L.dirX, L.dirY, L.dirZ)
                                          : -lightDir;
        const float cosAngle = dot(normalize(lightToSurface), coneAxis);
        coneAttenuation = smoothstep(L.shapingConeCosOuter, L.shapingConeCosInner, cosAngle);
    }

    float visibility = 1.0;
    if (traceShadow && NdotL > 0.0 && coneAttenuation > 0.0
        && recursionDepth + 1u < MAX_RAY_RECURSION) {
        float lightDist = 1e30;
        if (L.kind != LIGHT_KIND_DISTANT) {
            const float3 toLight = float3(L.posX, L.posY, L.posZ) - hitWorld;
            lightDist = length(toLight);
        }
        RayDesc shadowRay;
        shadowRay.Origin    = hitWorld + nWorld * 1e-3;
        shadowRay.Direction = lightDir;
        shadowRay.TMin      = 1e-3;
        shadowRay.TMax      = max(1e-3, lightDist - 1e-3);
        OcclusionPayload shadowPayload;
        shadowPayload.visible = 0.0;  // pre-trace = occluded
        TraceRay(gTlas,
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                   | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0xFFu,
                 /*RayContributionToHitGroupIndex*/ 0,
                 /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                 /*MissShaderIndex*/ 1,
                 shadowRay,
                 shadowPayload);
        visibility = shadowPayload.visible;
        occluderHitT = shadowPayload.hitT;
    }

    attenuation = falloff * coneAttenuation * visibility;
    return true;
}

// RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
// IndirectDiffusePass's Phase A estimator: the dome-finding + lat-long UV
// + mip-bias sample ShadeSurfaceHit's inline LIGHT_KIND_DOME branch (above)
// uses, WITHOUT that branch's AO ray (AmbientOcclusionPass's job now —
// callers pass `aoVisibility` from the new AO signal, or 1.0 if unavailable).
// APPENDED (duplicated, not refactored out of ShadeSurfaceHit) for the same
// byte-identical-golden reason EvaluateDirectLightSample documents above.
// Accumulates over every enabled Dome light in the scene (matches
// ShadeSurfaceHit's loop, which never `break`s on the first one). Returns
// false iff the scene authors no enabled Dome light (ambientOut stays 0).
bool EvaluateDomeAmbient(OpenPBRSurfaceParams pbr, float3 nWorld, float3 viewDir,
                         float aoVisibility, out float3 ambientOut) {
    ambientOut = float3(0.0, 0.0, 0.0);
    bool foundDome = false;
    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    for (uint i = 0; i < lightCount; ++i) {
        LightGpu L = gLights[i];
        if (L.intensity <= 0.0 || L.kind != LIGHT_KIND_DOME)
            continue;
        foundDome = true;
        const float3 Lcol = float3(L.colorR, L.colorG, L.colorB) * L.intensity;

        float3 sampleDir = nWorld;
        if (abs(L.domeRotationYRadians) > 1e-4) {
            const float cosR = cos(-L.domeRotationYRadians);
            const float sinR = sin(-L.domeRotationYRadians);
            sampleDir = float3(cosR * nWorld.x + sinR * nWorld.z,
                                nWorld.y,
                                -sinR * nWorld.x + cosR * nWorld.z);
        }
        const float u = atan2(sampleDir.x, sampleDir.z) / PYXIS_TWO_PI + 0.5;
        const float v = acos(clamp(sampleDir.y, -1.0, 1.0)) / PYXIS_PI;

        float domeWidth;
        float domeHeight;
        float domeMipLevels;
        gDomeEnvMap.GetDimensions(0, domeWidth, domeHeight, domeMipLevels);
        const float targetMip = min(6.0, max(0.0, domeMipLevels - 1.0));
        const float3 domeRadiance =
            gDomeEnvMap.SampleLevel(gDomeSampler, float2(u, v), targetMip).rgb;

        ambientOut += EvaluateOpenPBRAmbient(pbr, nWorld, viewDir, Lcol * domeRadiance, aoVisibility);
    }
    return foundDome;
}

// RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
// shared per-channel demodulation: divides a radiance signal by the
// surface's albedo so the NRD-style denoiser guides carry texture detail
// separately from the lighting estimate (re-modulated in the eventual
// CompositePass) — used by DirectLightingPass's gDirectDiffuse (see that
// pass's file header: gIndirectDiffuse does NOT use this — its bounce
// estimator never multiplies by primary albedo in the first place, so
// there's nothing to divide back out).
//
// Halo-fix note (rtx-realtime-alignment-design.md, "denoiser chain" /
// white-fringe forensics, 2026-07-05): the divisor used to be guarded only
// against near-zero (`albedo > 1e-4`, zeroing the channel below that). That
// guard is far too small a floor — a dark-but-not-black surface (the World
// Lobby black vase measures ~0.02 linear) still divides through almost
// unguarded, amplifying this pass's 1-shadow-ray-per-pixel Monte-Carlo
// noise by roughly 1/albedo^2. DenoiseShadowPass's spatial filter (the only
// denoiser that touches this channel) has no albedo/material edge-stop of
// its own (normal + occluder-hitT only), so a tap that crosses the
// silhouette into a bright neighbor carries this blown-up value straight
// into CompositePass's remodulation there — a white/bright halo baked
// around every dark object's edge. Flooring the divisor at
// MIN_DEMODULATION_ALBEDO bounds the amplification. CompositePass
// (composite.slang) re-modulates gDirectDiffuse with the IDENTICAL floor
// (mirror this constant there if it ever changes) so the denoise-off round
// trip stays exact: signal / max(albedo, floor) * max(albedo, floor) ==
// signal, for ANY floor value — the floor only bounds the amplitude the
// denoiser sees, it does not change the undenoised result.
static const float MIN_DEMODULATION_ALBEDO = 0.04;

float3 DemodulateByAlbedo(float3 signal, float3 albedo) {
    return float3(
        signal.x / max(albedo.x, MIN_DEMODULATION_ALBEDO),
        signal.y / max(albedo.y, MIN_DEMODULATION_ALBEDO),
        signal.z / max(albedo.z, MIN_DEMODULATION_ALBEDO));
}

#endif  // PYXIS_SHADING_SLANG
