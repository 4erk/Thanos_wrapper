#include "RtFallback.h"

#include "../log/Log.h"
#include "../../settings.h"
#define WIN32_LEAN_AND_MEAN
#include <unordered_map>
#include <vector>
#include <mutex>
#include <windows.h>

namespace thanosray::vk::rtfb {

// ============================================================================
// RtFallback.cpp
// LANDMARK RF10: Implementation of minimal RT fallback tracking.
//
// IMPORTANT:
// - This file intentionally avoids creating shaders/pipelines.
// - Phase 0b uses vkCmdClearColorImage ONLY when safe.
// - If we cannot prove safety (missing metadata / missing fns), we only log.
// ============================================================================

static std::mutex g_mtx;
static RealCmdFns g_real;

// Image handle -> create info subset
static std::unordered_map<VkImage, ImageInfo> g_images;

// ImageView -> image + view info
struct ViewInfo {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
};
static std::unordered_map<VkImageView, ViewInfo> g_views;

// DescriptorSet -> (binding -> list of image infos)
// We only care about STORAGE_IMAGE right now.
struct StorageBinding {
    uint32_t binding = 0;
    std::vector<VkDescriptorImageInfo> images; // includes view + layout
};

struct SetState {
    std::unordered_map<uint32_t, StorageBinding> storageImages;
};
static std::unordered_map<VkDescriptorSet, SetState> g_sets;

// CommandBuffer -> last bound RT descriptor sets
struct CmdRtState {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t firstSet = 0;
    std::vector<VkDescriptorSet> sets;
    VkPipeline pipeline = VK_NULL_HANDLE;
};
static std::unordered_map<VkCommandBuffer, CmdRtState> g_cmdRt;

static inline bool IsRayTracingBindPoint(VkPipelineBindPoint bp)
{
    // VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR = 1000165000
    return (int)bp == 1000165000;
}

void SetRealCmdFns(const RealCmdFns& fns)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_real = fns;
}

void OnCreateImage(VkDevice device, VkImage image, const VkImageCreateInfo& ci)
{
    if (!image) return;
    ImageInfo info;
    info.device = device;
    info.format = ci.format;
    info.extent = ci.extent;
    info.usage  = ci.usage;
    info.type   = ci.imageType;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_images[image] = info;
}

void CollectStorageImageTargets(VkCommandBuffer cb, std::vector<StorageImageTarget>& out)
{
    out.clear();
    if (!cb) return;

    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_cmdRt.find(cb);
    if (it == g_cmdRt.end()) return;
    const CmdRtState& cs = it->second;

    for (size_t si = 0; si < cs.sets.size(); ++si) {
        VkDescriptorSet ds = cs.sets[si];
        if (!ds) continue;

        auto sit = g_sets.find(ds);
        if (sit == g_sets.end()) continue;

        for (const auto& kv : sit->second.storageImages) {
            const StorageBinding& sb = kv.second;
            for (const VkDescriptorImageInfo& di : sb.images) {
                if (!di.imageView) continue;

                StorageImageTarget t{};
                t.di = di;

                auto vit = g_views.find(di.imageView);
                if (vit != g_views.end()) {
                    t.image = vit->second.image;
                }
                auto iit = g_images.find(t.image);
                if (iit != g_images.end()) {
                    t.info = iit->second;
                }

                out.push_back(t);
            }
        }
    }
}

void OnDestroyImage(VkImage image)
{
    if (!image) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_images.erase(image);
}

void OnCreateImageView(VkImageView view, const VkImageViewCreateInfo& ci)
{
    if (!view) return;
    ViewInfo v;
    v.image  = ci.image;
    v.format = ci.format;
    v.type   = ci.viewType;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_views[view] = v;
}

void OnDestroyImageView(VkImageView view)
{
    if (!view) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_views.erase(view);
}

static inline bool IsStorageImageWrite(const VkWriteDescriptorSet& w)
{
    return w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

void OnUpdateDescriptorSets(uint32_t writeCount, const VkWriteDescriptorSet* writes)
{
    if (!writes || writeCount == 0) return;

    std::lock_guard<std::mutex> lock(g_mtx);
    for (uint32_t i = 0; i < writeCount; ++i) {
        const VkWriteDescriptorSet& w = writes[i];
        if (!w.dstSet) continue;
        if (!IsStorageImageWrite(w)) continue;
        if (!w.pImageInfo || w.descriptorCount == 0) continue;

        SetState& ss = g_sets[w.dstSet];
        StorageBinding& sb = ss.storageImages[w.dstBinding];
        sb.binding = w.dstBinding;

        // Handle array updates
        uint32_t needed = w.dstArrayElement + w.descriptorCount;
        if (sb.images.size() < needed) sb.images.resize(needed);
        for (uint32_t j = 0; j < w.descriptorCount; ++j) {
            sb.images[w.dstArrayElement + j] = w.pImageInfo[j];
        }
    }
}

void OnCmdBindDescriptorSets(VkCommandBuffer cb,
    VkPipelineBindPoint bindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t setCount,
    const VkDescriptorSet* pSets)
{
    if (!cb || !pSets || setCount == 0) return;
    if (!IsRayTracingBindPoint(bindPoint)) return;

    std::lock_guard<std::mutex> lock(g_mtx);
    CmdRtState& cs = g_cmdRt[cb];
    cs.layout   = layout;
    cs.firstSet = firstSet;

    uint32_t needed = firstSet + setCount;
    if (cs.sets.size() < needed) cs.sets.resize(needed, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < setCount; ++i) {
        cs.sets[firstSet + i] = pSets[i];
    }
}

void OnCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipeline pipeline)
{
    if (!cb) return;
    if (!IsRayTracingBindPoint(bindPoint)) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_cmdRt[cb].pipeline = pipeline;
}

