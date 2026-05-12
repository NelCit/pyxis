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

[[vk::binding(0, 0)]]  ConstantBuffer<CameraUniforms>      gCamera;
// M9-fidelity shadow rays: closesthit re-uses the same TLAS the
// raygen launched into to TraceRay shadow rays per direct light.
// Same binding-set entry — no new layout slot needed.
[[vk::binding(1, 0)]]  RaytracingAccelerationStructure     gTlas;
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
[[vk::binding(31, 0)]] StructuredBuffer<float4>             gMeshTangents;
[[vk::binding(32, 0)]] StructuredBuffer<uint>               gMeshTangentOffsets;

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

static const float PYXIS_PI_SAFE = 3.14159265358979323846;

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

    // M9-fidelity ray-cone LOD. Per-pixel angular spread × ray
    // distance gives the cone radius at the hit; combined with the
    // texture's own dimensions (queried per Sample) it picks the
    // right mip level without ray differentials. Distance is the
    // dominant aliasing cause; we skip the |cos(view, normal)|
    // grazing-angle correction (would need the not-yet-computed
    // smooth normal upstream of the baseColor sample). Ref:
    // Akenine-Möller, Ray Tracing Gems Ch 20.
    const float coneRadius = gCamera.pixelSpreadRadians * RayTCurrent();

    // Bindless baseColor sample, gated on the §11.6 HasBaseColorMap
    // flag the C++ side sets when the material's resolved
    // baseColorTex slot is valid. LOD computed via SampleByCone
    // (M9-fidelity ray-cone footprint) so distant surfaces use mip
    // levels instead of mip 0 (LOD-0-pinned aliasing was the dominant
    // texture artefact pre-M9-fidelity).
    float3 baseColor = float3(mat.baseColorR, mat.baseColorG, mat.baseColorB);
    if ((mat.flags & MATERIAL_FLAG_HAS_BASE_COLOR_MAP) != 0u
        && mat.baseColorTex < BINDLESS_TEXTURES_CAP) {
        baseColor = SampleByCone(mat.baseColorTex, uv, coneRadius);
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
        roughness = SampleByConeRGBA(mat.roughnessTex, uv, coneRadius).g;
    }
    if ((mat.flags & MATERIAL_FLAG_HAS_METALLIC_MAP) != 0u
        && mat.metallicTex < BINDLESS_TEXTURES_CAP) {
        metalness = SampleByConeRGBA(mat.metallicTex, uv, coneRadius).b;
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
    float3 nWorld = normalize(mul(worldFromObject3x3, nLocal));

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
        const uint   tangentBase = gMeshTangentOffsets[meshSlot];
        const float4 tv0 = gMeshTangents[tangentBase + v0];
        const float4 tv1 = gMeshTangents[tangentBase + v1];
        const float4 tv2 = gMeshTangents[tangentBase + v2];
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

            // Sample the tangent-space normal map. Convention: the
            // texture stores [0,1]-encoded tangent-space normals;
            // unpack to [-1,1] before transforming.
            const float3 nTangent =
                SampleByCone(mat.normalTex, uv, coneRadius) * 2.0 - 1.0;

            // Transform tangent-space normal to world space.
            nWorld = normalize(nTangent.x * tangentWorld
                             + nTangent.y * bitangentWorld
                             + nTangent.z * nWorld);
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
                const float3 axisU = float3(L.axisUx, L.axisUy, L.axisUz);
                const float3 axisV = float3(L.axisVx, L.axisVy, L.axisVz);
                coneAxis = normalize(cross(axisU, axisV));
            } else {
                coneAxis = float3(L.dirX, L.dirY, L.dirZ);
            }
            // Vector FROM light TO surface (Distant has implicit
            // direction = +L.dir; Rect uses -toLight). Compare to
            // cone axis via cosine.
            const float3 lightToSurface = (L.kind == LIGHT_KIND_RECT)
                                              ? -lightDir
                                              : float3(L.dirX, L.dirY, L.dirZ);
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
        float visibility = 1.0;
        if (NdotL > 0.0 && coneAttenuation > 0.0) {
            float lightDist = 1e30;
            if (L.kind == LIGHT_KIND_RECT) {
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
                SampleByCone(mat.emissionTex, uv, coneRadius);
            emission *= sampledEmission;
        }
    }

    // M9-fidelity composite: diffuse already has baseColor + (1-metalness)
    // baked in by the per-light loop. Ambient (dome) still uses the
    // M7-simple Lambertian-against-up approximation modulated by
    // baseColor. Specular is energy-conserving via the per-light F0
    // / metalness split inside GgxSpecular.
    const float3 lit = (totalContribution > 0.0)
        ? (directDiffuse + directSpecular + baseColor * ambientSum)
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
