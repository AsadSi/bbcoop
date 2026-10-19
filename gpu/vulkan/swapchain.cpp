// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/swapchain.h"

#include <algorithm>
#include <array>
#include <chrono> // bbcoop
#include <cstdio> // bbcoop
#include <ctime>  // bbcoop: clock_gettime_nsec_np
#include <cstdlib> // bbcoop: BB_SWAPCHAIN_IMAGES
#include <limits>

#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "vulkan/instance.h"
#include "vulkan/platform.h"

namespace Vulkan {

namespace {

/// Rec.2020 with the PQ curve: what HDR games output. Other HDR spaces would need conversions.
constexpr vk::SurfaceFormatKHR HdrFormat{
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

vk::PresentModeKHR RequestedPresentMode() {
    const auto name = EmulatorSettings.GetPresentMode();
    if (name == "Fifo") {
        return vk::PresentModeKHR::eFifo;
    }
    if (name == "Immediate") {
        return vk::PresentModeKHR::eImmediate;
    }
    if (name != "Mailbox") {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, using Mailbox", name);
    }
    return vk::PresentModeKHR::eMailbox;
}

} // namespace

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window)
    : instance{instance_}, surface{CreateSurface(instance.GetInstance(), window)} {
    ChooseFormat();
    ChoosePresentMode();
    Create(u32(window.GetWidth()), u32(window.GetHeight()));
}

Swapchain::~Swapchain() {
    DestroyResources();
    if (swapchain) {
        instance.GetDevice().destroySwapchainKHR(swapchain);
    }
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::ChooseFormat() {
    const auto formats =
        Check<"query surface formats">(instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface));
    supports_hdr = EmulatorSettings.IsHdrAllowed() && std::ranges::contains(formats, HdrFormat);
    // A single undefined format: the surface takes any.
    if (formats.size() == 1 && formats[0].format == vk::Format::eUndefined) {
        sdr_format = {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
        return;
    }
    const auto it = std::ranges::find_if(formats, [](const vk::SurfaceFormatKHR& f) {
        return f.format == vk::Format::eR8G8B8A8Unorm || f.format == vk::Format::eB8G8R8A8Unorm;
    });
    ASSERT_MSG(it != formats.end(), "the surface has no RGBA8 or BGRA8 format");
    sdr_format = *it;
}

void Swapchain::ChoosePresentMode() {
    const auto [result, modes] = instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    vsync = BbSettings::Get().vsync; // bbcoop
    present_mode = RequestedPresentMode();
    if (result != vk::Result::eSuccess || !std::ranges::contains(modes, present_mode)) {
        // FIFO is always there.
        LOG_WARNING(Render_Vulkan, "Present mode {} is not supported, using Fifo",
                    vk::to_string(present_mode));
        present_mode = vk::PresentModeKHR::eFifo;
    }
}

void Swapchain::QuerySurface() {
    const auto caps = Check<"query surface capabilities">(
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface));
    extent = caps.currentExtent;
    if (extent.width == std::numeric_limits<u32>::max()) {
        // The surface takes the swapchain's size.
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    // One image more than the minimum: one to draw into while the others are shown.
    image_count = caps.minImageCount + 1;
    // bbcoop: with FIFO (V-Sync) every extra image is another refresh of input lag; keep the
    // fewest the surface allows. Mailbox keeps the extra image (it replaces queued frames).
    if (present_mode == vk::PresentModeKHR::eFifo) {
        image_count = std::max(caps.minImageCount, 2u);
    }
    // bbcoop: BB_SWAPCHAIN_IMAGES=<n> by hand: 3 may absorb a frame just past its vblank (otherwise
    // shown for three refreshes and the next for one: 50/17 ms judder at 30 FPS on a 60 Hz Mac).
    if (const char* env = std::getenv("BB_SWAPCHAIN_IMAGES"); env && std::atoi(env) >= 2) {
        image_count = std::max(caps.minImageCount, u32(std::atoi(env)));
    }
    if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    std::printf("GPU: swapchain %s, %u images (surface minimum %u)\n",
                vk::to_string(present_mode).c_str(), image_count, caps.minImageCount);
    transform = caps.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
                    ? vk::SurfaceTransformFlagBitsKHR::eIdentity
                    : caps.currentTransform;
    composite_alpha = caps.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
                          ? vk::CompositeAlphaFlagBitsKHR::eOpaque
                          : vk::CompositeAlphaFlagBitsKHR::eInherit;
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    // A minimised window has a 0x0 surface (issue #31) and no swapchain can have that extent:
    // the old one stays (presents are skipped) until the window is back.
    QuerySurface();
    zero_extent = extent.width == 0 || extent.height == 0;
    if (zero_extent) {
        needs_recreation = true;
        return;
    }

    // The old swapchain is retired through oldSwapchain, not destroyed first: NVIDIA's Wayland
    // WSI keeps a tearing controller per swapchain and rejected a fresh one on the same surface
    // ("Surface already has a tearing controller", SurfaceLost).
    DestroyResources();
    const vk::Device device = instance.GetDevice();
    const std::array families = {instance.GetGraphicsQueueFamilyIndex(),
                                 instance.GetPresentQueueFamilyIndex()};
    const bool exclusive = families[0] == families[1];
    const auto format = use_hdr ? HdrFormat : sdr_format;
    const vk::SwapchainKHR old = swapchain;
    swapchain = Check<"create swapchain">(device.createSwapchainKHR({
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent,
        .queueFamilyIndexCount = exclusive ? 1u : 2u,
        .pQueueFamilyIndices = families.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = old,
    }));
    if (old) {
        device.destroySwapchainKHR(old);
    }

    images = Check<"get swapchain images">(device.getSwapchainImagesKHR(swapchain));
    image_count = u32(images.size());
    views.resize(image_count);
    acquired.resize(image_count);
    present_ready.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        views[i] = Check<"create swapchain view">(device.createImageView({
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = format.format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        acquired[i] = Check<"create swapchain semaphore">(device.createSemaphore({}));
        present_ready[i] = Check<"create swapchain semaphore">(device.createSemaphore({}));
        SetObjectName(device, images[i], "swapchain image {}", i);
        SetObjectName(device, views[i], "swapchain view {}", i);
        SetObjectName(device, acquired[i], "swapchain acquired {}", i);
        SetObjectName(device, present_ready[i], "swapchain present ready {}", i);
    }
    frame_index = 0;
}

void Swapchain::DestroyResources() {
    const vk::Device device = instance.GetDevice();
    // The semaphores and views may still be in use by the last presents.
    if (const auto result = device.waitIdle(); result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Device idle wait before swapchain changes: {}",
                    vk::to_string(result));
    }
    for (const auto view : views) {
        device.destroyImageView(view);
    }
    for (const auto semaphore : acquired) {
        device.destroySemaphore(semaphore);
    }
    for (const auto semaphore : present_ready) {
        device.destroySemaphore(semaphore);
    }
    views.clear();
    acquired.clear();
    present_ready.clear();
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreating the swapchain: {}x{}, HDR {}", width_, height_, use_hdr);
    ChoosePresentMode(); // bbcoop: the V-Sync setting may have changed
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (use_hdr == hdr) {
        return;
    }
    use_hdr = hdr;
    Recreate(width, height);
}

bool Swapchain::AcquireNextImage() {
    const vk::Result result = instance.GetDevice().acquireNextImageKHR(
        swapchain, std::numeric_limits<u64>::max(), acquired[frame_index], VK_NULL_HANDLE,
        &image_index);
    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        Check<"acquire a swapchain image">(result);
    }
    return !needs_recreation;
}

bool Swapchain::Present() {
    vk::PresentInfoKHR info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };
    // bbcoop: at a fixed frame rate below the refresh rate (30 FPS on 60 Hz) each frame is asked
    // to appear a whole number of refreshes after the last one shown (Metal's presentAtTime), so a
    // frame that is ready a little early or late still gets its slot: every frame stays two
    // refreshes instead of 3 then 1 (the 50/17 ms judder). BB_PRESENT_PACE=0: off.
    static const bool pace = [] {
        const char* env = std::getenv("BB_PRESENT_PACE");
        return !(env && env[0] == '0');
    }();
    vk::PresentTimeGOOGLE time{.presentID = display_stats.next_id++};
    const vk::PresentTimesInfoGOOGLE times{.swapchainCount = 1, .pTimes = &time};
    if (instance.IsDisplayTimingSupported()) {
        const u64 period = display_clock.period_ns.load(std::memory_order_relaxed);
        const u32 refreshes = display_clock.refreshes_per_flip.load(std::memory_order_relaxed);
        if (pace && refreshes && period && display_stats.last_raw) {
            // Half a refresh early: the frame is shown at the first refresh after that time.
            time.desiredPresentTime = display_stats.last_raw +
                                      u64(time.presentID - display_stats.last_id) * refreshes * period -
                                      period / 2;
        }
        info.pNext = &times;
    }
    const vk::Result result = instance.GetPresentQueue().presentKHR(info);
    if (instance.IsDisplayTimingSupported()) {
        ReadDisplayTimes();
    }
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        Check<"present">(result);
    }
    frame_index = (frame_index + 1) % image_count;
    return !needs_recreation;
}

