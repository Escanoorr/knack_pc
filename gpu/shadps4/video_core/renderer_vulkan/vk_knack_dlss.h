// SPDX-License-Identifier: GPL-2.0-or-later
//
// knack: NVIDIA DLSS for Knack at the game's own render size (docs/knack-dlss.md), a module of
// its own next to the God of War III upscaler: Knack is a deferred renderer that already writes
// everything DLSS wants, so none of that upscaler's machinery (camera constants, UI composition,
// scene proxies) is used.
//
//  * The G-buffer pass (six colour targets, the sixth R16G16F) holds the scene depth and the
//    game's own velocity buffer: previous - current position in NDC, animation included.
//  * The HDR scene colour is the RGBA16F target drawn with that depth.
//  * Jitter is a sub-pixel viewport shift on every geometry draw that uses the scene depth: the
//    Z pre-pass, the G-buffer and the forward passes, so depth tests that compare against the
//    pre-pass keep matching and the game's clip-space velocities stay jitter free.
//  * Just before the game's first post-processing compute dispatch of the frame (a shader hash,
//    GOW3_KNACK_DLSS_BEFORE_CS) DLSS reads colour, depth and the converted motion vectors and its
//    result replaces the scene colour, so bloom, tone mapping, the game's SMAA and the UI follow
//    as usual.
//
// Everything is switched on from the menu (Image tab) or GOW3_KNACK_DLSS=1; with the setting off,
// or without gow3_dlss.dll and nvngx_dlss.dll, nothing here runs.

#pragma once

#include <array>
#include <chrono>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Runtime;
class Scheduler;

class KnackDlss {
public:
    KnackDlss(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
              Runtime& runtime);
    ~KnackDlss();

    /// Switched on and DLSS is usable. Cheap: it gates the per-draw hooks.
    [[nodiscard]] bool Enabled() const;

    /// Every draw: finds the G-buffer pass (and with it the scene depth and the velocity target)
    /// and the HDR scene colour. `color5` is only read when bit 5 of `mrt_mask` is set.
    void OnTargets(u32 mrt_mask, VideoCore::ImageId color0, VideoCore::ImageId color5,
                   VideoCore::ImageId depth);

    /// Sub-pixel viewport shift of a geometry draw in render pixels (x right, y down), or zero.
    [[nodiscard]] std::array<float, 2> DrawJitter(VideoCore::ImageId depth, u32 num_indices,
                                                  u32 num_instances) const;

    /// Before every compute dispatch: DLSS runs ahead of the game's post-processing.
    void OnDispatch(u64 cs_hash);

    /// The pass that copies a finished frame to a display buffer: the next frame starts.
    void OnDisplayPass();

private:
    struct ViewEntry {
        vk::Image image{};
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view{};
        u64 last_use = 0;
    };

    bool IsSceneDepth(VideoCore::ImageId depth) const;
    void NoteGBuffer(VideoCore::ImageId depth, VideoCore::ImageId velocity);
    bool EnsureResources(u32 width, u32 height);
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);
    void Run();
    void SetState(int state);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    // Options read once (environment): see docs/knack-dlss.md.
    enum class Test { None, NoJitter, NoMotion, NoDlss, BigJitter };
    u64 trigger_hash = 0x526df215;
    bool flip_mv_x = false, flip_mv_y = false;          ///< motion vector signs, for finding a mistake
    bool flip_jitter_x = false, flip_jitter_y = false;  ///< the signs of the jitter handed to DLSS
    u32 debug_mode = 0;  ///< 0 off, 1..4 see knack_dlss.comp (motion, split screen, difference)
    bool hdr_input = true;  ///< DLSS told the scene colour is linear HDR
    bool ldr_mode = false;  ///< run on the tone-mapped 8-bit image before the game's SMAA, not on the HDR
    Test test = Test::None;
    bool failed = false;
    bool printed_mismatch = false;

    // The scene depth buffer, kept across frames by address and size (image ids can be reused).
    VAddr depth_address = 0;
    u32 depth_width = 0, depth_height = 0;
    // This frame, from the draws seen so far.
    VideoCore::ImageId depth_id{}, velocity_id{}, color_id{}, ldr_id{};
    bool gbuffer_seen = false;
    bool frame_valid = false;  ///< the scene depth was known from the start of the frame
    bool done = false;         ///< DLSS ran (or was given up on) this frame: no more jitter
    bool ran_this_frame = false;
    bool ran_last_frame = false;
    bool reset_history = true;
    u32 jitter_index = 0;
    std::array<float, 2> jitter{};
    u32 frames_run = 0;
    u32 frames_without_run = 0;
    // Counters for the log line every StatsFrames frames (what really happened, for finding mistakes).
    struct Stats {
        u32 frames = 0, boundaries_ignored = 0, runs = 0, resets = 0, bail_outs = 0;
        u64 jittered_draws = 0;
    };
    mutable Stats stats;
    std::chrono::steady_clock::time_point last_run{};

    // Own resources: motion vectors in pixels, DLSS output, two compute pipelines.
    u32 width = 0, height = 0;
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    vk::UniqueImageView output_view;
    vk::UniqueDescriptorSetLayout motion_desc_layout, merge_desc_layout;
    vk::UniquePipelineLayout motion_pipeline_layout, merge_pipeline_layout;
    vk::UniquePipeline motion_pipeline, merge_pipeline, merge_ldr_pipeline;
    std::array<ViewEntry, 8> view_cache{};
    u64 view_uses = 0;
};

} // namespace Vulkan
