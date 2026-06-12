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
//   set=0, binding=4  : StructuredBuffer<InstanceInfoGpu> per-instance
//                       record (P2 packing — object→world rows + the
//                       §15 material slot + the M7-NdotL mesh slot;
//                       replaces the two parallel uint side-tables)
//   set=0, binding=5  : StructuredBuffer<LightGpu> lights
//                       (closesthit + miss — added at M7-simple)
//   set=0, binding=6  : StructuredBuffer<MeshInfoGpu> per-mesh record
//                       (P2 packing — face/UV/index/vertex-attrib
//                       offsets + vertexCount; replaces the five
//                       parallel per-mesh uint offset tables)
//   set=0, binding=7  : StructuredBuffer<float4> mesh face normals
//                       (closesthit — added at M7 NdotL; flat
//                        per-mesh concatenation, offset in MeshInfoGpu)
//   set=0, binding=9  : Texture2D<float4> dome HDRI env-map
//                       (miss — added at M7 IBL)
//   set=0, binding=10 : SamplerState dome HDRI sampler — also reused
//                       here for material bindless texture sampling
//                       (closesthit — added at M8a)
//   set=0, binding=24 : StructuredBuffer<float2> per-mesh UVs
//                       (closesthit — added at M8a UV pipeline)
//   set=0, binding=26 : StructuredBuffer<uint>   triangle indices
//                       (§14.5 index-pool structured view; offsets in
//                       MeshInfoGpu are pool element offsets)
//   set=0, binding=28 : Texture2D[BINDLESS_TEXTURES_CAP] bindless materials
//                       (closesthit — added at M8a; indexed by mat.baseColorTex)
//   set=0, binding=29 : StructuredBuffer<VertexAttribGpu> interleaved
//                       per-vertex normal+tangent (P2 packing; replaces
//                       the two parallel padded float4 streams)
//   (bindings 8/25/27/30/31/32 are RETIRED by the P2 packing —
//    Vulkan allows sparse binding numbers.)

#include "ShaderInterop.slang"

// V2.B follow-up — bind the dome HDRI + its sampler so the closesthit
// can evaluate dome irradiance per-surface (the "fast diffuse IBL"
// trick: sample the HDRI at the surface normal direction with a high
// mip bias to approximate the cosine-weighted hemisphere convolution).
// Replaces the M7-simple Lambert-vs-world-up approximation that gave
// vertical surfaces zero dome contribution. Bindings match the miss
// shader's layout (binding 9 + sampler 33) — descriptor-set 0 is
// pipeline-wide, so the existing PathTracePass binding fills both
// shaders. Deterministic by construction (single texture sample, no
// RNG, no Monte Carlo).
[[vk::binding(9, 0)]]  Texture2D<float4>                   gDomeEnvMap;
[[vk::binding(33, 0)]] SamplerState                        gDomeSampler;

[[vk::binding(0, 0)]]  ConstantBuffer<CameraUniforms>      gCamera;
// M9-fidelity shadow rays: closesthit re-uses the same TLAS the
// raygen launched into to TraceRay shadow rays per direct light.
// Same binding-set entry — no new layout slot needed.
[[vk::binding(1, 0)]]  RaytracingAccelerationStructure     gTlas;
[[vk::binding(3, 0)]]  StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
[[vk::binding(4, 0)]]  StructuredBuffer<InstanceInfoGpu>    gInstanceInfo;
[[vk::binding(5, 0)]]  StructuredBuffer<LightGpu>           gLights;
[[vk::binding(6, 0)]]  StructuredBuffer<MeshInfoGpu>        gMeshInfo;
[[vk::binding(7, 0)]]  StructuredBuffer<float4>             gMeshFaceNormals;
[[vk::binding(10, 0)]] SamplerState                         gBindlessSampler;
[[vk::binding(24, 0)]] StructuredBuffer<float2>             gMeshUvs;
[[vk::binding(26, 0)]] StructuredBuffer<uint>               gMeshIndices;
[[vk::binding(28, 0)]] Texture2D                            gBindlessTextures[BINDLESS_TEXTURES_CAP];
[[vk::binding(29, 0)]] StructuredBuffer<VertexAttribGpu>    gMeshVertexAttribs;

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
    return gBindlessTextures[texIdx].SampleLevel(gBindlessSampler, uv, lod).rgb;
}

float4 SampleByConeRGBA(uint texIdx, float2 uv, float coneRadius) {
    float texW;
    float texH;
    gBindlessTextures[texIdx].GetDimensions(texW, texH);
    const float texSize = sqrt(texW * texH);
    const float lod = max(0.0, log2(max(coneRadius * texSize, 1.0)));
    return gBindlessTextures[texIdx].SampleLevel(gBindlessSampler, uv, lod);
}

