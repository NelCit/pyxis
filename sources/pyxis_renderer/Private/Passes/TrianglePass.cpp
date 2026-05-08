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
    std::ifstream stream(std::string{path}, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) return {};
    const auto fileSize = stream.tellg();
    if (fileSize <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(fileSize));
    stream.seekg(0, std::ios::beg);
    stream.read(bytes.data(), fileSize);
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
    nvrhi::ShaderDesc shaderDesc{};
    shaderDesc.shaderType = stage;
    shaderDesc.entryName  = entry;
    shaderDesc.debugName  = std::string{path};
    return device->createShader(shaderDesc, bytes.data(), bytes.size());
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

    nvrhi::BindingLayoutDesc layoutDesc{};
    layoutDesc.visibility = nvrhi::ShaderType::All;
    _bindingLayout = _device->createBindingLayout(layoutDesc);

    const nvrhi::BindingSetDesc bindingSetDesc{};
    _bindingSet = _device->createBindingSet(bindingSetDesc, _bindingLayout);

    _shadersOk = true;
}

TrianglePass::~TrianglePass() = default;

nvrhi::FramebufferHandle TrianglePass::GetOrCreateFramebuffer(nvrhi::ITexture* color) {
    if (auto cached = _framebufferCache.find(color); cached != _framebufferCache.end()) {
        return cached->second;
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
    const nvrhi::FramebufferHandle framebuffer = _device->createFramebuffer(fbDesc);
    _framebufferCache[color] = framebuffer;

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.VS                                     = _vs;
    pipelineDesc.PS                                     = _fs;
    pipelineDesc.primType                               = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.bindingLayouts                         = { _bindingLayout };
    pipelineDesc.renderState.depthStencilState.depthTestEnable    = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable   = false;
    pipelineDesc.renderState.depthStencilState.stencilEnable      = false;
    pipelineDesc.renderState.rasterState.cullMode                 = nvrhi::RasterCullMode::None;
    // Newer NVRHI deprecated the (desc, framebuffer) overload; the
    // (desc, framebuffer-info) form is the supported path.
    const nvrhi::GraphicsPipelineHandle pipeline =
        _device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
    _pipelineCache[color] = pipeline;
    return framebuffer;
}

void TrianglePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
    if (!_shadersOk || !context.targets || !context.targets->color) return;

    nvrhi::ITexture* colorTex = context.targets->color;

    // Clear the color target to the settings-provided clear colour.
    if (context.settings) {
        const auto& clearRgba = context.settings->clearColor;
        const nvrhi::Color clearColor(clearRgba[0], clearRgba[1], clearRgba[2], clearRgba[3]);
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
