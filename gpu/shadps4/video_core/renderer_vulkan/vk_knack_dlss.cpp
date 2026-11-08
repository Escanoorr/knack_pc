// SPDX-License-Identifier: GPL-2.0-or-later
// knack: NVIDIA DLSS for Knack at the game's own render size, see vk_knack_dlss.h.
#include "video_core/renderer_vulkan/vk_knack_dlss.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vk_mem_alloc.h>

#include "gow3_settings.h"
#include "video_core/host_shaders/knack_merge_comp.h"
#include "video_core/host_shaders/knack_motion_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

/// Jitter phases: DLSS at 1:1 (DLAA) wants 8.
constexpr u32 JitterPhases = 8;

/// The game's G-buffer pass has six colour targets; the sixth holds its velocities.
constexpr u32 VelocityTarget = 5;

float Halton(u32 index, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

} // namespace

KnackDlss::KnackDlss(const Instance& instance_, Scheduler& scheduler_,
                     VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_} {
    // The compute dispatch that DLSS runs in front of: the first of the game's post-processing
    // chain, found by its shader hash in a frame capture (see docs/knack-dlss.md).
    if (const char* env = std::getenv("GOW3_KNACK_DLSS_BEFORE_CS"); env && env[0]) {
        trigger_hash = std::strtoull(env, nullptr, 16);
    }
    flip_motion_y = EnvFlag("GOW3_KNACK_DLSS_FLIP_MV_Y");
    flip_jitter = EnvFlag("GOW3_KNACK_DLSS_FLIP_JITTER");
    debug_motion = EnvFlag("GOW3_KNACK_DLSS_DEBUG");
}

KnackDlss::~KnackDlss() {
    if (motion_image || motion_pipeline) {
        scheduler.Finish();
    }
    const auto device = instance.GetDevice();
    for (auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

bool KnackDlss::Enabled() const {
    if (failed || !Gow3Settings::Get().knack_dlss.load(std::memory_order_relaxed)) {
        return false;
    }
    const Dlss* dlss = Dlss::Get();
    return dlss && dlss->Available();
}

void KnackDlss::SetState(int state) {
    Gow3Settings::Get().knack_dlss_state.store(state, std::memory_order_relaxed);
}

bool KnackDlss::IsSceneDepth(VideoCore::ImageId depth) const {
    if (!depth_address || !depth) {
        return false;
    }
    const auto& info = texture_cache.GetImage(depth).info;
    return info.guest_address == depth_address && info.size.width == depth_width &&
           info.size.height == depth_height;
}

void KnackDlss::NoteGBuffer(VideoCore::ImageId depth, VideoCore::ImageId velocity) {
    const auto& d = texture_cache.GetImage(depth);
    const auto& v = texture_cache.GetImage(velocity);
    if (v.info.pixel_format != vk::Format::eR16G16Sfloat ||
        v.info.size.width != d.info.size.width || v.info.size.height != d.info.size.height) {
        return;
    }
    depth_id = depth;
    velocity_id = velocity;
    gbuffer_seen = true;
    if (d.info.guest_address != depth_address || d.info.size.width != depth_width ||
        d.info.size.height != depth_height) {
        // A new scene depth (the first scene, or another size): the Z pre-pass of this frame was
        // drawn without jitter and the old history means nothing.
        depth_address = d.info.guest_address;
        depth_width = d.info.size.width;
        depth_height = d.info.size.height;
        frame_valid = false;
        reset_history = true;
        std::printf("KnackDlss: scene found, %ux%u\n", depth_width, depth_height);
    }
}

void KnackDlss::OnTargets(u32 mrt_mask, VideoCore::ImageId color0, VideoCore::ImageId color5,
                          VideoCore::ImageId depth) {
    if (!depth) {
        return;
    }
    if (std::popcount(mrt_mask) >= 5 && (mrt_mask & (1u << VelocityTarget)) && color5) {
        if (depth != depth_id || color5 != velocity_id) {
            NoteGBuffer(depth, color5);
        }
        return;
    }
    // The HDR scene colour: a full-size RGBA16F target drawn with the scene depth (sky, lighting,
    // forward and transparent passes). The last one before the post-processing is the one used.
    if (gbuffer_seen && depth == depth_id && (mrt_mask & 1u) && color0 &&
        std::popcount(mrt_mask) <= 2) {
        const auto& color = texture_cache.GetImage(color0);
        const auto& scene_depth = texture_cache.GetImage(depth);
        if (color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
            color.info.size.width == scene_depth.info.size.width &&
            color.info.size.height == scene_depth.info.size.height) {
            color_id = color0;
        }
    }
}

std::array<float, 2> KnackDlss::DrawJitter(VideoCore::ImageId depth, u32 num_indices,
                                           u32 num_instances) const {
    // Full-screen passes (a triangle, a quad) are not geometry. Every pass of the frame that uses
    // the scene depth until DLSS has run is shifted the same way; the post and the UI are not.
    if (!frame_valid || done || !depth || (num_indices <= 6 && num_instances <= 1) ||
        !IsSceneDepth(depth)) {
        return {};
    }
    return jitter;
}

void KnackDlss::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done || !frame_valid || !gbuffer_seen) {
        return;
    }
    if (!color_id || !depth_id || !velocity_id) {
        return;
    }
    Run();
}

