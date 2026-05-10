// Pyxis renderer — closest-hit shader.
//
// Plan §41 M5/M6/M7 + §15. Reads the per-instance OpenPBR material
// via a two-step lookup: `InstanceID()` (the per-instance slot
// packed into VkAccelerationStructureInstanceKHR.instanceCustomIndex
// per §15) indexes the instance→material side-table at binding 4 to
// get a material slot, which then indexes the material structured
// buffer at binding 3. The indirection frees the TLAS custom-index
// for the M6 instanceId AOV + future picking (§19.4).
//
// **M7-simple lighting (no NdotL, no shadow rays, no NEE, no MIS,
// no importance sampling)**. Each light contributes
// `color * intensity * weight` where weight is 1.0 for direct
// (Distant / Rect) and 0.5 for ambient (Dome). Final colour is
// `baseColor * sum_of_contributions`. The no-lights fallback
// (PathTracePass binds a 1-element intensity=0 sentinel) emits
// baseColor directly so M5/M6 unlit fixtures stay byte-equal.
//
// The user's full OpenPBR + NEE+MIS+IBL impl replaces this body
// alongside the BSDF lobes per §11.6 — branchless on `mat.flags`
// bits, sampling lights via NEE, querying the dome via the alias
// table at §7.
//
// What this shader exercises end-to-end at M5/M6/M7:
//   * Material buffer binding (binding 3, structured-buffer SRV)
//   * Instance→material side-table (binding 4, M6 P0)
//   * Lights buffer (binding 5, M7-simple)
//   * §11 OpenPBRMaterialGPU layout + LightGpu layout (declared in
//     ShaderInterop.slang; scalar fields avoid the float3
//     16-vs-12-byte trap)
//
// Bindings — match pyxis::PathTracePass's NVRHI BindingLayout. The
// raygen / miss shaders share the same set 0; binding numbers stay
// stable across stages so the single binding set covers all three.
//
//   set=0, binding=0  : CameraUniforms cbuffer (raygen-only)
//   set=0, binding=1  : RaytracingAccelerationStructure TLAS (raygen)
//   set=0, binding=2  : RWTexture2D<float4> output image (raygen)
//   set=0, binding=3  : StructuredBuffer<OpenPBRMaterialGPU> materials
//                       (closesthit — added at M5)
//   set=0, binding=4  : StructuredBuffer<uint> instance→material slot
//                       (closesthit — added at M6, plan §15)
//   set=0, binding=5  : StructuredBuffer<LightGpu> lights
//                       (closesthit + miss — added at M7-simple)
//   set=0, binding=6  : StructuredBuffer<uint> instance→mesh slot
//                       (closesthit — added at M7 NdotL)
//   set=0, binding=7  : StructuredBuffer<float4> mesh face normals
//                       (closesthit — added at M7 NdotL; flat
//                        per-mesh concatenation, indexed via offset 8)
//   set=0, binding=8  : StructuredBuffer<uint> mesh face-normal offsets
//                       (closesthit — added at M7 NdotL)
//   set=0, binding=9  : Texture2D<float4> dome HDRI env-map
//                       (miss — added at M7 IBL)
//   set=0, binding=10 : SamplerState dome HDRI sampler — also reused
//                       here for material bindless texture sampling
//                       (closesthit — added at M8a)
//   set=0, binding=24 : StructuredBuffer<float2> per-mesh UVs
//                       (closesthit — added at M8a UV pipeline)
//   set=0, binding=25 : StructuredBuffer<uint>   per-mesh UV offsets
//   set=0, binding=26 : StructuredBuffer<uint>   per-mesh triangle indices
//   set=0, binding=27 : StructuredBuffer<uint>   per-mesh index offsets
//   set=0, binding=28 : Texture2D[BINDLESS_TEXTURES_CAP] bindless materials
//                       (closesthit — added at M8a; indexed by mat.baseColorTex)

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]]  StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
[[vk::binding(4, 0)]]  StructuredBuffer<uint>               gInstanceMaterial;
[[vk::binding(5, 0)]]  StructuredBuffer<LightGpu>           gLights;
[[vk::binding(6, 0)]]  StructuredBuffer<uint>               gInstanceMesh;
[[vk::binding(7, 0)]]  StructuredBuffer<float4>             gMeshFaceNormals;
[[vk::binding(8, 0)]]  StructuredBuffer<uint>               gMeshFaceOffsets;
[[vk::binding(10, 0)]] SamplerState                         gBindlessSampler;
[[vk::binding(24, 0)]] StructuredBuffer<float2>             gMeshUvs;
[[vk::binding(25, 0)]] StructuredBuffer<uint>               gMeshUvOffsets;
[[vk::binding(26, 0)]] StructuredBuffer<uint>               gMeshIndices;
[[vk::binding(27, 0)]] StructuredBuffer<uint>               gMeshIndexOffsets;
[[vk::binding(28, 0)]] Texture2D                            gBindlessTextures[BINDLESS_TEXTURES_CAP];
[[vk::binding(29, 0)]] StructuredBuffer<float4>             gMeshVertexNormals;
[[vk::binding(30, 0)]] StructuredBuffer<uint>               gMeshVertexNormalOffsets;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // §15: InstanceID() is the per-instance slot. The side-table
    // resolves it to a material slot (0 = "no material assigned" →
    // sentinel grey material at materials[0]).
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceMaterial[instanceId];
    const OpenPBRMaterialGPU mat = gMaterials[materialId];

    // M8a UV pipeline: interpolate per-vertex UV via barycentrics,
    // then bindless-sample the baseColor texture (when authored) so
    // textured materials actually pick up their texels instead of
    // collapsing to the scalar fallback.
    //
    //   ofs = gMeshIndexOffsets[meshSlot]
    //   v0/1/2 = gMeshIndices[ofs + PrimitiveIndex()*3 + {0,1,2}]
    //   uv0/1/2 = gMeshUvs[gMeshUvOffsets[meshSlot] + v_i]
    //   uv = uv0*(1-bary.x-bary.y) + uv1*bary.x + uv2*bary.y
    // attribs.barycentrics is the (u,v) pair the closesthit input
    // attribute carries; w0 = 1 - u - v is the third weight.
    const uint meshSlot = gInstanceMesh[instanceId];
    const uint indexBase = gMeshIndexOffsets[meshSlot] + PrimitiveIndex() * 3u;
    const uint v0 = gMeshIndices[indexBase + 0u];
    const uint v1 = gMeshIndices[indexBase + 1u];
    const uint v2 = gMeshIndices[indexBase + 2u];
    const uint uvBase = gMeshUvOffsets[meshSlot];
    const float2 uv0 = gMeshUvs[uvBase + v0];
    const float2 uv1 = gMeshUvs[uvBase + v1];
    const float2 uv2 = gMeshUvs[uvBase + v2];
    const float2 bary = attribs.barycentrics;
    const float  w0   = 1.0 - bary.x - bary.y;
    const float2 uv   = uv0 * w0 + uv1 * bary.x + uv2 * bary.y;

    // Bindless baseColor sample, gated on the §11.6 HasBaseColorMap
    // flag the C++ side sets when the material's resolved
    // baseColorTex slot is valid. Without the flag check, INVALID_
    // BINDLESS_TEXTURE (=~0u) would index out of bounds → driver UB
    // (or magenta missingTexture if slot 0 hits via PartiallyBound).
    // Linear-clamp `gBindlessSampler` (binding 10) is shared with the
    // dome — the renderer owns one sampler in v1; per-role samplers
    // are an M9 polish item.
    float3 baseColor = float3(mat.baseColorR, mat.baseColorG, mat.baseColorB);
    if ((mat.flags & MATERIAL_FLAG_HAS_BASE_COLOR_MAP) != 0u
        && mat.baseColorTex < BINDLESS_TEXTURES_CAP) {
        const float4 sampled = gBindlessTextures[mat.baseColorTex].SampleLevel(
            gBindlessSampler, uv, 0);
        baseColor = sampled.rgb;
    }

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
    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3x3 worldFromObject3x3 = float3x3(
        objectToWorld[0].xyz,
        objectToWorld[1].xyz,
        objectToWorld[2].xyz);

    const uint vertexNormalBase = gMeshVertexNormalOffsets[meshSlot];
    const float3 nv0 = gMeshVertexNormals[vertexNormalBase + v0].xyz;
    const float3 nv1 = gMeshVertexNormals[vertexNormalBase + v1].xyz;
    const float3 nv2 = gMeshVertexNormals[vertexNormalBase + v2].xyz;
    const float3 smoothLocal = nv0 * w0 + nv1 * bary.x + nv2 * bary.y;
    const float  smoothLenSq = dot(smoothLocal, smoothLocal);

    float3 nLocal;
    if (smoothLenSq > 1e-6) {
        nLocal = smoothLocal * rsqrt(smoothLenSq);
    } else {
        // Mesh authored no normals — fall back to face normal.
        const uint faceOffset = gMeshFaceOffsets[meshSlot];
        nLocal = gMeshFaceNormals[faceOffset + PrimitiveIndex()].xyz;
    }
    const float3 nWorld = normalize(mul(worldFromObject3x3, nLocal));

    // M7-simple lighting + NdotL Lambert for direct lights. Loop
    // over every authored light, accumulate the per-light Lambert
    // contribution. The no-lights case (PathTracePass binds a single
    // intensity=0 sentinel) leaves `directSum` zero → falls through
    // to baseColor-only emission, preserving M5/M6 byte-equal.
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
    //   2. The camera's 2^exposure factor applied at raygen, which we
    //      already do (CameraUniforms.exposure × ACES tonemap).
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
    float3 directSum = float3(0.0, 0.0, 0.0);
    float3 ambientSum = float3(0.0, 0.0, 0.0);
    float  totalContribution = 0.0;
    const float3 hitWorld = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    for (uint i = 0; i < lightCount; ++i) {
        LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;  // sentinel / disabled
        const float3 Lcol = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
        if (L.kind == LIGHT_KIND_DOME) {
            // Dome ambient with NdotL against the world up axis.
            // The proper hemispheric integral
            //   ∫ radiance(ω) · max(0, dot(N, ω)) dω
            // is what the M7-full alias-table importance sampler
            // (§7) computes; the simplest approximation that
            // responds to surface orientation is Lambert against the
            // up axis: surfaces facing up get full dome, surfaces
            // facing horizontally get zero, surfaces facing down get
            // zero. Half-strength scale matches the M7-simple
            // "ambient is half-bright" convention so a single dome
            // doesn't out-bright a single sun.
            const float NdotUp = max(0.0, dot(nWorld, float3(0.0, 1.0, 0.0)));
            ambientSum += Lcol * 0.5 * NdotUp;
            totalContribution += L.intensity * 0.5;
            continue;
        }
        float3 lightDir;
        float falloff = 1.0;  // Distant defaults to no attenuation
        if (L.kind == LIGHT_KIND_RECT) {
            const float3 toLight = float3(L.posX, L.posY, L.posZ) - hitWorld;
            const float  distSq  = dot(toLight, toLight);
            // Soft minimum on r² avoids the 1/0 singularity for
            // surfaces touching a light. 0.01 m² ≈ 10cm — closer than
            // that, the falloff caps at 100×.
            falloff  = 1.0 / max(distSq, 0.01);
            lightDir = toLight * rsqrt(max(distSq, 1e-12));
        } else {
            // Distant: no falloff
            lightDir = -float3(L.dirX, L.dirY, L.dirZ);
        }
        const float NdotL = max(0.0, dot(nWorld, lightDir));
        directSum += Lcol * NdotL * falloff;
        // pre-NdotL, pre-falloff totalContribution drives only the
        // "any-lights / fall through to emissive baseColor" sentinel
        // below. Adding the raw intensity (rather than the attenuated
        // value) keeps that test stable regardless of distance —
        // otherwise a far-away scene with no nearby lights would
        // collapse to baseColor-only emission and lose dome/ambient.
        totalContribution += L.intensity;
    }

    // M9 emission. Emissive materials (UsdPreviewSurface authored
    // `emissiveColor` non-zero, MaterialFlag::EMISSIVE bit set by
    // PackMaterialGpu) add their authored radiance directly to the
    // output — independent of NdotL, distance falloff, or any
    // incoming light. Per-material emissionTex (when authored)
    // multiplies the constant emissionColor pointwise; no UV-set
    // indirection beyond what the baseColor sample already did.
    float3 emission = float3(0.0, 0.0, 0.0);
    if ((mat.flags & MATERIAL_FLAG_EMISSIVE) != 0u) {
        emission = float3(mat.emissionR, mat.emissionG, mat.emissionB)
                   * mat.emissionLuminance;
        if ((mat.flags & MATERIAL_FLAG_HAS_EMISSION_MAP) != 0u
            && mat.emissionTex < BINDLESS_TEXTURES_CAP) {
            const float3 sampledEmission = gBindlessTextures[mat.emissionTex].SampleLevel(
                gBindlessSampler, uv, 0).rgb;
            emission *= sampledEmission;
        }
    }

    const float3 lit = (totalContribution > 0.0)
        ? (baseColor * (directSum + ambientSum))
        : baseColor;
    payload.color = lit + emission;
    payload.hitT  = RayTCurrent();
    // M7 follow-up — fields the raygen-side AOV inspector reads.
    // Normal is the world-space face normal we already computed for
    // the Lambert pass; instanceId is the per-instance slot the
    // closesthit branched on for material lookup. baseColor /
    // materialId / worldHit feed the new BaseColor / MaterialID /
    // WorldPos AOVs respectively — all already in flight as locals.
    payload.normalWorld = nWorld;
    payload.instanceId  = instanceId;
    payload.baseColor   = baseColor;
    payload.materialId  = materialId;
    payload.worldHit    = hitWorld;
    // PrimitiveIndex() is the per-face id within the BLAS — Hydra's
    // HdAovTokens->elementId. ~0u is the miss sentinel raygen sets
    // before TraceRay; we overwrite with the real face here.
    payload.elementId   = PrimitiveIndex();
}