// Anisotropic sample via hardware SampleGrad. `ddxUv` / `ddyUv` are the
// UV-space footprint axes (∂uv/∂screenX-equivalent, ∂uv/∂screenY-
// equivalent) the closesthit derives from the ray-cone ellipse +
// surface tangent frame. The HW builds the anisotropic footprint from
// the two gradient vectors and runs N-tap anisotropic filtering — sharp
// along the minor axis, filtered along the stretched major axis. This
// is the NVIDIA RTX texture-LOD approach and removes grazing-angle
// moiré/streaking that isotropic mip selection (SampleByCone) can't.
float3 SampleByGrad(uint texIdx, float2 uv, float2 ddxUv, float2 ddyUv) {
    return gBindlessTextures[texIdx].SampleGrad(gBindlessSampler, uv, ddxUv, ddyUv).rgb;
}

float4 SampleByGradRGBA(uint texIdx, float2 uv, float2 ddxUv, float2 ddyUv) {
    return gBindlessTextures[texIdx].SampleGrad(gBindlessSampler, uv, ddxUv, ddyUv);
}

static const float PYXIS_PI_SAFE = 3.14159265358979323846;
static const float PYXIS_PI       = 3.14159265358979323846;
static const float PYXIS_TWO_PI   = 6.28318530717958647692;
static const float PYXIS_INV_PI   = 0.31830988618379067154;

// V2.B — ray recursion budget. Every secondary TraceRay (reflection,
// AO, shadow) is gated on `payload.recursionDepth + 1u <
// MAX_RAY_RECURSION`, and the pipeline's maxRecursionDepth
// (PathTracePass) is set to the same value. This matches the
// plan_for_v2.md §V2.B doctrine (direct + shadows + 1 reflection bounce
// = depth 3) exactly.
//
// Note: opacity TRANSMISSION no longer spends this budget. An earlier
// follow-up traced a recursive continuation ray straight through each
// transparent surface, which forced the cap up to 6 (a glass pane is 2
// surfaces; two panes = 4, plus a reflection + a shadow level). That
// see-through is now composited FRONT-TO-BACK by raygen's transmission
// loop at recursion depth 0 — see raygen.slang — so stacked transparent
// layers are unbounded and free of recursion cost, and the budget is
// back to the documented 3.
static const uint MAX_RAY_RECURSION = 3u;

// M9-fidelity Schlick Fresnel. F0 derives per-material:
//   dielectrics: F0 = ((ior - 1) / (ior + 1))² — typically ~0.04
//                 for plastics / glass at ior ≈ 1.5
//   metals:      F0 = baseColor (each channel acts as the per-
//                 wavelength reflectance at normal incidence)
// metalness blends linearly between the two.
float3 SchlickF0(float3 baseColor, float metalness, float ior) {
    const float dielectricBase = (ior - 1.0) / (ior + 1.0);
    const float dielectricF0   = dielectricBase * dielectricBase;
    return lerp(float3(dielectricF0, dielectricF0, dielectricF0),
                baseColor, metalness);
}

float3 SchlickFresnel(float3 f0, float cosTheta) {
    const float oneMinus = 1.0 - max(0.0, cosTheta);
    const float oneMinus5 = oneMinus * oneMinus * oneMinus * oneMinus * oneMinus;
    return f0 + (float3(1.0, 1.0, 1.0) - f0) * oneMinus5;
}

