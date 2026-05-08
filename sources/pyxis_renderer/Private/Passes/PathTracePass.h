// Pyxis renderer — primary-ray path-trace pass.
//
// Plan §41 M3. M3 simplification per the v1 scope discussion: 1 ray
// per pixel, no recursion, no NEE, no shadow rays. The closesthit
// shader returns barycentric colour; the miss shader returns flat
// sky blue. That's enough to validate the full RT-pipeline plumbing
// (BLAS / TLAS bound via descriptor set, raygen / miss / closesthit
// stitched together by a shader binding table, dispatchRays
// cascading the right shader records per-hit) without wading into
// the multi-bounce path-tracing integral that M5+ brings.
//
// Bindings (matched to raygen.slang):
//   space=0, b0  : CameraUniforms cbuffer  (uploaded per-frame from GpuScene::GetCamera)
//   space=0, t0  : RaytracingAccelerationStructure (TLAS, GpuScene::GetTlas)
//   space=0, u0  : RWTexture2D<float4>     (output colour AOV)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <string_view>
#include <unordered_map>

namespace pyxis {

class GpuScene;

class PathTracePass final : public IRenderPass {
public:
    PathTracePass(nvrhi::IDevice* device, GpuScene& scene);
    ~PathTracePass() override;
    PathTracePass(const PathTracePass&)            = delete;
    PathTracePass& operator=(const PathTracePass&) = delete;

    void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
    [[nodiscard]] std::string_view Name() const override { return "pass.PathTrace"; }

private:
    // Build the binding set for the supplied output texture. Cached
    // per `nvrhi::ITexture*` identity so we don't churn descriptor
    // sets on every frame; a swapchain rebuild invalidates pointers
    // and the cache is bounded so stale entries get evicted.
    [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* output);

    nvrhi::IDevice* _device   = nullptr;
    GpuScene*       _scene    = nullptr;

    // Compiled shaders + pipeline + shader binding table. Built once
    // in the ctor; reused every frame.
    nvrhi::ShaderHandle           _raygenShader;
    nvrhi::ShaderHandle           _missShader;
    nvrhi::ShaderHandle           _closestHitShader;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::rt::PipelineHandle     _pipeline;
    nvrhi::rt::ShaderTableHandle  _shaderTable;

    // Per-frame constant buffer carrying CameraUniforms (worldFromView
    // + viewFromClip inverses).
    nvrhi::BufferHandle           _cameraUniformsBuffer;

    // Output binding-set cache, keyed on the output texture pointer.
    // Bounded — a swapchain rebuild churns through up to ~3 swapchain
    // images, so 6 entries is more than enough headroom and the
    // eviction-on-overflow keeps stale dangling pointers from
    // accumulating across re-init.
    std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> _bindingSetCache;

    bool _shadersOk = false;   // true if ctor loaded all three shaders + built pipeline.
};

}  // namespace pyxis
