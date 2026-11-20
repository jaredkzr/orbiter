// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Fsr4.h"
#include "OrbiterAPI.h"
#include <cfloat>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef VKC_HAVE_FSR4

#include "ffx_vk_linux.h"
#include <dlfcn.h>
#include <filesystem>

namespace {
	const VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	const char *LIB_NAME = "libffx_vk_linux.so";

	// the directory of FSR 4's files: FSR4 beside this module
	std::string Fsr4Dir ()
	{
		Dl_info info{};
		if (!dladdr ((void*)&Fsr4Dir, &info) || !info.dli_fname) return "";
		std::error_code ec;
		std::filesystem::path p = std::filesystem::absolute (info.dli_fname, ec).parent_path () / "FSR4";
		return p.string ();
	}

	// SHA-256 of a file, as hexadecimal ("": unreadable). The loader takes a
	// DLL that was changed (tried: four bytes of the upscaler's overwritten),
	// so AMD's DLLs are checked here against the release's, before they run
	std::string Sha256 (const std::string &path)
	{
		static const uint32_t K[64] = {
			0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
			0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
			0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
			0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
			0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
			0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
		FILE *f = fopen (path.c_str (), "rb");
		if (!f) return "";
		std::vector<uint8_t> data;
		uint8_t buf[65536];
		for (size_t n; (n = fread (buf, 1, sizeof (buf), f)) > 0; ) data.insert (data.end (), buf, buf + n);
		fclose (f);
		uint64_t bits = (uint64_t)data.size () * 8;
		data.push_back (0x80);
		while (data.size () % 64 != 56) data.push_back (0);
		for (int i = 7; i >= 0; i--) data.push_back ((uint8_t)(bits >> (8 * i)));
		uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
		auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
		for (size_t o = 0; o < data.size (); o += 64) {
			uint32_t w[64];
			for (int i = 0; i < 16; i++)
				w[i] = (uint32_t)data[o + 4 * i] << 24 | (uint32_t)data[o + 4 * i + 1] << 16 | (uint32_t)data[o + 4 * i + 2] << 8 | data[o + 4 * i + 3];
			for (int i = 16; i < 64; i++) {
				uint32_t s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], g = h[5], j = h[6], k = h[7];
			for (int i = 0; i < 64; i++) {
				uint32_t t1 = k + (rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25)) + ((e & g) ^ (~e & j)) + K[i] + w[i];
				uint32_t t2 = (rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				k = j, j = g, g = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
			}
			h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += g, h[6] += j, h[7] += k;
		}
		char hex[65];
		for (int i = 0; i < 8; i++) snprintf (hex + 8 * i, 9, "%08x", h[i]);
		return hex;
	}

	// the loader's library and the functions taken from it (AMD's API, as its DLL exports it)
	struct Lib {
		void *handle = nullptr;
		int (*load) (const char*) = nullptr;
		const char *(*error) () = nullptr;
		ffxReturnCode_t (*createContext) (ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*) = nullptr;
		ffxReturnCode_t (*destroyContext) (ffxContext*, const ffxAllocationCallbacks*) = nullptr;
		ffxReturnCode_t (*query) (ffxContext*, ffxQueryDescHeader*) = nullptr;
		ffxReturnCode_t (*dispatch) (ffxContext*, const ffxDispatchDescHeader*) = nullptr;
		bool loaded = false; // (AMD's DLLs: once per process)
	};
	Lib g_lib;

	bool OpenLib (std::string &why)
	{
		if (g_lib.loaded) return true;
		std::string dir = Fsr4Dir ();
		if (Sha256 (dir + "/amd_fidelityfx_loader_vk.dll") != VKC_FSR4_LOADER_DLL_SHA256 ||
			Sha256 (dir + "/amd_fidelityfx_upscaler_vk.dll") != VKC_FSR4_UPSCALER_DLL_SHA256) {
			why = "AMD's DLLs in " + dir + " are not the release's";
			return false;
		}
		if (!g_lib.handle) {
			g_lib.handle = dlopen ((dir + "/" + LIB_NAME).c_str (), RTLD_NOW | RTLD_LOCAL);
			if (!g_lib.handle) { why = dlerror (); return false; }
			auto sym = [](const char *n) { return dlsym (g_lib.handle, n); };
			g_lib.load = (decltype (g_lib.load))sym ("ffxvkLoad");
			g_lib.error = (decltype (g_lib.error))sym ("ffxvkError");
			g_lib.createContext = (decltype (g_lib.createContext))sym ("ffxCreateContext");
			g_lib.destroyContext = (decltype (g_lib.destroyContext))sym ("ffxDestroyContext");
			g_lib.query = (decltype (g_lib.query))sym ("ffxQuery");
			g_lib.dispatch = (decltype (g_lib.dispatch))sym ("ffxDispatch");
		}
		if (!g_lib.load || !g_lib.error || !g_lib.createContext || !g_lib.destroyContext || !g_lib.query || !g_lib.dispatch) {
			why = "the library lacks a function";
			return false;
		}
		if (g_lib.load (dir.c_str ()) != 0) { why = g_lib.error (); return false; }
		g_lib.loaded = true;
		return true;
	}

	void Message (uint32_t type, const wchar_t *msg)
	{
		std::string s;
		for (const wchar_t *p = msg; p && *p; p++) s += (char)(*p < 128 ? *p : '?');
		oapiWriteLogV ("VulkanClient: FSR 4 %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s.c_str());
	}

	FfxApiResource Resource (VkImage image, VkFormat fmt, VkExtent2D e, uint32_t usage, uint32_t state)
	{
		FfxApiResourceDescription d{};
		d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
		d.format = ffxApiGetSurfaceFormatVK (fmt);
		d.width = e.width, d.height = e.height, d.depth = 1;
		d.mipCount = 1;
		d.flags = FFX_API_RESOURCE_FLAGS_NONE;
		d.usage = usage;
		return ffxApiGetResourceVK ((void*)image, d, state);
	}
}

struct Fsr4Backend::Impl {
	vkc::Context *ctx = nullptr;
	ffxContext context = nullptr;
	uint64_t versionId = 0; // (FSR 4's among the DLL's providers)
	std::string versionName;
	uint32_t generation = ~0u;
	VkExtent2D renderSize{}, displaySize{};
	vkc::Image output;
	bool useReactive = true;

	bool FindVersion ()
	{
		// the versions AMD's DLL offers this GPU: FSR 4's (4.x) or none
		VkDeviceContext dc{ ctx->device, ctx->physicalDevice, FfxGetDeviceProc };
		uint64_t count = 0;
		ffxQueryDescGetVersions v{};
		v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		v.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		v.device = &dc, v.outputCount = &count;
		if (g_lib.query (nullptr, &v.header) != FFX_API_RETURN_OK || !count || count > 256) return false;
		std::vector<uint64_t> ids (count);
		std::vector<const char*> names (count);
		v.versionIds = ids.data (), v.versionNames = names.data ();
		if (g_lib.query (nullptr, &v.header) != FFX_API_RETURN_OK || count > ids.size ()) return false;
		for (uint64_t i = 0; i < count; i++)
			if (names[i] && names[i][0] == '4' && names[i][1] == '.') { versionId = ids[i], versionName = names[i]; return true; }
		return false;
	}

	bool CreateContext ()
	{
		DestroyContext ();
		renderSize = ctx->RenderExtent (), displaySize = ctx->Extent ();
		ffxCreateContextDescUpscaleVersion version{};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		version.version = FFX_UPSCALER_VERSION;
		ffxOverrideVersion over{};
		over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		over.header.pNext = &version.header;
		over.versionId = versionId;
		ffxCreateBackendVKDesc backend{};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
		backend.header.pNext = &over.header;
		backend.deviceContext = VkDeviceContext{ ctx->device, ctx->physicalDevice, FfxGetDeviceProc };
		ffxCreateContextDescUpscale up{};
		up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		up.header.pNext = &backend.header;
		up.maxRenderSize = { renderSize.width, renderSize.height };
		up.maxUpscaleSize = { displaySize.width, displaySize.height };
		up.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
			FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		up.fpMessage = Message;
		ffxReturnCode_t r = g_lib.createContext (&context, &up.header, nullptr);
		if (r != FFX_API_RETURN_OK) { context = nullptr; oapiWriteLogV ("VulkanClient: FSR 4: no context (%u)", (unsigned)r); return false; }
		// (the provider it came with: FSR 4's, not an older one in its place)
		ffxQueryGetProviderVersion pv{};
		pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (g_lib.query (&context, &pv.header) != FFX_API_RETURN_OK || pv.versionId != versionId) {
			oapiWriteLogV ("VulkanClient: FSR 4: the context is %s's, not FSR 4's", pv.versionName ? pv.versionName : "another version");
			DestroyContext ();
			return false;
		}

		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = OUTPUT_FORMAT;
		ici.extent = { displaySize.width, displaySize.height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &output.image, &output.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = output.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = OUTPUT_FORMAT;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &output.view));
		VkImage img = output.image;
		ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
			vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
		});
		generation = ctx->SwapchainGeneration ();
		oapiWriteLogV ("VulkanClient: FSR 4 upscaler (%s) %ux%u -> %ux%u", versionName.c_str (), renderSize.width, renderSize.height,
			displaySize.width, displaySize.height);
		return true;
	}

	void DestroyContext ()
	{
		if (!context) return;
		vkDeviceWaitIdle (ctx->device);
		g_lib.destroyContext (&context, nullptr);
		context = nullptr;
		if (output.image) ctx->DestroyImage (output);
		generation = ~0u;
	}
};

