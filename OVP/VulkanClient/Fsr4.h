// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// AMD FSR 4 (FSR Upscaling 4.1.2), an opt-in upscaler beside FSR 3.1:
// FsrUpscaler's other backend.
//
// AMD ships FSR 4 for Vulkan as signed Windows DLLs only (FidelityFX SDK
// 2.4). They run in this process through ffx-vk-linux, a PE loader
// (ffx/Fsr4.cmake fetches both into Modules/Plugin/FSR4). The library is
// opened only when FSR 4 is chosen (the graphics settings' anti-aliasing,
// ORBITER_VK_UPSCALER=fsr4); whatever fails - the files missing, a GPU
// AMD's DLL does not give FSR 4 to, a call's error - FsrUpscaler goes on
// with FSR 3.1.
// =======================================================================

#ifndef __FSR4_H
#define __FSR4_H

#include "Fsr.h"

class Fsr4Backend {
public:
	Fsr4Backend ();
	~Fsr4Backend ();
	/// whether the client was built with FSR 4 and its files are in place
	static bool Available ();

	/// opens the library and AMD's DLLs; false: why is logged
	bool Init (vkc::Context *ctx);
	void Shutdown ();
	/// as FsrUpscaler::Dispatch
	VkImageView Dispatch (const FsrUpscaler::Inputs &in, float sharpness, bool debugView);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

/// The FidelityFX backends' device function lookup (Fsr.cpp): entry points
/// of extensions that are core in Vulkan 1.3 or not enabled resolved
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FfxGetDeviceProc (VkDevice device, const char *name);

#endif // !__FSR4_H
