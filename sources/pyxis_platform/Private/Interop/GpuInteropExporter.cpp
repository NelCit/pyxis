// Pyxis platform — GPU external-memory exporter (Win32). RFC 0004.
//
// See Public/Pyxis/Platform/Interop/GpuInteropExporter.h for the contract.
// This TU owns the Win32-specific include order (VK_USE_PLATFORM_WIN32_KHR
// must precede every Vulkan include) so the public header stays Win32-free.

// VK_USE_PLATFORM_WIN32_KHR is defined project-wide for pyxis_platform (the
// device managers use the Win32 surface), so vulkan.h already exposes the
// Win32 external-memory entry points; vulkan_win32.h is pulled explicitly for
// clarity of intent.
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <Pyxis/Platform/Interop/GpuInteropExporter.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nvrhi/nvrhi.h>

#include <cstring>
#include <vector>

namespace pyxis {
namespace {

// §25.I.1 AOV formats we support sharing, mapped VkFormat <-> nvrhi::Format.
// Anything outside this table fails CreateExportableImage with a logged error
// rather than guessing — the importer must agree on the exact format.
struct FormatPair {
  uint32_t vk;
  nvrhi::Format nvrhi;
};
constexpr FormatPair SUPPORTED_FORMATS[] = {
    {VK_FORMAT_R16G16B16A16_SFLOAT, nvrhi::Format::RGBA16_FLOAT},  // color (§25.I.1)
    {VK_FORMAT_R8G8B8A8_UNORM, nvrhi::Format::RGBA8_UNORM},        // 8-bit color / test
    {VK_FORMAT_R32_SFLOAT, nvrhi::Format::R32_FLOAT},              // depth (§25.I.1)
    {VK_FORMAT_R32_UINT, nvrhi::Format::R32_UINT},                 // id AOVs (§25.I.1)
};

[[nodiscard]] bool TryMapFormat(uint32_t vkFormat, nvrhi::Format& out) noexcept {
  for (const FormatPair& pair : SUPPORTED_FORMATS)
    if (pair.vk == vkFormat) {
      out = pair.nvrhi;
      return true;
    }
  return false;
}

[[nodiscard]] uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                                      VkMemoryPropertyFlags wanted) noexcept {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const bool typeOk = (typeBits & (1u << i)) != 0u;
    const bool propsOk = (props.memoryTypes[i].propertyFlags & wanted) == wanted;
    if (typeOk && propsOk)
      return i;
  }
  return UINT32_MAX;
}

}  // namespace

struct GpuInteropExporter::Impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  nvrhi::IDevice* nvrhiDevice = nullptr;

  PFN_vkGetMemoryWin32HandleKHR getMemoryHandle = nullptr;
  PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreHandle = nullptr;

  // Resources the exporter owns and tears down. nvrhi::TextureHandle keeps the
  // adopted wrapper alive; the VkImage/VkDeviceMemory/HANDLE are destroyed by
  // us (NVRHI does not own adopted-native memory).
  struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    HANDLE win32 = nullptr;
    nvrhi::TextureHandle texture;
  };
  std::vector<OwnedImage> images;

  struct OwnedSemaphore {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    HANDLE win32 = nullptr;
  };
  std::vector<OwnedSemaphore> semaphores;

  ~Impl() {
    if (device == VK_NULL_HANDLE)
      return;
    vkDeviceWaitIdle(device);
    for (OwnedImage& img : images) {
      img.texture = nullptr;  // drop NVRHI wrapper before destroying the image.
      if (img.image != VK_NULL_HANDLE)
        vkDestroyImage(device, img.image, nullptr);
      if (img.memory != VK_NULL_HANDLE)
        vkFreeMemory(device, img.memory, nullptr);
      if (img.win32 != nullptr)
        ::CloseHandle(img.win32);
    }
    for (const OwnedSemaphore& sem : semaphores) {
      if (sem.semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(device, sem.semaphore, nullptr);
      if (sem.win32 != nullptr)
        ::CloseHandle(sem.win32);
    }
  }
};

GpuInteropExporter::GpuInteropExporter() : _impl(std::make_unique<Impl>()) {}
GpuInteropExporter::~GpuInteropExporter() = default;

