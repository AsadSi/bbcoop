// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: the window's swapchain: its surface, format (SDR or HDR10 PQ), present mode,
// images with views, and the semaphores of acquire and present. Recreated on resizes; a
// minimised (0x0) window keeps the old one until it is back.
#pragma once

#include <atomic>
#include <vector>

#include "common/types.h"
#include "vulkan/vk_common.h"
#include "bbport_settings.h" // bbcoop: vsync

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;

/// bbcoop: the display's refresh as seen through VK_GOOGLE_display_timing (steady-clock ns; 0 until
/// a frame has been shown), for VideoOut's vblank to keep its phase (BbVideoOut::Display).
struct DisplayClock {
    std::atomic<u64> shown_ns{0};  ///< a refresh at which a frame appeared
    std::atomic<u64> period_ns{0}; ///< the refresh period
    /// Refreshes each flip should stay on screen (2 at 30 FPS on 60 Hz); 0: shown when ready.
    std::atomic<u32> refreshes_per_flip{0};
};
inline DisplayClock display_clock;

class Swapchain {
public:
    Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// A swapchain for the window's size (`width` x `height` when the surface leaves it open).
    void Recreate(u32 width, u32 height);
    /// Acquires the next image; false when the swapchain has to be recreated first.
    bool AcquireNextImage();
    /// Presents the acquired image; false when the swapchain has to be recreated.
    bool Present();

    /// Switches the images to HDR10 (PQ) or back, when the surface supports it.
    void SetHDR(bool hdr);
    bool HasHDR() const {
        return supports_hdr;
    }
    bool GetHDR() const {
        return use_hdr;
    }

    /// bbcoop: the V-Sync setting changed since the present mode was chosen.
    bool VsyncChanged() const {
        return vsync != BbSettings::Get().vsync.load();
    }

    bool IsPresentable() const {
        return swapchain && !zero_extent;
    }
    vk::Image Image() const {
        return images[image_index];
    }
    vk::ImageView ImageView() const {
        return views[image_index];
    }
    /// The SDR surface format (frames and passes are made in it; HDR images take A2B10G10R10).
    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return sdr_format;
    }
    vk::Extent2D GetExtent() const {
        return extent;
    }
    u32 GetWidth() const {
        return width;
    }
    u32 GetHeight() const {
        return height;
    }
    u32 GetImageCount() const {
        return image_count;
    }
    vk::Semaphore GetImageAcquiredSemaphore() const {
        return acquired[frame_index];
    }
    vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

private:
    void ChooseFormat();
    void ChoosePresentMode();
    /// Extent, image count, transform and alpha mode from the surface's capabilities.
    void QuerySurface();
    void Create(u32 width, u32 height);
    /// The views and semaphores (and the swapchain unless it is being retired).
    void DestroyResources();
    /// bbcoop: reads back when earlier frames reached the screen (DisplayClock) and, with
    /// BB_FRAME_STATS, prints every 5 s for how many refreshes each frame stayed.
    void ReadDisplayTimes();

    struct DisplayStats {
        u64 last_ns = 0;  ///< when the latest shown frame appeared (steady clock)
        u64 last_raw = 0; ///< the same in the display's clock (desiredPresentTime's)
        u32 last_id = 0;  ///< its present id
        u32 next_id = 1;  ///< the id of the next present
        u64 shown[4]{};   ///< frames that stayed 1, 2, 3, 4 or more refreshes
        u64 not_shown = 0; ///< replaced before the display took them
        u64 report_ns = 0; ///< the next report
    } display_stats; // bbcoop

    const Instance& instance;
    vk::SurfaceKHR surface;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceFormatKHR sdr_format{};
    vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
    bool vsync = false; // bbcoop
    vk::Extent2D extent{};
    vk::SurfaceTransformFlagBitsKHR transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    vk::CompositeAlphaFlagBitsKHR composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> views;
    std::vector<vk::Semaphore> acquired;      ///< per frame in flight
    std::vector<vk::Semaphore> present_ready; ///< per image
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool zero_extent = false; ///< the surface was 0x0 (minimised) at the last Create
    bool use_hdr = false;
    bool supports_hdr = false;
};

} // namespace Vulkan