// Simplified GGX specular evaluation for direct lighting. Trowbridge-
// Reitz NDF + Smith correlated G — full microfacet eval but no
// importance sampling (we evaluate per-light contribution at the
// half-vector, not sample a bounce direction). Roughness is
// remapped via α = roughness² (the Disney convention) so the input
// roughness is perceptually linear.
//
// Returns: per-channel specular contribution. Multiply by light
// radiance, NdotL, falloff, and cone attenuation at the call site.
float3 GgxSpecular(float3 N, float3 V, float3 L, float3 baseColor,
                   float metalness, float roughness, float ior) {
    const float3 H = normalize(V + L);
    const float NdotL = max(1e-4, dot(N, L));
    const float NdotV = max(1e-4, dot(N, V));
    const float NdotH = max(1e-4, dot(N, H));
    const float VdotH = max(1e-4, dot(V, H));

    // Roughness² remap (Disney). α=0 is mirror; α=1 is diffuse-like.
    const float alpha  = max(0.001, roughness * roughness);
    const float alpha2 = alpha * alpha;

    // GGX / Trowbridge-Reitz NDF.
    const float denomD = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    const float D      = alpha2 / (PYXIS_PI_SAFE * denomD * denomD);

    // Smith-correlated G (Heitz 2014 — single divide for both
    // shadowing+masking).
    const float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2);
    const float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2);
    const float G2      = 0.5 / (lambdaV + lambdaL);

    // Schlick Fresnel at the half-vector.
    const float3 F0 = SchlickF0(baseColor, metalness, ior);
    const float3 F  = SchlickFresnel(F0, VdotH);

    return D * G2 * F;  // (NdotL × NdotV) factor cancels with G2's denominator
}

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // §15: InstanceID() is the per-instance slot. The packed instance
    // record resolves it to a material slot (0 = "no material assigned"
    // → sentinel grey material at materials[0]) + the mesh slot whose
    // packed MeshInfoGpu record carries every side-table offset.
    const uint instanceId = InstanceID();
    const InstanceInfoGpu instInfo = gInstanceInfo[instanceId];
    const uint materialId = instInfo.materialSlot;
    const OpenPBRMaterialGPU mat = gMaterials[materialId];

    // M8a UV pipeline: interpolate per-vertex UV via barycentrics,
    // then bindless-sample the baseColor texture (when authored) so
    // textured materials actually pick up their texels instead of
    // collapsing to the scalar fallback.
    //
    //   mi = gMeshInfo[meshSlot]
    //   v0/1/2 = gMeshIndices[mi.indexOffset + PrimitiveIndex()*3 + {0,1,2}]
    //   uv0/1/2 = gMeshUvs[mi.uvOffset + v_i]
    //   uv = uv0*(1-bary.x-bary.y) + uv1*bary.x + uv2*bary.y
    // attribs.barycentrics is the (u,v) pair the closesthit input
    // attribute carries; w0 = 1 - u - v is the third weight.
    const uint meshSlot = instInfo.meshSlot;
    const MeshInfoGpu mi = gMeshInfo[meshSlot];
    const uint indexBase = mi.indexOffset + PrimitiveIndex() * 3u;
    const uint v0 = gMeshIndices[indexBase + 0u];
    const uint v1 = gMeshIndices[indexBase + 1u];
    const uint v2 = gMeshIndices[indexBase + 2u];
    const uint uvBase = mi.uvOffset;
    const float2 uv0 = gMeshUvs[uvBase + v0];
    const float2 uv1 = gMeshUvs[uvBase + v1];
    const float2 uv2 = gMeshUvs[uvBase + v2];
    const float2 bary = attribs.barycentrics;
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
        const float3 hitW = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
        const float3 faceNormalLocal =
            gMeshFaceNormals[mi.faceOffset + PrimitiveIndex()].xyz;
        float3 hp;
        float3 fnP;
        if (objectProj) {
            // Project in the instance's local frame: transform the hit by
            // WorldToObject; the stored face normal is ALREADY object-space.
            hp  = mul(WorldToObject3x4(), float4(hitW, 1.0)).xyz;
            fnP = abs(normalize(faceNormalLocal));
        } else {
            const float3x4 o2wp = ObjectToWorld3x4();
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
    const float coneRadius = gCamera.pixelSpreadRadians * RayTCurrent();

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
    const uint   faceDensitySlot = mi.faceOffset + PrimitiveIndex();
    const float  faceTexelDensity = gMeshFaceNormals[faceDensitySlot].w;
    const float3x4 instanceXform = ObjectToWorld3x4();
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
    const float  ndotvLod = max(abs(dot(faceNormalW, normalize(-WorldRayDirection()))), 0.1);
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
        const float3 vw  = normalize(-WorldRayDirection());
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
            const float3 viewW = normalize(-WorldRayDirection());
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
        baseColor *= SampleByGrad(mat.baseColorTex, uv, ddxUv, ddyUv);
    }

    // M9-fidelity: roughness + metallic from per-material textures.
    // Both default to the scalar in the material; per-texture sample
    // overrides when the matching MATERIAL_FLAG_HAS_*_MAP bit is set
    // (PackMaterialGpu sets the bit when the texture handle is valid).
    // Roughness samples the .g channel (the GLTF / OpenPBR convention
    // for ORM packed maps); metallic samples .b. UsdPreviewSurface
    // authoring can split them across separate textures, in which
    // case the .g read still hits the data channel for a single-
    // channel roughness map.
    float roughness = mat.roughness;
    float metalness = mat.metalness;
    if ((mat.flags & MATERIAL_FLAG_HAS_ROUGHNESS_MAP) != 0u
        && mat.roughnessTex < BINDLESS_TEXTURES_CAP) {
        roughness = SampleByGradRGBA(mat.roughnessTex, uv, ddxUv, ddyUv).g;
    }
    if ((mat.flags & MATERIAL_FLAG_HAS_METALLIC_MAP) != 0u
        && mat.metallicTex < BINDLESS_TEXTURES_CAP) {
        metalness = SampleByGradRGBA(mat.metallicTex, uv, ddxUv, ddyUv).b;
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
    const float3x4 objectToWorld = ObjectToWorld3x4();
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
        nLocal = gMeshFaceNormals[faceOffset + PrimitiveIndex()].xyz;
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
    // side and for double-sided materials alike. WorldRayDirection()
    // points FROM eye TO surface, so a positive dot means the normal
    // shares the ray's direction (faces away from the eye) → flip.
    if (dot(nWorld, WorldRayDirection()) > 0.0)
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
    // Three guards skip the TBN sample (closesthit keeps the
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
            // the tangent-plane deviation, then reconstruct Z from the
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
        const float3 viewW = normalize(-WorldRayDirection());
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
    float3 directDiffuse  = float3(0.0, 0.0, 0.0);
    float3 directSpecular = float3(0.0, 0.0, 0.0);
    float3 ambientSum     = float3(0.0, 0.0, 0.0);
    float  totalContribution = 0.0;
    const float3 hitWorld = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    // M9-fidelity: view direction (FROM surface TO eye) for the GGX
    // microfacet half-vector + Schlick Fresnel.
    const float3 viewDir = normalize(-WorldRayDirection());
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
            // the final composition at line ~574 multiplies
            // `ambientSum` by `baseColor`, which together form the
            // diffuse BRDF `baseColor / π` × irradiance.
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
            if (payload.recursionDepth + 1u < MAX_RAY_RECURSION)
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
                aoPayload.recursionDepth  = payload.recursionDepth + 1u;
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

            ambientSum += Lcol * domeRadiance * PYXIS_INV_PI * aoVisibility;
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
        // generated this closesthit invocation. TMin also takes
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
            && payload.recursionDepth + 1u < MAX_RAY_RECURSION) {
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
            shadowPayload.recursionDepth  = payload.recursionDepth + 1;  // M29
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

        // M9-fidelity diffuse + specular split. Diffuse: Lambert
        // (baseColor × NdotL). Specular: GGX microfacet at the
        // half-vector. Schlick Fresnel handles the dielectric vs
        // metal F0 split inside GgxSpecular. Energy conservation
        // (1 - F) on diffuse for dielectrics + (0) for metals
        // (which absorb diffuse light) is approximated via
        // (1 - metalness) — proper energy-conservation requires
        // computing F at the view angle and weighting diffuse by
        // (1 - F)·(1 - metalness), which is M11+ polish.
        const float3 diffuseTerm =
            baseColor * (1.0 - metalness) * NdotL * falloff * coneAttenuation * visibility;
        const float3 specularTerm =
            GgxSpecular(nWorld, viewDir, lightDir, baseColor, metalness, roughness, mat.specularIor)
            * NdotL * falloff * coneAttenuation * visibility;
        directDiffuse  += Lcol * diffuseTerm;
        directSpecular += Lcol * specularTerm;
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
            const float3 sampledEmission =
                SampleByGrad(mat.emissionTex, uv, ddxUv, ddyUv);
            emission *= sampledEmission;
        }
    }

    // M9-fidelity composite: diffuse already has baseColor + (1-metalness)
    // baked in by the per-light loop. Ambient (dome) still uses the
    // M7-simple Lambertian-against-up approximation modulated by
    // baseColor. Specular is energy-conserving via the per-light F0
    // / metalness split inside GgxSpecular.
    // Ambient (dome) is a LAMBERTIAN diffuse term, so it must be gated
    // by (1 - metalness): metals have no diffuse lobe. Without this a
    // metal (mirror, gold, steel) gets a grey Lambertian ambient wash
    // layered over its reflection — the "washed-out mirror" symptom
    // (the mirror is metal=1 yet read half-grey). Now metals rely on the
    // specular reflection (handled in the layered composite below).
    float3 lit = (totalContribution > 0.0)
        ? (directDiffuse + directSpecular + baseColor * ambientSum * (1.0 - metalness))
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
    if (payload.recursionDepth + 1u < MAX_RAY_RECURSION
        && (metalness > 0.05 || roughness < 0.5))
    {
        // Geometric mirror direction. WorldRayDirection() is FROM eye
        // TO surface; `reflect(d, n)` returns the mirror across `n`
        // (which is the surface normal). For roughness > 0 we keep the
        // mirror direction unchanged today — the cone-spreading via
        // envmap-mip blur lands in the follow-up. Self-intersection
        // is avoided by pushing the origin out by 1e-3 along the
        // shading normal, same epsilon shadow rays use above.
        const float3 reflDir = normalize(reflect(WorldRayDirection(), nWorld));

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
        // mirrors raygen's primary transmission loop (raygen.slang) but
        // runs at the reflection ray's recursion depth. Each segment
        // re-traces past the previous translucent hit by LOOP iteration
        // (not hardware recursion), so the §V2.B depth cap of 3 is
        // untouched; stacked transparent layers in a reflection composite
        // exactly as they do for the primary ray. 0.999 / 1e-3 thresholds
        // match the primary loop and the anyhit translucency gate.
        const uint REFL_MAX_SEGMENTS = 16u;
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
            reflPayload.recursionDepth  = payload.recursionDepth + 1u;
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

        // Schlick-Fresnel weight at the mirror direction. F0 derives
        // per-material (metals = baseColor, dielectrics = ((ior-1)/
        // (ior+1))²). With mirror sampling we evaluate Fresnel at the
        // surface normal (H = N → HdotV = NdotV), which is exact for
        // smooth surfaces and a deterministic approximation for rough
        // ones until the envmap blur lands.
        const float3 F0 = SchlickF0(baseColor, metalness, mat.specularIor);
        const float NdotV = max(0.0, dot(nWorld, viewDir));
        // V2.B follow-up — roughness-aware glossy attenuation. The
        // bounce ray is a single sharp mirror sample (no stochastic
        // cone, per the no-noise doctrine); a rough surface should
        // reflect a dimmer + softer image than a polished one. We
        // can't blur the reflected geometry deterministically, so we
        // fade the reflection's contribution as roughness climbs:
        // (1 - roughness)² gives a perceptually-reasonable falloff
        // (polished floor at roughness 0.12 keeps ~77% of the mirror;
        // a half-rough surface keeps ~25%; matte surfaces approach 0).
        // This both softens the hard `roughness < 0.5` gate edge and
        // stops rough surfaces from showing a crisp — and physically
        // wrong — mirror image.
        const float glossy = (1.0 - roughness) * (1.0 - roughness);
        reflWeightOut = SchlickFresnel(F0, NdotV) * glossy;
        reflColorOut  = reflRadiance;
    }

    // V2.B — layered glass/translucency composite. Two transport paths
    // stay SEPARATE so a transparent surface still reflects:
    //   * reflection: a Fresnel-weighted term (reflWeightOut) layered ON
    //     TOP, independent of opacity.
    //   * transmission: the light from BEHIND that the surface lets
    //     through. This shader does NOT trace a continuation ray for it
    //     any more — it reports a per-channel `transmittance` and raygen
    //     composites what's behind front-to-back (re-tracing at recursion
    //     depth 0). That keeps stacked transparent layers free of
    //     recursion cost and reproduces the old result exactly.
    //
    // The previous single-surface composite was
    //   out = reflWeight·refl + (1-reflWeight)·(lit·opacity + behind·(1-opacity)) + emission
    // Split into a surface-emitted term (independent of `behind`) and a
    // transmittance the raygen loop multiplies into `behind`:
    //   emitted       = reflWeight·refl + (1-reflWeight)·lit·opacity + emission
    //   transmittance = (1-reflWeight)·(1-opacity)
    //   - Glass (opacity 0): emitted ≈ reflWeight·refl (+emission),
    //     transmittance ≈ (1-reflWeight) → grazing reflects, head-on sees
    //     through. Falls back to reflection (not black) past the loop cap.
    //   - Mirror / metal: reflWeight ≈ baseColor → emitted ≈ reflection,
    //     transmittance ≈ 0.
    //   - Opaque diffuse: opacity 1 → transmittance 0, emitted ≈ lit.
    // 0.999 matches the anyhit translucency gate.
    const float3 oneMinusRefl = float3(1.0, 1.0, 1.0) - reflWeightOut;
    const float3 emitted = reflWeightOut * reflColorOut
                         + oneMinusRefl * lit * mat.opacity
                         + emission;

    payload.color = emitted;
    payload.transmittance = (mat.opacity < 0.999)
                              ? oneMinusRefl * (1.0 - mat.opacity)
                              : float3(0.0, 0.0, 0.0);
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
