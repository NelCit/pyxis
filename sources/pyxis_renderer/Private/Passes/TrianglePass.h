// Pyxis renderer — TrianglePass (M1).
//
// Hard-coded triangle for the §41 M1 exit criterion. No vertex buffer
// — positions + colours are inlined in the vertex shader and indexed
// by SV_VertexID. The pass loads its own SPIR-V from
// <bin>/Resources/shaders/triangle.{vert,frag}.spv (compiled by
// _cmake/Slang.cmake at build time). Framebuffers are cached per
// nvrhi::ITexture identity so the swapchain rotation doesn't trigger
// a re-create every frame.

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <unordered_map>

namespace pyxis {

class TrianglePass final : public IRenderPass {
public:
    explicit TrianglePass(nvrhi::IDevice* device);
    ~TrianglePass() override;

    [[nodiscard]] std::string_view Name() const override { return "pass.Triangle"; }
    void Execute(nvrhi::ICommandList* commandList, const PassContext& ctx) override;

private:
    nvrhi::FramebufferHandle GetOrCreateFramebuffer(nvrhi::ITexture* color);

    nvrhi::IDevice*                  _device = nullptr;
    nvrhi::ShaderHandle              _vs;
    nvrhi::ShaderHandle              _fs;
    nvrhi::BindingLayoutHandle       _bindingLayout;
    nvrhi::BindingSetHandle          _bindingSet;

    // Pipeline depends on the framebuffer's render-target format set,
    // so we key it by the same hash as the framebuffer cache.
    std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle>     _framebufferCache;
    std::unordered_map<nvrhi::ITexture*, nvrhi::GraphicsPipelineHandle> _pipelineCache;

    bool _shadersOk = false;
};

}  // namespace pyxis
