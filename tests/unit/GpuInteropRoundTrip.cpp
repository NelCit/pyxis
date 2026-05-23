// RFC 0004 — GPU external-memory interop round-trip.
//
// Verifies the Omniverse Kit viewport handoff primitive on real hardware
// WITHOUT needing the Kit SDK: export an AOV image + timeline semaphore from
// the Pyxis Vulkan device (device A), then import them into a *second* logical
// Vulkan device (device B) standing in for Kit, and prove device B reads back
// the exact pixels device A wrote — zero host copy of the image content across
// the device boundary.
//
// The cross-device protocol mirrors what the real Kit importer must do:
//   A: clear image -> release queue-family ownership to VK_QUEUE_FAMILY_EXTERNAL
//      -> signal exported timeline semaphore = 1
//   B: import memory + semaphore -> wait timeline = 1 -> acquire ownership from
//      VK_QUEUE_FAMILY_EXTERNAL -> copy image to host-visible buffer -> read back
//
// Skips cleanly (not fails) on a CPU-only CI box or an adapter without
// VK_KHR_external_memory_win32, so it never breaks non-GPU runs.

#include <gtest/gtest.h>

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Interop/GpuInteropExporter.h>
#include <Pyxis/Platform/Interop/GpuInteropImporter.h>

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace pyxis;

namespace {

// A Pyxis headless device + an exporter on it, or empty if unavailable. Lets
// the lightweight export-validation tests share one bringup path and skip
// cleanly on CPU-only CI / adapters without external-memory.
struct InteropHarness {
  std::unique_ptr<IDeviceManager> dm;
  std::unique_ptr<GpuInteropExporter> exporter;
  [[nodiscard]] bool Ready() const { return dm && exporter; }
};

uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
      return i;
  }
  return UINT32_MAX;
}

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
// Clear colour (1.0, 0.5, 0.25, 1.0) -> 8-bit UNORM bytes.
constexpr uint8_t kExpectR = 255;
constexpr uint8_t kExpectG = 128;  // round(0.5*255) == 128
constexpr uint8_t kExpectB = 64;   // round(0.25*255) == 64
constexpr uint8_t kExpectA = 255;

InteropHarness MakeHarness() {
  InteropHarness h;
  DeviceCreationParams params;
  params.framesInFlight = 1;
  params.applicationName = "pyxis_interop_test";
  const Resolution res{kWidth, kHeight};
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  h.dm.reset(CreateHeadlessDeviceManager(params, res, &status));
  if (!h.dm || status != DeviceManagerCreateStatus::Ok || h.dm->GetDevice() == nullptr)
    return h;  // dm without exporter -> Ready() == false -> test skips.
  h.exporter = GpuInteropExporter::Create(h.dm->GetVulkanContext(), h.dm->GetDevice());
  return h;
}

}  // namespace

