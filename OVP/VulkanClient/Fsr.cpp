// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Fsr.h"
#include "OrbiterAPI.h"
#include <cfloat>
#include <string>
#include <cstdlib>
#include <vector>

#ifdef VKC_HAVE_FSR

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

namespace {
	const VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

	void Message (FfxMsgType type, const wchar_t *msg)
	{
		std::string s;
		for (const wchar_t *p = msg; p && *p; p++) s += (char)(*p < 128 ? *p : '?');
		oapiWriteLogV ("VulkanClient: FSR %s: %s", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", s.c_str());
	}

	// Device function lookup for the backend: entry points it asks for under
	// extension names that are core in Vulkan 1.3 resolve to the core
	// functions; debug labels, object names and buffer markers (extensions
	// that may not be enabled) become no-ops
	VKAPI_ATTR VkResult VKAPI_CALL NoSetObjectName (VkDevice, const VkDebugUtilsObjectNameInfoEXT *) { return VK_SUCCESS; }
	VKAPI_ATTR void VKAPI_CALL NoBeginLabel (VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
	VKAPI_ATTR void VKAPI_CALL NoEndLabel (VkCommandBuffer) {}
	VKAPI_ATTR void VKAPI_CALL NoMarker (VkCommandBuffer, VkPipelineStageFlagBits, VkBuffer, VkDeviceSize, uint32_t) {}
	VKAPI_ATTR void VKAPI_CALL NoMarker2 (VkCommandBuffer, VkPipelineStageFlags2, VkBuffer, VkDeviceSize, uint32_t) {}

	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProc (VkDevice device, const char *name)
	{
		if (PFN_vkVoidFunction f = vkGetDeviceProcAddr (device, name)) return f;
		std::string n (name);
		for (const char *suffix : { "KHR", "EXT" })
			if (n.size() > 3 && n.compare (n.size() - 3, 3, suffix) == 0)
				if (PFN_vkVoidFunction f = vkGetDeviceProcAddr (device, n.substr (0, n.size() - 3).c_str())) return f;
		if (n == "vkSetDebugUtilsObjectNameEXT") return (PFN_vkVoidFunction)NoSetObjectName;
		if (n == "vkCmdBeginDebugUtilsLabelEXT") return (PFN_vkVoidFunction)NoBeginLabel;
		if (n == "vkCmdEndDebugUtilsLabelEXT") return (PFN_vkVoidFunction)NoEndLabel;
		if (n == "vkCmdWriteBufferMarkerAMD") return (PFN_vkVoidFunction)NoMarker;
		if (n == "vkCmdWriteBufferMarker2AMD") return (PFN_vkVoidFunction)NoMarker2;
		return nullptr;
	}

	FfxResourceDescription Desc (VkFormat fmt, VkExtent2D e, uint32_t usage)
	{
		FfxResourceDescription d{};
		d.type = FFX_RESOURCE_TYPE_TEXTURE2D;
		d.format = ffxGetSurfaceFormatVK (fmt);
		d.width = e.width, d.height = e.height, d.depth = 1;
		d.mipCount = 1;
		d.flags = FFX_RESOURCE_FLAGS_NONE;
		d.usage = (FfxResourceUsage)usage;
		return d;
	}
}

struct FsrUpscaler::Impl {
	vkc::Context *ctx = nullptr;
	std::vector<uint8_t> scratch;
	FfxInterface backend{};
	FfxFsr3UpscalerContext context{};
	bool haveContext = false;
	uint32_t generation = ~0u;
	VkExtent2D renderSize{}, displaySize{};
	vkc::Image output;
	// resources the upscaler shares with later effects, owned by the caller
	struct Shared { vkc::Image img; VkFormat fmt; FfxResourceDescription desc; } shared[3];

	vkc::Image CreateImage (VkFormat fmt, VkExtent2D e, VkImageUsageFlags usage, VkImageLayout layout)
	{
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = fmt;
		ici.extent = { e.width, e.height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		VkImage img = im.image;
		ctx->ImmediateSubmit ([img, layout](VkCommandBuffer cmd) {
			vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, layout,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
		});
		return im;
	}

	bool CreateContext ()
	{
		DestroyContext ();
		renderSize = ctx->RenderExtent (), displaySize = ctx->Extent ();
		size_t size = ffxGetScratchMemorySizeVK (ctx->physicalDevice, FFX_FSR3UPSCALER_CONTEXT_COUNT);
		scratch.assign (size, 0);
		VkDeviceContext dc{ ctx->device, ctx->physicalDevice, GetDeviceProc };
		if (ffxGetInterfaceVK (&backend, ffxGetDeviceVK (&dc), scratch.data(), size, FFX_FSR3UPSCALER_CONTEXT_COUNT) != FFX_OK)
			return false;
		FfxFsr3UpscalerContextDescription cd{};
		cd.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED |
			FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
		cd.maxRenderSize = { renderSize.width, renderSize.height };
		cd.maxUpscaleSize = { displaySize.width, displaySize.height };
		cd.fpMessage = Message;
		cd.backendInterface = backend;
		if (ffxFsr3UpscalerContextCreate (&context, &cd) != FFX_OK) return false;
		haveContext = true;

		output = CreateImage (OUTPUT_FORMAT, displaySize, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// shared resources (dilated depth and motion, reconstructed depth of
		// the previous frame), kept in general layout
		FfxFsr3UpscalerSharedResourceDescriptions sd{};
		if (ffxFsr3UpscalerGetSharedResourceDescriptions (&context, &sd) != FFX_OK) return false;
		const FfxCreateResourceDescription *cds[3] = { &sd.dilatedDepth, &sd.dilatedMotionVectors, &sd.reconstructedPrevNearestDepth };
		for (int i = 0; i < 3; i++) {
			const FfxResourceDescription &rd = cds[i]->resourceDescription;
			VkFormat fmt = rd.format == FFX_SURFACE_FORMAT_R32_FLOAT ? VK_FORMAT_R32_SFLOAT
				: rd.format == FFX_SURFACE_FORMAT_R16G16_FLOAT ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R32_UINT;
			shared[i].fmt = fmt;
			shared[i].desc = rd;
			shared[i].img = CreateImage (fmt, { rd.width, rd.height },
				VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_LAYOUT_GENERAL);
		}
		generation = ctx->SwapchainGeneration ();
		oapiWriteLogV ("VulkanClient: FSR 3 upscaler %ux%u -> %ux%u", renderSize.width, renderSize.height,
			displaySize.width, displaySize.height);
		return true;
	}

	void DestroyContext ()
	{
		if (!haveContext) return;
		vkDeviceWaitIdle (ctx->device);
		ffxFsr3UpscalerContextDestroy (&context);
		haveContext = false;
		ctx->DestroyImage (output);
		for (auto &sh : shared) ctx->DestroyImage (sh.img);
		generation = ~0u;
	}
};

FsrUpscaler::FsrUpscaler () : impl (new Impl) {}
FsrUpscaler::~FsrUpscaler () {}

bool FsrUpscaler::Available () { return true; }

bool FsrUpscaler::Init (vkc::Context *ctx)
{
	impl->ctx = ctx;
	if (const char *e = getenv ("ORBITER_VK_FSR_SHARPNESS")) sharpness = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_FSR_DEBUG")) debugView = atoi (e) != 0;
	return true;
}

void FsrUpscaler::Shutdown ()
{
	if (!impl->ctx) return;
	impl->DestroyContext ();
	impl->ctx = nullptr;
}

void FsrUpscaler::Jitter (uint32_t n, uint32_t renderW, uint32_t displayW, float &jx, float &jy)
{
	int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount ((int32_t)renderW, (int32_t)displayW);
	ffxFsr3UpscalerGetJitterOffset (&jx, &jy, (int32_t)(n % (uint32_t)phases), phases);
}

VkImageView FsrUpscaler::Dispatch (const Inputs &in)
{
	vkc::Context *ctx = impl->ctx;
	if (impl->generation != ctx->SwapchainGeneration () && !impl->CreateContext ()) {
		oapiWriteLogV ("VulkanClient: FSR 3 context creation failed");
		return VK_NULL_HANDLE;
	}
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd ();
	VkExtent2D rs = impl->renderSize;
	// the depth buffer is read by the upscaler (it is cleared at the next use)
	vkc::ImageBarrier (cmd, in.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
		VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

	FfxFsr3UpscalerDispatchDescription d{};
	d.commandList = ffxGetCommandListVK (cmd);
	d.color = ffxGetResourceVK ((void*)in.color, Desc (in.colorFormat, rs, FFX_RESOURCE_USAGE_READ_ONLY), L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.depth = ffxGetResourceVK ((void*)in.depth, Desc (ctx->depthFormat, rs, FFX_RESOURCE_USAGE_DEPTHTARGET), L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.motionVectors = ffxGetResourceVK ((void*)in.velocity, Desc (in.velocityFormat, rs, FFX_RESOURCE_USAGE_READ_ONLY), L"motion", FFX_RESOURCE_STATE_COMPUTE_READ);
	if (in.reactive)
		d.reactive = ffxGetResourceVK ((void*)in.reactive, Desc (VK_FORMAT_R8_UNORM, rs, FFX_RESOURCE_USAGE_READ_ONLY), L"reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.output = ffxGetResourceVK ((void*)impl->output.image, Desc (OUTPUT_FORMAT, impl->displaySize, FFX_RESOURCE_USAGE_UAV), L"output", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.dilatedDepth = ffxGetResourceVK ((void*)impl->shared[0].img.image, impl->shared[0].desc, L"dilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	d.dilatedMotionVectors = ffxGetResourceVK ((void*)impl->shared[1].img.image, impl->shared[1].desc, L"dilatedMotion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	d.reconstructedPrevNearestDepth = ffxGetResourceVK ((void*)impl->shared[2].img.image, impl->shared[2].desc, L"prevNearestDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	d.jitterOffset = { in.jitter[0], in.jitter[1] };
	// ours: now - then [uv]; FSR: from this frame to the last [pixels]
	d.motionVectorScale = { -(float)rs.width, -(float)rs.height };
	d.renderSize = { rs.width, rs.height };
	d.upscaleSize = { impl->displaySize.width, impl->displaySize.height };
	d.enableSharpening = sharpness > 0.0f;
	d.sharpness = sharpness;
	d.frameTimeDelta = in.frameTime * 1000.0f;
	d.preExposure = 1.0f;
	d.reset = in.reset;
	d.cameraNear = in.nearPlane;
	d.cameraFar = FLT_MAX;
	d.cameraFovAngleVertical = in.fovY;
	d.viewSpaceToMetersFactor = 1.0f;
	if (debugView) d.flags |= FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW;
	FfxErrorCode err = ffxFsr3UpscalerContextDispatch (&impl->context, &d);
	if (err != FFX_OK) {
		oapiWriteLogV ("VulkanClient: FSR 3 dispatch failed (%d)", (int)err);
		return VK_NULL_HANDLE;
	}
	return impl->output.view;
}

#else // !VKC_HAVE_FSR

struct FsrUpscaler::Impl {};
FsrUpscaler::FsrUpscaler () {}
FsrUpscaler::~FsrUpscaler () {}
bool FsrUpscaler::Available () { return false; }
bool FsrUpscaler::Init (vkc::Context *) { return false; }
void FsrUpscaler::Shutdown () {}
void FsrUpscaler::Jitter (uint32_t, uint32_t, uint32_t, float &jx, float &jy) { jx = jy = 0.0f; }
VkImageView FsrUpscaler::Dispatch (const Inputs &) { return VK_NULL_HANDLE; }

#endif