static void LogCandidates_NoLock(VkCommandBuffer cb, const CmdRtState& cs)
{
    for (size_t si = 0; si < cs.sets.size(); ++si) {
        VkDescriptorSet ds = cs.sets[si];
        if (!ds) continue;

        auto it = g_sets.find(ds);
        if (it == g_sets.end()) continue;

        const SetState& ss = it->second;
        for (const auto& kv : ss.storageImages) {
            const StorageBinding& sb = kv.second;

            for (size_t ai = 0; ai < sb.images.size(); ++ai) {
                const VkDescriptorImageInfo& di = sb.images[ai];

                VkImageView view = di.imageView;
                VkImage img = VK_NULL_HANDLE;
                VkFormat vf = VK_FORMAT_UNDEFINED;

                auto vit = g_views.find(view);
                if (vit != g_views.end()) {
                    img = vit->second.image;
                    vf  = vit->second.format;
                }

                ImageInfo ii{};
                bool hasImg = false;
                auto iit = g_images.find(img);
                if (iit != g_images.end()) {
                    ii = iit->second;
                    hasImg = true;
                }

                thanosray::log::Logf(
                    "[RTFB] CB=%p set=%zu binding=%u[%zu] view=%p img=%p layout=%d vfmt=%d extent=%ux%u usage=0x%08X",
                    (void*)cb,
                    si,
                    sb.binding,
                    ai,
                    (void*)view,
                    (void*)img,
                    (int)di.imageLayout,
                    (int)vf,
                    hasImg ? ii.extent.width  : 0,
                    hasImg ? ii.extent.height : 0,
                    hasImg ? (uint32_t)ii.usage : 0u);
            }
        }
    }
}

static bool TryAmbientClear_NoLock(VkCommandBuffer cb, const VkDescriptorImageInfo& di)
{
    if (!g_real.vkCmdPipelineBarrier || !g_real.vkCmdClearColorImage) return false;
    if (!di.imageView) return false;

    auto vit = g_views.find(di.imageView);
    if (vit == g_views.end()) return false;

    VkImage img = vit->second.image;
    if (!img) return false;

    auto iit = g_images.find(img);
    if (iit == g_images.end()) return false;

    const ImageInfo& ii = iit->second;

    // Safety: only if TRANSFER_DST is set.
    if ((ii.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) return false;

    // INI gate
// LANDMARK RF-GATE: Safe runtime gate (no Settings_Get dependency)
// Enable by setting environment variable: THANOS_FALLBACK_GI=1
    char buf[8] = {};
    DWORD n = GetEnvironmentVariableA("THANOS_FALLBACK_GI", buf, (DWORD)sizeof(buf));
    if (n == 0 || (buf[0] != '1' && buf[0] != 'y' && buf[0] != 'Y' && buf[0] != 't' && buf[0] != 'T')) {
        return false;
    }


    VkImageLayout oldLayout = di.imageLayout;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) return false;

    VkClearColorValue c{};
    c.float32[0] = 0.06f;
    c.float32[1] = 0.065f;
    c.float32[2] = 0.075f;
    c.float32[3] = 1.0f;

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    VkImageMemoryBarrier b0{};
    b0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b0.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b0.oldLayout = oldLayout;
    b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.image = img;
    b0.subresourceRange = range;

    g_real.vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b0);

    g_real.vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);

    VkImageMemoryBarrier b1 = b0;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.newLayout = oldLayout;

    g_real.vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);

    thanosray::log::Logf("[RTFB] Ambient clear applied to img=%p (extent=%ux%u)",
        (void*)img, ii.extent.width, ii.extent.height);
    return true;
}

bool OnCmdTraceRaysKHR(VkCommandBuffer cb)
{
    if (!cb) return false;

    std::lock_guard<std::mutex> lock(g_mtx);

    auto it = g_cmdRt.find(cb);
    if (it == g_cmdRt.end()) {
        static bool once = false;
        if (!once) {
            thanosray::log::Logf("[RTFB] TraceRays: no RT bind state for CB=%p (need vkCmdBindDescriptorSets hook)", (void*)cb);
            once = true;
        }
        return false;
    }

    const CmdRtState& cs = it->second;

    static uint32_t dumpEveryN = 1;
    static std::unordered_map<VkCommandBuffer, uint32_t> seen;
    uint32_t& ctr = seen[cb];
    ctr++;
    if (ctr % dumpEveryN == 1) {
        thanosray::log::Logf("[RTFB] vkCmdTraceRaysKHR hit: CB=%p pipeline=%p sets=%zu",
            (void*)cb, (void*)cs.pipeline, cs.sets.size());
        LogCandidates_NoLock(cb, cs);
    }

    bool did = false;
    for (size_t si = 0; si < cs.sets.size() && !did; ++si) {
        VkDescriptorSet ds = cs.sets[si];
        if (!ds) continue;

        auto sit = g_sets.find(ds);
        if (sit == g_sets.end()) continue;

        for (const auto& kv : sit->second.storageImages) {
            const StorageBinding& sb = kv.second;
            for (const VkDescriptorImageInfo& di : sb.images) {
                if (TryAmbientClear_NoLock(cb, di)) {
                    did = true;
                    break;
                }
            }
            if (did) break;
        }
    }
    return did;
}

} // namespace thanosray::vk::rtfb