std::unique_ptr<GpuInteropExporter> GpuInteropExporter::Create(
    const VulkanContext& ctx, nvrhi::IDevice* nvrhiDevice) noexcept {
  auto& log = Logging::Get();
  if (ctx.device == nullptr || nvrhiDevice == nullptr) {
    log.Error(log::PLATFORM, "GpuInteropExporter::Create: null device");
    return nullptr;
  }

  auto exporter = std::unique_ptr<GpuInteropExporter>(new GpuInteropExporter());
  Impl& impl = *exporter->_impl;
  impl.instance = static_cast<VkInstance>(ctx.instance);
  impl.physicalDevice = static_cast<VkPhysicalDevice>(ctx.physicalDevice);
  impl.device = static_cast<VkDevice>(ctx.device);
  impl.queue = static_cast<VkQueue>(ctx.graphicsQueue);
  impl.queueFamily = ctx.graphicsFamily;
  impl.nvrhiDevice = nvrhiDevice;

  impl.getMemoryHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
      vkGetDeviceProcAddr(impl.device, "vkGetMemoryWin32HandleKHR"));
  impl.getSemaphoreHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
      vkGetDeviceProcAddr(impl.device, "vkGetSemaphoreWin32HandleKHR"));
  if (impl.getMemoryHandle == nullptr || impl.getSemaphoreHandle == nullptr) {
    log.Error(log::PLATFORM,
              "GpuInteropExporter::Create: VK_KHR_external_memory_win32 / "
              "external_semaphore_win32 not enabled on this device; interop unavailable");
    return nullptr;
  }
  return exporter;
}

DeviceUuid GpuInteropExporter::GetDeviceUuid() const noexcept {
  DeviceUuid uuid{};
  VkPhysicalDeviceIDProperties idProps{};
  idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &idProps;
  vkGetPhysicalDeviceProperties2(_impl->physicalDevice, &props2);
  std::memcpy(uuid.bytes, idProps.deviceUUID, sizeof(uuid.bytes));
  uuid.valid = true;
  return uuid;
}

