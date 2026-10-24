// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The Vulkan backend refers to the frame interpolation swapchain, which is
// not built (only the FSR 3 upscaler is used)

#include <FidelityFX/host/backends/vk/ffx_vk.h>

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK (FfxFrameGenerationConfig const *)
{
	return FFX_ERROR_INVALID_ARGUMENT;
}