void KnackDlss::OnDisplayPass() {
    if (!gbuffer_seen && !done) {
        return; // HUD draws go to the display buffer too: only the first one ends the frame
    }
    if (gbuffer_seen && !ran_this_frame && !failed) {
        if (++frames_without_run == 180 && frames_run == 0) {
            std::printf("KnackDlss: the 3D scene is drawn but the compute dispatch %08llx that DLSS "
                        "runs in front of never came (or its images do not fit)\n",
                        static_cast<unsigned long long>(trigger_hash));
        }
    }
    if (gbuffer_seen) {
        jitter_index = jitter_index % JitterPhases + 1;
        jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    }
    ran_last_frame = ran_this_frame;
    if (!ran_this_frame) {
        reset_history = true; // a frame without DLSS leaves the history behind
    }
    ran_this_frame = false;
    gbuffer_seen = false;
    done = false;
    frame_valid = depth_address != 0;
    depth_id = {};
    velocity_id = {};
    color_id = {};
    if (!failed) {
        SetState(ran_last_frame ? 2 : 1);
    }
}

vk::ImageView KnackDlss::CachedView(const VideoCore::Image& image, vk::Format format,
                                    vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

bool KnackDlss::EnsureResources(u32 w, u32 h) {
    const auto device = instance.GetDevice();
    if (!motion_pipeline) {
        // Both passes read two sampled images and write one storage image.
        const auto make = [&](std::span<const u32> code, u32 push_size,
                              vk::UniqueDescriptorSetLayout& layout,
                              vk::UniquePipelineLayout& pipeline_layout,
                              vk::UniquePipeline& pipeline, vk::DescriptorType middle) {
            const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
                {.binding = 0,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .descriptorCount = 1,
                 .stageFlags = vk::ShaderStageFlagBits::eCompute},
                {.binding = 1,
                 .descriptorType = middle,
                 .descriptorCount = 1,
                 .stageFlags = vk::ShaderStageFlagBits::eCompute},
                {.binding = 2,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .descriptorCount = 1,
                 .stageFlags = vk::ShaderStageFlagBits::eCompute},
            }};
            layout = Check(device.createDescriptorSetLayoutUnique({
                .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            }));
            const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                              .offset = 0,
                                              .size = push_size};
            pipeline_layout = Check(device.createPipelineLayoutUnique({
                .setLayoutCount = 1,
                .pSetLayouts = &*layout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &range,
            }));
            const auto module = CompileSPV(code, device);
            pipeline = Check(device.createComputePipelineUnique(
                {}, vk::ComputePipelineCreateInfo{
                        .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                  .module = module,
                                  .pName = "main"},
                        .layout = *pipeline_layout,
                    }));
            device.destroyShaderModule(module);
        };
        make(KNACK_MOTION_COMP, sizeof(float) * 2, motion_desc_layout, motion_pipeline_layout,
             motion_pipeline, vk::DescriptorType::eStorageImage);
        make(KNACK_MERGE_COMP, sizeof(u32), merge_desc_layout, merge_pipeline_layout,
             merge_pipeline, vk::DescriptorType::eStorageImage);
    }
    if (motion_image && w == width && h == height) {
        return true;
    }
    if (motion_image) {
        scheduler.Finish(); // the old images may still be in flight
    }
    const auto allocator = instance.GetAllocator();
    motion_view.reset();
    output_view.reset();
    motion_image = VideoCore::UniqueImage(device, allocator);
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_image = VideoCore::UniqueImage(device, allocator);
    output_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_view = Check(device.createImageViewUnique({
        .image = vk::Image(output_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    motion_view = Check(device.createImageViewUnique({
        .image = vk::Image(motion_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    width = w;
    height = h;
    reset_history = true;
    return true;
}

void KnackDlss::Run() {
    done = true; // whatever happens below, once per frame, and the jitter stops here
    auto& color = texture_cache.GetImage(color_id);
    auto& depth = texture_cache.GetImage(depth_id);
    auto& velocity = texture_cache.GetImage(velocity_id);
    const u32 w = color.info.size.width;
    const u32 h = color.info.size.height;
    const auto sized = [&](const VideoCore::Image& image) {
        return image.info.size.width == w && image.info.size.height == h;
    };
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat || !sized(depth) ||
        !sized(velocity) || !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        !(velocity.usage_flags & vk::ImageUsageFlagBits::eSampled)) {
        if (!printed_mismatch) {
            printed_mismatch = true;
            std::printf("KnackDlss: the scene images do not fit: colour %ux%u format %d usage %x, "
                        "depth %ux%u, velocity %ux%u format %d usage %x\n",
                        w, h, static_cast<int>(color.info.pixel_format),
                        static_cast<unsigned>(static_cast<VkImageUsageFlags>(color.usage_flags)),
                        depth.info.size.width, depth.info.size.height, velocity.info.size.width,
                        velocity.info.size.height, static_cast<int>(velocity.info.pixel_format),
                        static_cast<unsigned>(static_cast<VkImageUsageFlags>(velocity.usage_flags)));
        }
        return;
    }
    if (!EnsureResources(w, h)) {
        failed = true;
        SetState(3);
        return;
    }
    const auto depth_view = CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
    const auto color_view =
        CachedView(color, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor);
    const auto velocity_view =
        CachedView(velocity, vk::Format::eR16G16Sfloat, vk::ImageAspectFlagBits::eColor);

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&velocity, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // Neither image keeps anything from the last frame: the layouts start undefined every time.
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    own_barrier(vk::Image(output_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    // 1. The game's velocities (NDC, previous - current) become DLSS motion vectors (pixels).
    {
        const float scale[2] = {float(w) * 0.5f, (flip_motion_y ? 1.0f : -1.0f) * float(h) * 0.5f};
        const vk::DescriptorImageInfo velocity_info{.imageView = velocity_view,
                                                    .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &velocity_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &depth_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(scale), scale);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    // 2. DLSS.
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_run).count();
    // A long gap (first run, a menu, DLSS switched off for a while): the history is stale.
    const bool gap = last_run == std::chrono::steady_clock::time_point{} || frame_ms > 250.0f;
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_run = now;

    Dlss* dlss = Dlss::Get();
    const int preset = Gow3Settings::Get().knack_dlss_preset.load();
    const Dlss::FeatureDesc desc{w, h, w, h, Dlss::QualityForScale(1.0f), true, preset};
    bool reset = reset_history || !ran_last_frame || gap;
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        // The previous feature may still be in use by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset = true;
    }
    if (ok) {
        const float sign = flip_jitter ? -1.0f : 1.0f;
        ok = dlss->Evaluate(
            cmdbuf,
            {
                .color = {color.GetImage(), color_view, color.info.pixel_format,
                          vk::ImageAspectFlagBits::eColor, w, h},
                .depth = {depth.GetImage(), depth_view, depth.info.pixel_format,
                          vk::ImageAspectFlagBits::eDepth, w, h},
                .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                           vk::ImageAspectFlagBits::eColor, w, h},
                .output = {vk::Image(output_image), *output_view,
                           vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor, w, h},
                .jitter_x = sign * jitter[0],
                .jitter_y = sign * jitter[1],
                .reset = reset,
                .frame_ms = frame_ms,
                .sharpness = 0.0f, // the menu's own sharpening (RCAS) stays in charge
            });
    }
    if (!ok) {
        std::printf("KnackDlss: DLSS failed; it stays off until the game is restarted\n");
        failed = true;
        reset_history = true;
        SetState(3);
        return;
    }

    // 3. The result replaces the scene colour's RGB; its alpha stays (the post reads data from it).
    own_barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw,
                vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();
    {
        const u32 mode = debug_motion ? 1u : 0u;
        const vk::DescriptorImageInfo out_info{.imageView = *output_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &out_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &motion_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *merge_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*merge_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(mode), &mode);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    reset_history = false;
    ran_this_frame = true;
    frames_without_run = 0;
    if (frames_run++ == 0) {
        std::printf("KnackDlss: running, %ux%u, preset %d%s\n", w, h, preset,
                    debug_motion ? " (motion vector debug view)" : "");
    } else if (frames_run % 3600 == 0) {
        std::printf("KnackDlss: %u frames upscaled\n", frames_run);
    }
}

} // namespace Vulkan
