#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace thanosray::vk::rtfb {

// ============================================================================
// RtFallback.h
// LANDMARK RF00: Minimal, fail-open RT fallback helpers.
//
// Phase 0 (safe):
//   - Track descriptor sets + image/view metadata.
//   - At vkCmdTraceRaysKHR, log candidate STORAGE_IMAGE outputs.
//
// Phase 0b (still safe, gated):
//   - If a candidate output image supports VK_IMAGE_USAGE_TRANSFER_DST_BIT,
//     we can clear it to a small ambient value to prove wiring.
//     (No shader modules, no pipeline creation.)
//
// Everything is fail-open: if we can't prove safety, we do nothing.
// ============================================================================

struct ImageInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkImageUsageFlags usage = 0;
    VkImageType type = VK_IMAGE_TYPE_2D;
};

// ---- Image tracking (CreateImage / CreateImageView) ----
void OnCreateImage(VkDevice device, VkImage image, const VkImageCreateInfo& ci);
void OnDestroyImage(VkImage image);

void OnCreateImageView(VkImageView view, const VkImageViewCreateInfo& ci);
void OnDestroyImageView(VkImageView view);

// ---- Descriptor tracking (UpdateDescriptorSets / UpdateDescriptorSetWithTemplate) ----
void OnUpdateDescriptorSets(uint32_t writeCount, const VkWriteDescriptorSet* writes);

// ---- Command buffer bind tracking (CmdBindDescriptorSets / CmdBindPipeline) ----
void OnCmdBindDescriptorSets(VkCommandBuffer cb,
    VkPipelineBindPoint bindPoint,
    VkPipelineLayout layout, uint32_t firstSet,uint32_t setCount,   const VkDescriptorSet* pSets);

void OnCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipeline pipeline);

// ---- TraceRays interception point ----
// Returns true if we performed an ambient clear on at least one output.
bool OnCmdTraceRaysKHR(VkCommandBuffer cb);

// ---- Query helpers (used by VkHooks to run compute fallback on the same RT outputs) ----
struct StorageImageTarget {
    VkDescriptorImageInfo di{};  // view + layout
    VkImage image = VK_NULL_HANDLE;
    ImageInfo info{};            // includes device/extent/usage
};

// Collect candidate STORAGE_IMAGE targets bound at the RT bind point for this CB.
// Fail-open: returns empty if we don't know.
void CollectStorageImageTargets(VkCommandBuffer cb, std::vector<StorageImageTarget>& out);

// Provide the real Vulkan entrypoints we may need for safe clears/barriers.
struct RealCmdFns {
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
};

void SetRealCmdFns(const RealCmdFns& fns);

} // namespace thanosray::vk::rtfb
