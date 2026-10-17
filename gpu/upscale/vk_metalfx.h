// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): Apple's MetalFX temporal scaler as an upscaler. It runs inside MoltenVK, at its
// place in the command stream, through vkCmdUpscaleMetalFXBB: a command of the MoltenVK build the
// Mac port ships (moltenvk-bb, Commands/MVKCmdBBMetalFX.h), not a Vulkan extension. This struct is
// the same as that header's; without the command (another MoltenVK) MetalFX is unavailable.

#pragma once

#include "vulkan/vk_common.h"

extern "C" {

struct VkBBMetalFXUpscaleInfo {
    VkImage color;          ///< Render-size color
    VkImage depth;          ///< Render-size depth
    VkImage motion;         ///< Render-size motion vectors (RG16F), current to previous
    VkImage output;         ///< Output-size color
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t outputWidth;
    uint32_t outputHeight;
    float jitterX;          ///< This frame's sub-pixel jitter, in input pixels
    float jitterY;
    float motionScaleX;     ///< Takes the motion texture's values to input pixels
    float motionScaleY;
    VkBool32 reset;         ///< The history is invalid
    VkBool32 depthReversed; ///< Far is 0
    VkBool32 hdr;           ///< Linear HDR color (auto exposure); else display-ready color
};

using PFN_vkCmdUpscaleMetalFXBB = void(VKAPI_PTR*)(VkCommandBuffer, const VkBBMetalFXUpscaleInfo*);
}