TEST(GpuInteropRoundTrip, ExportImportTimelineBlit) {
  // --- Device A: the Pyxis headless device --------------------------------
  DeviceCreationParams params;
  params.framesInFlight = 1;
  params.applicationName = "pyxis_interop_test";
  const Resolution res{kWidth, kHeight};
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  std::unique_ptr<IDeviceManager> dm(CreateHeadlessDeviceManager(params, res, &status));
  if (!dm || status != DeviceManagerCreateStatus::Ok || dm->GetDevice() == nullptr)
    GTEST_SKIP() << "No Vulkan device (CPU-only CI).";

  const VulkanContext ctx = dm->GetVulkanContext();
  auto physA = static_cast<VkPhysicalDevice>(ctx.physicalDevice);
  auto devA = static_cast<VkDevice>(ctx.device);
  auto queueA = static_cast<VkQueue>(ctx.graphicsQueue);
  const uint32_t family = ctx.graphicsFamily;

  auto exporter = GpuInteropExporter::Create(ctx, dm->GetDevice());
  if (!exporter)
    GTEST_SKIP() << "VK_KHR_external_memory_win32 not supported on this adapter.";

  // --- Export the shared image + timeline semaphore -----------------------
  const ExportedImage shared =
      exporter->CreateExportableImage(kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM, true);
  ASSERT_TRUE(shared.IsValid());
  ASSERT_EQ(shared.width, kWidth);
  ASSERT_GT(shared.allocationSize, 0u);
  ASSERT_TRUE(shared.dedicatedAllocation);

  const ExportedSemaphore timeline = exporter->CreateExportableTimelineSemaphore();
  ASSERT_TRUE(timeline.IsValid());

  const DeviceUuid uuidA = exporter->GetDeviceUuid();
  ASSERT_TRUE(uuidA.valid);

  auto imgA = static_cast<VkImage>(shared.texture->getNativeObject(nvrhi::ObjectTypes::VK_Image));
  ASSERT_NE(imgA, VkImage(VK_NULL_HANDLE));

  // --- A-side: clear + release ownership to EXTERNAL ----------------------
  VkCommandPool poolA = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCi.queueFamilyIndex = family;
  poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  ASSERT_EQ(vkCreateCommandPool(devA, &poolCi, nullptr, &poolA), VK_SUCCESS);

  VkCommandBuffer cmdA = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo cbAi{};
  cbAi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbAi.commandPool = poolA;
  cbAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAi.commandBufferCount = 1;
  ASSERT_EQ(vkAllocateCommandBuffers(devA, &cbAi, &cmdA), VK_SUCCESS);

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cmdA, &begin), VK_SUCCESS);

  const VkImageSubresourceRange fullColor{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkImageMemoryBarrier toDst{};
  toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = imgA;
  toDst.subresourceRange = fullColor;
  vkCmdPipelineBarrier(cmdA, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &toDst);

  const VkClearColorValue clear{{1.0f, 0.5f, 0.25f, 1.0f}};
  vkCmdClearColorImage(cmdA, imgA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &fullColor);

  // Release ownership to EXTERNAL; B will acquire with the matching layouts.
  VkImageMemoryBarrier release{};
  release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  release.dstAccessMask = 0;
  release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  release.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  release.srcQueueFamilyIndex = family;
  release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  release.image = imgA;
  release.subresourceRange = fullColor;
  vkCmdPipelineBarrier(cmdA, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &release);

  ASSERT_EQ(vkEndCommandBuffer(cmdA), VK_SUCCESS);

  VkSubmitInfo submitA{};
  submitA.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitA.commandBufferCount = 1;
  submitA.pCommandBuffers = &cmdA;
  ASSERT_EQ(vkQueueSubmit(queueA, 1, &submitA, VK_NULL_HANDLE), VK_SUCCESS);

  // Publish "frame ready" — ordered after the clear submit on the same queue.
  exporter->SignalTimeline(timeline, 1);

  // --- Device B: the stand-in for Kit's Vulkan device ---------------------
  const char* extB[] = {
      "VK_KHR_external_memory",        "VK_KHR_external_memory_win32",
      "VK_KHR_external_semaphore",     "VK_KHR_external_semaphore_win32",
      VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
  };
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkPhysicalDeviceTimelineSemaphoreFeatures tlFeat{};
  tlFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  tlFeat.timelineSemaphore = VK_TRUE;

  VkDeviceCreateInfo devCi{};
  devCi.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  devCi.pNext = &tlFeat;
  devCi.queueCreateInfoCount = 1;
  devCi.pQueueCreateInfos = &qci;
  devCi.enabledExtensionCount = static_cast<uint32_t>(std::size(extB));
  devCi.ppEnabledExtensionNames = extB;

  VkDevice devB = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateDevice(physA, &devCi, nullptr, &devB), VK_SUCCESS);
  VkQueue queueB = VK_NULL_HANDLE;
  vkGetDeviceQueue(devB, family, 0, &queueB);

  // Import the shared image + timeline semaphore via the shipping
  // GpuInteropImporter — the SAME class the Kit render pass uses. Device B
  // (built above) stands in for Kit's Vulkan device.
  VulkanContext ctxB{};
  ctxB.instance = ctx.instance;
  ctxB.physicalDevice = physA;
  ctxB.device = devB;
  ctxB.graphicsQueue = queueB;
  ctxB.graphicsFamily = family;
  auto importer = GpuInteropImporter::Create(ctxB);
  ASSERT_NE(importer, nullptr);

  const ImportedImage importedB = importer->ImportImage(
      shared.memoryHandle, shared.allocationSize, kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM, true);
  ASSERT_TRUE(importedB.IsValid());
  auto imgB = static_cast<VkImage>(importedB.image);

  auto semB = static_cast<VkSemaphore>(importer->ImportTimelineSemaphore(timeline.handle));
  ASSERT_NE(semB, VkSemaphore(VK_NULL_HANDLE));

  // Host-visible readback buffer on device B.
  const VkDeviceSize byteCount = VkDeviceSize(kWidth) * kHeight * 4u;
  VkBufferCreateInfo bufCi{};
  bufCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufCi.size = byteCount;
  bufCi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer bufB = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateBuffer(devB, &bufCi, nullptr, &bufB), VK_SUCCESS);
  VkMemoryRequirements bufReq{};
  vkGetBufferMemoryRequirements(devB, bufB, &bufReq);
  VkMemoryAllocateInfo bufAi{};
  bufAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  bufAi.allocationSize = bufReq.size;
  bufAi.memoryTypeIndex = FindMemoryType(
      physA, bufReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_NE(bufAi.memoryTypeIndex, UINT32_MAX);
  VkDeviceMemory bufMem = VK_NULL_HANDLE;
  ASSERT_EQ(vkAllocateMemory(devB, &bufAi, nullptr, &bufMem), VK_SUCCESS);
  ASSERT_EQ(vkBindBufferMemory(devB, bufB, bufMem, 0), VK_SUCCESS);

  // B-side command buffer: acquire from EXTERNAL + copy image -> buffer.
  VkCommandPool poolB = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo poolCiB = poolCi;
  ASSERT_EQ(vkCreateCommandPool(devB, &poolCiB, nullptr, &poolB), VK_SUCCESS);
  VkCommandBuffer cmdB = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo cbAiB = cbAi;
  cbAiB.commandPool = poolB;
  ASSERT_EQ(vkAllocateCommandBuffers(devB, &cbAiB, &cmdB), VK_SUCCESS);
  ASSERT_EQ(vkBeginCommandBuffer(cmdB, &begin), VK_SUCCESS);

  VkImageMemoryBarrier acquire{};
  acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  acquire.srcAccessMask = 0;
  acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  acquire.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  acquire.dstQueueFamilyIndex = family;
  acquire.image = imgB;
  acquire.subresourceRange = fullColor;
  vkCmdPipelineBarrier(cmdB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &acquire);

  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kWidth, kHeight, 1};
  vkCmdCopyImageToBuffer(cmdB, imgB, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bufB, 1, &copy);
  ASSERT_EQ(vkEndCommandBuffer(cmdB), VK_SUCCESS);

  // Submit B, waiting on the cross-device timeline reaching 1.
  const uint64_t waitValue = 1;
  VkTimelineSemaphoreSubmitInfo tlWait{};
  tlWait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  tlWait.waitSemaphoreValueCount = 1;
  tlWait.pWaitSemaphoreValues = &waitValue;
  const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submitB{};
  submitB.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitB.pNext = &tlWait;
  submitB.waitSemaphoreCount = 1;
  submitB.pWaitSemaphores = &semB;
  submitB.pWaitDstStageMask = &waitStage;
  submitB.commandBufferCount = 1;
  submitB.pCommandBuffers = &cmdB;

  VkFenceCreateInfo fenceCi{};
  fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fenceB = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(devB, &fenceCi, nullptr, &fenceB), VK_SUCCESS);
  ASSERT_EQ(vkQueueSubmit(queueB, 1, &submitB, fenceB), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(devB, 1, &fenceB, VK_TRUE, 5'000'000'000ull), VK_SUCCESS);

  // --- Verify: device B read back exactly what device A wrote -------------
  void* mapped = nullptr;
  ASSERT_EQ(vkMapMemory(devB, bufMem, 0, byteCount, 0, &mapped), VK_SUCCESS);
  const auto* px = static_cast<const uint8_t*>(mapped);
  EXPECT_NEAR(px[0], kExpectR, 2);
  EXPECT_NEAR(px[1], kExpectG, 2);
  EXPECT_NEAR(px[2], kExpectB, 2);
  EXPECT_NEAR(px[3], kExpectA, 2);
  // A pixel in the middle, too — proves the whole surface aliases, not just (0,0).
  const size_t mid = (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4u;
  EXPECT_NEAR(px[mid + 0], kExpectR, 2);
  EXPECT_NEAR(px[mid + 1], kExpectG, 2);
  EXPECT_NEAR(px[mid + 2], kExpectB, 2);
  vkUnmapMemory(devB, bufMem);

  // --- Cleanup (device B owns these; the exporter cleans up device A) -----
  vkDeviceWaitIdle(devB);
  vkDestroyFence(devB, fenceB, nullptr);
  vkDestroyCommandPool(devB, poolB, nullptr);
  vkDestroyBuffer(devB, bufB, nullptr);
  vkFreeMemory(devB, bufMem, nullptr);
  importer.reset();  // owns imgB / memB / semB; destroy before the device.
  vkDestroyDevice(devB, nullptr);
  vkDeviceWaitIdle(devA);
  vkDestroyCommandPool(devA, poolA, nullptr);
}

// --- Export-validation tests (no second device; verify the export API) -----

// Every §25.I.1 AOV format we claim to support exports a valid, adoptable image.
TEST(GpuInteropExport, SupportsAovFormats) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";

  struct Case {
    uint32_t vkFormat;
    bool renderTarget;
  };
  const Case cases[] = {
      {VK_FORMAT_R16G16B16A16_SFLOAT, true},   // color
      {VK_FORMAT_R32_SFLOAT, false},           // depth
      {VK_FORMAT_R32_UINT, false},             // id AOVs
      {VK_FORMAT_R8G8B8A8_UNORM, true},        // 8-bit color
  };
  for (const Case& test : cases) {
    const ExportedImage img =
        harness.exporter->CreateExportableImage(kWidth, kHeight, test.vkFormat, test.renderTarget);
    EXPECT_TRUE(img.IsValid()) << "vkFormat=" << test.vkFormat;
    EXPECT_EQ(img.vkFormat, test.vkFormat);
    EXPECT_EQ(img.width, kWidth);
    EXPECT_EQ(img.height, kHeight);
    EXPECT_GT(img.allocationSize, 0u);
    EXPECT_TRUE(img.dedicatedAllocation);
    EXPECT_NE(img.texture, nullptr);
    EXPECT_NE(img.memoryHandle, nullptr);
  }
}

// An unsupported VkFormat fails cleanly (no crash, invalid result), per the
// "agree on the exact format or fail" contract — never a silent substitution.
TEST(GpuInteropExport, UnsupportedFormatFailsCleanly) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";

  const ExportedImage img =
      harness.exporter->CreateExportableImage(kWidth, kHeight, VK_FORMAT_R8_UNORM, false);
  EXPECT_FALSE(img.IsValid());
  EXPECT_EQ(img.texture, nullptr);
  EXPECT_EQ(img.memoryHandle, nullptr);
}

