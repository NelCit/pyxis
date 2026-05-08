// Pyxis renderer — primary-ray path-trace pass.

#include "Passes/PathTracePass.h"

#include "RenderGraph/PassContext.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <hlsl++.h>

#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

namespace pyxis {

namespace {

// CameraUniforms layout matches resources/shaders/ShaderInterop.slang
// — two float4x4 matrices, 128 bytes total. We don't reach for the
// shared header here because including a Slang file from C++ requires
// jumping through hoops; the layout is a hard contract anyway, so a
// local shadow of the struct + a static_assert against the expected
// size is the simplest enforcement.
struct CameraUniformsCpu {
    hlslpp::float4x4 worldFromView;
    hlslpp::float4x4 viewFromClip;
};
static_assert(sizeof(CameraUniformsCpu) == 128,
              "CameraUniforms is two row-major float4x4s = 128 bytes "
              "(must match resources/shaders/ShaderInterop.slang).");

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

nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice*    device,
                              std::string_view   path,
                              nvrhi::ShaderType  stage,
                              const char*        entry) noexcept {
    auto& log = Logging::Get();
    auto bytes = ReadBinaryFile(path);
    if (bytes.empty()) {
        std::string msg = "PathTracePass: failed to load shader ";
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

PathTracePass::PathTracePass(nvrhi::IDevice* device, GpuScene& scene)
    : _device(device), _scene(&scene) {
    const AssetLocator locator;
    const Path raygenPath     = locator.LocateResource("shaders/raygen.spv");
    const Path missPath       = locator.LocateResource("shaders/miss.spv");
    const Path closestHitPath = locator.LocateResource("shaders/closesthit.spv");

    _raygenShader     = LoadSpirv(_device, raygenPath.View(),
                                  nvrhi::ShaderType::RayGeneration, "RayGenMain");
    _missShader       = LoadSpirv(_device, missPath.View(),
                                  nvrhi::ShaderType::Miss, "MissMain");
    _closestHitShader = LoadSpirv(_device, closestHitPath.View(),
                                  nvrhi::ShaderType::ClosestHit, "ClosestHitMain");
    if (!_raygenShader || !_missShader || !_closestHitShader) {
        Logging::Get().Error(log::RENDER,
            "PathTracePass: shader load failed; pass will skip");
        return;
    }

    // Binding layout — visibility=AllRayTracing covers all five RT
    // stages so we don't have to re-author this for shadow-trace etc.
    // additions later. Slot indices match the raygen.slang register
    // assignments (b0 / t0 / u0).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),   // b0 CameraUniforms
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),    // t0 TLAS
        nvrhi::BindingLayoutItem::Texture_UAV(0),              // u0 output
    };
    _bindingLayout = _device->createBindingLayout(layoutDesc);
    if (!_bindingLayout) {
        Logging::Get().Error(log::RENDER, "PathTracePass: createBindingLayout failed");
        return;
    }

    // Pipeline state — three shader stages registered by `exportName`,
    // one hit group bundling closesthit (no anyhit / intersection in
    // M3), maxRecursionDepth=1 because we only TraceRay from raygen
    // and the closesthit doesn't recurse.
    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.shaders = {
        nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(_raygenShader),
        nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain"  ).setShader(_missShader),
    };
    pipelineDesc.hitGroups = {
        nvrhi::rt::PipelineHitGroupDesc{}
            .setExportName("HitGroupDefault")
            .setClosestHitShader(_closestHitShader),
    };
    pipelineDesc.globalBindingLayouts = { _bindingLayout };
    pipelineDesc.maxRecursionDepth    = 1;
    _pipeline = _device->createRayTracingPipeline(pipelineDesc);
    if (!_pipeline) {
        Logging::Get().Error(log::RENDER, "PathTracePass: createRayTracingPipeline failed");
        return;
    }

    // Shader binding table — one raygen, one miss, one hit group.
    // The pass dispatches with this single SBT every frame; the
    // entries are static across the run.
    _shaderTable = _pipeline->createShaderTable();
    if (!_shaderTable) {
        Logging::Get().Error(log::RENDER, "PathTracePass: createShaderTable failed");
        return;
    }
    _shaderTable->setRayGenerationShader("RayGenMain");
    _shaderTable->addMissShader("MissMain");
    _shaderTable->addHitGroup("HitGroupDefault");

    // Camera uniforms constant buffer — sized for one CameraUniforms
    // struct; rewritten every frame from GpuScene's CameraDesc.
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize           = sizeof(CameraUniformsCpu);
    cbDesc.debugName          = "PathTrace.CameraUniforms";
    cbDesc.isConstantBuffer   = true;
    cbDesc.isVolatile         = true;
    cbDesc.maxVersions        = 16;   // ample for per-frame rewrites within a multi-frame ring.
    _cameraUniformsBuffer     = _device->createBuffer(cbDesc);
    if (!_cameraUniformsBuffer) {
        Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(CameraUniforms) failed");
        return;
    }

    _shadersOk = true;
    Logging::Get().Info(log::RENDER, "PathTracePass: initialised (RT pipeline + SBT ready)");
}

PathTracePass::~PathTracePass() = default;

nvrhi::BindingSetHandle PathTracePass::GetOrCreateBindingSet(nvrhi::ITexture* output) {
    if (auto cached = _bindingSetCache.find(output); cached != _bindingSetCache.end()) {
        return cached->second;
    }
    constexpr std::size_t MAX_CACHE_ENTRIES = 6;
    if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES) {
        _bindingSetCache.clear();
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, _cameraUniformsBuffer),
        nvrhi::BindingSetItem::RayTracingAccelStruct(0, _scene->GetTlas()),
        nvrhi::BindingSetItem::Texture_UAV(0, output),
    };
    nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
    _bindingSetCache[output] = set;
    return set;
}

void PathTracePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
    if (!_shadersOk) return;
    if (commandList == nullptr || context.targets == nullptr) return;
    nvrhi::ITexture* const output = context.targets->color;
    if (output == nullptr) return;

    // Need a TLAS + camera before we can trace anything. M3 callers
    // (HeadlessMode / ViewerMode) populate both at startup and call
    // CommitResources before RenderFrame, so the early-out only
    // fires in degenerate "scene with no instances or camera"
    // configurations.
    const nvrhi::rt::IAccelStruct* const tlas = _scene->GetTlas();
    if (tlas == nullptr || !_scene->HasCamera()) return;

    const Profiler::GpuScope gpuScope(*context.profiler, commandList, "pass.PathTrace");

    // ---- Upload camera uniforms ----------------------------------------
    // Inverses are computed on the CPU each frame because the shader
    // wants worldFromView + viewFromClip, while GpuScene stores the
    // forward viewFromWorld + projFromView (matching CameraDesc's
    // §18.4 contract). hlslpp::inverse handles the row-vector
    // convention correctly because it's a pure linear-algebra
    // operation that doesn't care about row-vector vs column-vector
    // semantics.
    const CameraDesc& camera = _scene->GetCamera();
    CameraUniformsCpu cameraUniforms{};
    cameraUniforms.worldFromView = hlslpp::inverse(camera.viewFromWorld);
    cameraUniforms.viewFromClip  = hlslpp::inverse(camera.projFromView);
    commandList->writeBuffer(_cameraUniformsBuffer.Get(),
                             &cameraUniforms,
                             sizeof(cameraUniforms));

    // ---- Bind + dispatch ----------------------------------------------
    const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(output);
    if (!bindingSet) return;

    // Output image must be in UnorderedAccess so the shader can write
    // it. The caller (or RenderGraph) is responsible for transitioning
    // it to a presentable / copy-source state afterward; that happens
    // in the viewer / headless paths.
    commandList->setTextureState(output, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    nvrhi::rt::State state;
    state.shaderTable = _shaderTable;
    state.bindings    = { bindingSet };
    commandList->setRayTracingState(state);

    nvrhi::rt::DispatchRaysArguments args;
    args.width  = output->getDesc().width;
    args.height = output->getDesc().height;
    args.depth  = 1;
    commandList->dispatchRays(args);
}

}  // namespace pyxis
