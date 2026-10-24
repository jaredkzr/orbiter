// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// AMD FidelityFX Super Resolution 3 upscaler (FidelityFX SDK, Vulkan
// backend): temporal anti-aliasing and upscaling of the scene from the
// render size to the display size
// =======================================================================

#ifndef __FSR_H
#define __FSR_H

#include "VkContext.h"
#include <memory>

class FsrUpscaler {
public:
	FsrUpscaler ();
	~FsrUpscaler ();
	/// whether the client was built with FSR
	static bool Available ();

	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// Projection jitter [pixels] of frame n, for upscaling from renderW to displayW
	static void Jitter (uint32_t n, uint32_t renderW, uint32_t displayW, float &jx, float &jy);

	struct Inputs {
		VkImage color;         // HDR scene, render size, shader-read layout
		VkImage depth;         // scene depth (inverted, infinite), render size, depth-attachment layout
		VkImage velocity;      // screen motion [uv] (now - then), render size, shader-read layout
		VkFormat colorFormat, velocityFormat;
		float jitter[2];       // [pixels]
		float frameTime;       // [s]
		float nearPlane;       // [m]
		float fovY;            // vertical field of view [rad]
		bool reset;
	};
	/// Upscale (outside rendering scopes); returns the output image (display
	/// size, shader-read layout), or VK_NULL_HANDLE on failure
	VkImageView Dispatch (const Inputs &in);

	float sharpness = 0.5f;    // RCAS sharpening 0..1 (ORBITER_VK_FSR_SHARPNESS)
	bool debugView = false;    // FSR's debug panels (ORBITER_VK_FSR_DEBUG)

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

#endif // !__FSR_H