Fsr4Backend::Fsr4Backend () : impl (new Impl) {}
Fsr4Backend::~Fsr4Backend () {}

bool Fsr4Backend::Available ()
{
	// (off with ORBITER_VK_FSR4=0: tests)
	if (const char *e = getenv ("ORBITER_VK_FSR4")) if (!atoi (e)) return false;
	static const bool there = [] {
		std::string dir = Fsr4Dir ();
		std::error_code ec;
		return !dir.empty () && std::filesystem::exists (dir + "/" + LIB_NAME, ec) &&
			std::filesystem::exists (dir + "/amd_fidelityfx_upscaler_vk.dll", ec);
	}();
	return there;
}

bool Fsr4Backend::Init (vkc::Context *ctx)
{
	impl->ctx = ctx;
	if (const char *e = getenv ("ORBITER_VK_FSR4_REACTIVE")) impl->useReactive = atoi (e) != 0;
	std::string why;
	if (!OpenLib (why)) {
		oapiWriteLogV ("VulkanClient: FSR 4 could not be loaded (%s)", why.c_str ());
		return false;
	}
	if (!impl->FindVersion ()) {
		oapiWriteLogV ("VulkanClient: FSR 4: AMD's library offers this graphics card no FSR 4");
		return false;
	}
	return impl->CreateContext ();
}