void Swapchain::ReadDisplayTimes() {
    const vk::Device device = instance.GetDevice();
    if (!display_clock.period_ns.load(std::memory_order_relaxed)) {
        const auto [result, cycle] = device.getRefreshCycleDurationGOOGLE(swapchain);
        if (result == vk::Result::eSuccess) {
            display_clock.period_ns.store(cycle.refreshDuration, std::memory_order_relaxed);
        }
    }
    const auto [result, timings] = device.getPastPresentationTimingGOOGLE(swapchain);
    if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
        return;
    }
    const u64 now = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
#ifdef __APPLE__
    // Metal's presentedTime is the uptime clock, which stops while the Mac sleeps; the steady
    // clock does not (Wooloomooloo2/bloodborne_mac_port, DisplayTimeToSteady).
    const u64 to_steady = now - clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    const u64 to_steady = 0;
#endif
    const u64 period = display_clock.period_ns.load(std::memory_order_relaxed);
    auto& stats = display_stats;
    for (const vk::PastPresentationTimingGOOGLE& timing : timings) {
        if (!timing.actualPresentTime) {
            ++stats.not_shown;
            continue;
        }
        const u64 shown = timing.actualPresentTime + to_steady;
        if (stats.last_ns && period && shown > stats.last_ns) {
            const u64 refreshes = (shown - stats.last_ns + period / 2) / period;
            ++stats.shown[std::clamp<u64>(refreshes, 1, 4) - 1];
        }
        stats.last_ns = shown;
        stats.last_raw = timing.actualPresentTime;
        stats.last_id = timing.presentID;
        display_clock.shown_ns.store(shown, std::memory_order_relaxed);
    }
    static const bool report = std::getenv("BB_FRAME_STATS") != nullptr;
    if (report && now >= stats.report_ns) {
        if (stats.report_ns) {
            std::printf("Display: frames on screen for 1/2/3/4+ refreshes: %llu/%llu/%llu/%llu, "
                        "not shown %llu\n",
                        (unsigned long long)stats.shown[0], (unsigned long long)stats.shown[1],
                        (unsigned long long)stats.shown[2], (unsigned long long)stats.shown[3],
                        (unsigned long long)stats.not_shown);
        }
        std::fill(std::begin(stats.shown), std::end(stats.shown), 0);
        stats.not_shown = 0;
        stats.report_ns = now + 5'000'000'000ULL;
    }
}

} // namespace Vulkan