// The device UUID is stable, valid, and non-zero — it is the value the Kit
// importer compares against its own device before importing (RFC 0004 §4).
TEST(GpuInteropExport, DeviceUuidStableAndValid) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";

  const DeviceUuid first = harness.exporter->GetDeviceUuid();
  const DeviceUuid second = harness.exporter->GetDeviceUuid();
  ASSERT_TRUE(first.valid);
  ASSERT_TRUE(second.valid);
  EXPECT_EQ(std::memcmp(first.bytes, second.bytes, sizeof(first.bytes)), 0);
  bool anyNonZero = false;
  for (uint8_t byte : first.bytes)
    anyNonZero = anyNonZero || (byte != 0);
  EXPECT_TRUE(anyNonZero) << "deviceUUID should not be all-zero";
}

// Two exported images / semaphores from one exporter get distinct handles, so a
// multi-AOV frame can share several surfaces at once.
TEST(GpuInteropExport, MultipleResourcesGetDistinctHandles) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";

  const ExportedImage imgA =
      harness.exporter->CreateExportableImage(kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM, true);
  const ExportedImage imgB =
      harness.exporter->CreateExportableImage(kWidth, kHeight, VK_FORMAT_R32_SFLOAT, false);
  ASSERT_TRUE(imgA.IsValid());
  ASSERT_TRUE(imgB.IsValid());
  EXPECT_NE(imgA.memoryHandle, imgB.memoryHandle);
  EXPECT_NE(imgA.texture, imgB.texture);

  const ExportedSemaphore semA = harness.exporter->CreateExportableTimelineSemaphore();
  const ExportedSemaphore semB = harness.exporter->CreateExportableTimelineSemaphore();
  ASSERT_TRUE(semA.IsValid());
  ASSERT_TRUE(semB.IsValid());
  EXPECT_NE(semA.handle, semB.handle);
}