void Fsr4Backend::Shutdown ()
{
	if (!impl->ctx) return;
	impl->DestroyContext ();
	impl->ctx = nullptr;
}

VkImageView Fsr4Backend::Dispatch (const FsrUpscaler::Inputs &in, float sharpness, bool debugView)
{
	vkc::Context *ctx = impl->ctx;
	if (impl->generation != ctx->SwapchainGeneration () && !impl->CreateContext ()) return VK_NULL_HANDLE;
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd ();
	VkExtent2D rs = impl->renderSize;
	// the depth buffer is read by the upscaler (it is cleared at the next use)
	vkc::ImageBarrier (cmd, in.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
		VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	// (the output: written by the upscaler in the general layout, read by us after)
	vkc::ImageBarrier (cmd, impl->output.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

	ffxDispatchDescUpscale d{};
	d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	d.commandList = cmd;
	d.color = Resource (in.color, in.colorFormat, rs, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.depth = Resource (in.depth, ctx->depthFormat, rs, FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.motionVectors = Resource (in.velocity, in.velocityFormat, rs, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	if (in.reactive && impl->useReactive)
		d.reactive = Resource (in.reactive, VK_FORMAT_R8_UNORM, rs, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.output = Resource (impl->output.image, OUTPUT_FORMAT, impl->displaySize, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
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
	if (debugView) d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
	ffxReturnCode_t r = g_lib.dispatch (&impl->context, &d.header);
	vkc::ImageBarrier (cmd, impl->output.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	if (r != FFX_API_RETURN_OK) {
		oapiWriteLogV ("VulkanClient: FSR 4 dispatch failed (%u)", (unsigned)r);
		return VK_NULL_HANDLE;
	}
	return impl->output.view;
}

#else // !VKC_HAVE_FSR4

struct Fsr4Backend::Impl {};
Fsr4Backend::Fsr4Backend () {}
Fsr4Backend::~Fsr4Backend () {}
bool Fsr4Backend::Available () { return false; }
bool Fsr4Backend::Init (vkc::Context *) { return false; }
void Fsr4Backend::Shutdown () {}
VkImageView Fsr4Backend::Dispatch (const FsrUpscaler::Inputs &, float, bool) { return VK_NULL_HANDLE; }

#endif