ExportedImage GpuInteropExporter::CreateExportableImage(uint32_t width, uint32_t height,
                                                        uint32_t vkFormat,
                                                        bool isColorRenderTarget) noexcept {
  auto& log = Logging::Get();
  ExportedImage out{};

  nvrhi::Format nvrhiFormat = nvrhi::Format::UNKNOWN;
  if (!TryMapFormat(vkFormat, nvrhiFormat)) {
    log.Error(log::PLATFORM, std::string{"GpuInteropExporter: unsupported VkFormat "}
                                 + std::to_string(vkFormat) + " for sharing (§25.I.1)");
    return out;
  }

  Impl& impl = *_impl;

  // 1. Image, flagged exportable.
  VkExternalMemoryImageCreateInfo extImg{};
  extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
  if (isColorRenderTarget)
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

  VkImageCreateInfo imgInfo{};
  imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.pNext = &extImg;
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.format = static_cast<VkFormat>(vkFormat);
  imgInfo.extent = {width, height, 1};
  imgInfo.mipLevels = 1;
  imgInfo.arrayLayers = 1;
  imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage = usage;
  imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  Impl::OwnedImage owned{};
  if (vkCreateImage(impl.device, &imgInfo, nullptr, &owned.image) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropExporter: vkCreateImage failed");
    return out;
  }

  // 2. Dedicated, exportable memory. OPAQUE_WIN32 export on NVIDIA wants a
  // dedicated allocation; we always use one for shared images.
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(impl.device, owned.image, &req);
  const uint32_t typeIndex =
      FindMemoryType(impl.physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (typeIndex == UINT32_MAX) {
    log.Error(log::PLATFORM, "GpuInteropExporter: no DEVICE_LOCAL memory type for shared image");
    vkDestroyImage(impl.device, owned.image, nullptr);
    return out;
  }

  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = owned.image;

  VkExportMemoryAllocateInfo exportAlloc{};
  exportAlloc.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exportAlloc.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  exportAlloc.pNext = &dedicated;

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &exportAlloc;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = typeIndex;

  if (vkAllocateMemory(impl.device, &alloc, nullptr, &owned.memory) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropExporter: vkAllocateMemory (exportable) failed");
    vkDestroyImage(impl.device, owned.image, nullptr);
    return out;
  }
  if (vkBindImageMemory(impl.device, owned.image, owned.memory, 0) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropExporter: vkBindImageMemory failed");
    vkFreeMemory(impl.device, owned.memory, nullptr);
    vkDestroyImage(impl.device, owned.image, nullptr);
    return out;
  }

  // 3. Export the Win32 handle.
  VkMemoryGetWin32HandleInfoKHR getHandle{};
  getHandle.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
  getHandle.memory = owned.memory;
  getHandle.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  if (impl.getMemoryHandle(impl.device, &getHandle, &owned.win32) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropExporter: vkGetMemoryWin32HandleKHR failed");
    vkFreeMemory(impl.device, owned.memory, nullptr);
    vkDestroyImage(impl.device, owned.image, nullptr);
    return out;
  }

  // 4. Adopt the VkImage into NVRHI so the renderer can target it.
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nvrhiFormat;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isRenderTarget = isColorRenderTarget;
  desc.isUAV = isColorRenderTarget;
  desc.isShaderResource = true;
  desc.debugName = "interop.exportedImage";
  desc.initialState =
      isColorRenderTarget ? nvrhi::ResourceStates::RenderTarget : nvrhi::ResourceStates::ShaderResource;
  desc.keepInitialState = true;
  owned.texture = impl.nvrhiDevice->createHandleForNativeTexture(
      nvrhi::ObjectTypes::VK_Image, nvrhi::Object(owned.image), desc);
  if (!owned.texture) {
    log.Error(log::PLATFORM, "GpuInteropExporter: createHandleForNativeTexture failed");
    ::CloseHandle(owned.win32);
    vkFreeMemory(impl.device, owned.memory, nullptr);
    vkDestroyImage(impl.device, owned.image, nullptr);
    return out;
  }

  out.texture = owned.texture;
  out.memoryHandle = owned.win32;
  out.allocationSize = req.size;
  out.width = width;
  out.height = height;
  out.vkFormat = vkFormat;
  out.dedicatedAllocation = true;
  impl.images.push_back(std::move(owned));
  return out;
}

ExportedSemaphore GpuInteropExporter::CreateExportableTimelineSemaphore() noexcept {
  auto& log = Logging::Get();
  ExportedSemaphore out{};
  Impl& impl = *_impl;

  VkSemaphoreTypeCreateInfo typeInfo{};
  typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  typeInfo.initialValue = 0;

  VkExportSemaphoreCreateInfo exportInfo{};
  exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
  exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  exportInfo.pNext = &typeInfo;

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semInfo.pNext = &exportInfo;

  Impl::OwnedSemaphore owned{};
  if (vkCreateSemaphore(impl.device, &semInfo, nullptr, &owned.semaphore) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropExporter: vkCreateSemaphore (exportable timeline) failed");
    return out;
  }

  VkSemaphoreGetWin32HandleInfoKHR getHandle{};
  getHandle.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
  getHandle.semaphore = owned.semaphore;
  getHandle.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  if (impl.getSemaphoreHandle(impl.device, &getHandle, &owned.win32) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropExporter: vkGetSemaphoreWin32HandleKHR failed");
    vkDestroySemaphore(impl.device, owned.semaphore, nullptr);
    return out;
  }

  out.handle = owned.win32;
  impl.semaphores.push_back(owned);
  return out;
}

void GpuInteropExporter::SignalTimeline(const ExportedSemaphore& sem, uint64_t value) noexcept {
  const Impl& impl = *_impl;
  VkSemaphore vkSem = VK_NULL_HANDLE;
  for (const Impl::OwnedSemaphore& owned : impl.semaphores)
    if (owned.win32 == sem.handle) {
      vkSem = owned.semaphore;
      break;
    }
  if (vkSem == VK_NULL_HANDLE)
    return;

  VkTimelineSemaphoreSubmitInfo timeline{};
  timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline.signalSemaphoreValueCount = 1;
  timeline.pSignalSemaphoreValues = &value;

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.pNext = &timeline;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &vkSem;
  vkQueueSubmit(impl.queue, 1, &submit, VK_NULL_HANDLE);
}

}  // namespace pyxis