// --- GpuInteropImporter direct tests + exporter edge cases -----------------

// The importer creates on a device that enabled the interop extensions.
TEST(GpuInteropImport, CreateSucceedsOnInteropDevice) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";
  auto importer = GpuInteropImporter::Create(harness.dm->GetVulkanContext());
  EXPECT_NE(importer, nullptr);
}

// Importing a bogus Win32 memory handle must fail cleanly (invalid result),
// never crash — robustness against a stale/wrong handle from the wire.
TEST(GpuInteropImport, RejectsInvalidMemoryHandle) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";
  auto importer = GpuInteropImporter::Create(harness.dm->GetVulkanContext());
  ASSERT_NE(importer, nullptr);
  const ImportedImage img = importer->ImportImage(reinterpret_cast<void*>(0xDEAD), 1u << 20, kWidth,
                                                  kHeight, VK_FORMAT_R8G8B8A8_UNORM, true);
  EXPECT_FALSE(img.IsValid());
}

// Importing a bogus semaphore handle must fail cleanly (null), not crash.
TEST(GpuInteropImport, RejectsInvalidSemaphoreHandle) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";
  auto importer = GpuInteropImporter::Create(harness.dm->GetVulkanContext());
  ASSERT_NE(importer, nullptr);
  EXPECT_EQ(importer->ImportTimelineSemaphore(reinterpret_cast<void*>(0xBADF00D)), nullptr);
}

// Signalling the exported timeline to monotonically increasing values is safe
// (the cross-device wait is covered by GpuInteropRoundTrip).
TEST(GpuInteropExport, TimelineSignalIsCallableAndMonotonic) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";
  const ExportedSemaphore sem = harness.exporter->CreateExportableTimelineSemaphore();
  ASSERT_TRUE(sem.IsValid());
  for (uint64_t value = 1; value <= 4; ++value)
    harness.exporter->SignalTimeline(sem, value);
  harness.dm->WaitIdle();
  SUCCEED();
}

// Signalling an unknown (not-from-this-exporter) semaphore is a no-op, not a crash.
TEST(GpuInteropExport, SignalUnknownSemaphoreIsNoOp) {
  InteropHarness harness = MakeHarness();
  if (!harness.Ready())
    GTEST_SKIP() << "No device / external-memory interop unavailable.";
  ExportedSemaphore bogus{};
  bogus.handle = reinterpret_cast<void*>(0x1234);
  harness.exporter->SignalTimeline(bogus, 1);  // must not crash.
  SUCCEED();
}
