// Pyxis renderer — TrianglePass implementation.

#include "Passes/TrianglePass.h"

#include "RenderGraph/PassContext.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include <cstdio>
#include <fstream>
#include <ios>
#include <vector>

namespace pyxis {

namespace {

std::vector<char> ReadBinaryFile(std::string_view path) noexcept {
    std::ifstream f(std::string{path}, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const auto sz = f.tellg();
    if (sz <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(sz));
    f.seekg(0, std::ios::beg);
    f.read(bytes.data(), sz);
    return bytes;
}

nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice* device, std::string_view path,
                              nvrhi::ShaderType stage, const char* entry) noexcept {
    auto& log = Logging::Get();
    auto bytes = ReadBinaryFile(path);
    if (bytes.empty()) {
        std::string msg = "TrianglePass: failed to load shader ";
        msg.append(path);
        log.Error(log::RENDER, msg);
        return nullptr;
    }
    nvrhi::ShaderDesc sd{};
    sd.shaderType = stage;
    sd.entryName  = entry;
    sd.debugName  = std::string{path};
    return device->createShader(sd, bytes.data(), bytes.size());
}

}  // namespace

TrianglePass::TrianglePass(nvrhi::IDevice* device) : _device(device) {
    const AssetLocator locator;
    const Path vertPath = locator.LocateResource("shaders/triangle.vert.spv");
    const Path fragPath = locator.LocateResource("shaders/triangle.frag.spv");

    _vs = LoadSpirv(_device, vertPath.View(), nvrhi::ShaderType::Vertex,   "main");
    _fs = LoadSpirv(_device, fragPath.View(), nvrhi::ShaderType::Pixel,    "main");
    if (!_vs || !_fs) {
        Logging::Get().Error(log::RENDER, "TrianglePass: shader load failed; pass will skip");
        return;
    }

    nvrhi::BindingLayoutDesc bl{};
    bl.visibility = nvrhi::ShaderType::All;
    _bindingLayout = _device->createBindingLayout(bl);

    const nvrhi::BindingSetDesc bs{};
    _bindingSet = _device->createBindingSet(bs, _bindingLayout);

    _shadersOk = true;
}

TrianglePass::~TrianglePass() = default;

nvrhi::FramebufferHandle TrianglePass::GetOrCreateFramebuffer(nvrhi::ITexture* color) {
    if (auto it = _framebufferCache.find(color); it != _framebufferCache.end()) {
        return it->second;
    }
    // Bounded-size cache: after a swapchain rebuild the prior swapchain
    // images are released by NVRHI's RefCountPtr but stale `nvrhi::ITexture*`
    // keys (whose values are now dangling framebuffers) sit in the cache
    // forever. Once we exceed 2× the expected swapchain image count, drop
    // everything — the cache miss costs one frame, which is invisible.
    constexpr std::size_t MAX_CACHE_ENTRIES = 6;
    if (_framebufferCache.size() >= MAX_CACHE_ENTRIES) {
        _framebufferCache.clear();
        _pipelineCache.clear();
    }
    nvrhi::FramebufferDesc fbDesc;
    fbDesc.addColorAttachment(color);
    const nvrhi::FramebufferHandle fb = _device->createFramebuffer(fbDesc);
    _framebufferCache[color] = fb;

    nvrhi::GraphicsPipelineDesc pipDesc;
    pipDesc.VS                                     = _vs;
    pipDesc.PS                                     = _fs;
    pipDesc.primType                               = nvrhi::PrimitiveType::TriangleList;
    pipDesc.bindingLayouts                         = { _bindingLayout };
    pipDesc.renderState.depthStencilState.depthTestEnable    = false;
    pipDesc.renderState.depthStencilState.depthWriteEnable   = false;
    pipDesc.renderState.depthStencilState.stencilEnable      = false;
    pipDesc.renderState.rasterState.cullMode                 = nvrhi::RasterCullMode::None;
    // Newer NVRHI deprecated the (desc, framebuffer) overload; the
    // (desc, framebuffer-info) form is the supported path.
    const nvrhi::GraphicsPipelineHandle pipe =
        _device->createGraphicsPipeline(pipDesc, fb->getFramebufferInfo());
    _pipelineCache[color] = pipe;
    return fb;
}

void TrianglePass::Execute(nvrhi::ICommandList* commandList, const PassContext& ctx) {
    if (!_shadersOk || !ctx.targets || !ctx.targets->color) return;

    nvrhi::ITexture* colorTex = ctx.targets->color;

    // Clear the color target to the settings-provided clear colour.
    if (ctx.settings) {
        const auto& c = ctx.settings->clearColor;
        const nvrhi::Color clearColor(c[0], c[1], c[2], c[3]);
        commandList->clearTextureFloat(colorTex, nvrhi::AllSubresources, clearColor);
    }

    const nvrhi::FramebufferHandle      framebuffer = GetOrCreateFramebuffer(colorTex);
    const nvrhi::GraphicsPipelineHandle pipeline    = _pipelineCache[colorTex];
    if (!pipeline || !framebuffer) return;

    const auto& tdesc = colorTex->getDesc();
    nvrhi::GraphicsState graphicsState;
    graphicsState.pipeline    = pipeline;
    graphicsState.framebuffer = framebuffer;
    graphicsState.bindings    = { _bindingSet };
    graphicsState.viewport.addViewportAndScissorRect(nvrhi::Viewport(
        static_cast<float>(tdesc.width),
        static_cast<float>(tdesc.height)));
    commandList->setGraphicsState(graphicsState);

    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    commandList->draw(draw);
}

}  // namespace pyxis
