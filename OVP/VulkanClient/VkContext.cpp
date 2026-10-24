// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VkContext.h"
#include "OrbiterAPI.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstring>

namespace vkc {

void Check (VkResult res, const char *what)
{
	if (res != VK_SUCCESS)
		oapiWriteLogError ("VulkanClient: %s failed (VkResult %d)", what, (int)res);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback (VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		oapiWriteLogV ("VulkanClient [validation]: %s", data->pMessage);
	return VK_FALSE;
}

// ======================================================================
// Initialisation

bool Context::Init (SDL_Window *wnd, bool enableValidation, bool offscreenMode)
{
	window = wnd;
	offscreen = offscreenMode;

	// Instance
	std::vector<const char*> extensions;
	if (!offscreen) {
		Uint32 nSdlExt = 0;
		const char *const *sdlExt = SDL_Vulkan_GetInstanceExtensions (&nSdlExt);
		if (sdlExt) extensions.assign (sdlExt, sdlExt + nSdlExt);
	}
	std::vector<const char*> layers;
	if (enableValidation) {
		uint32_t nLayer = 0;
		vkEnumerateInstanceLayerProperties (&nLayer, nullptr);
		std::vector<VkLayerProperties> avail (nLayer);
		vkEnumerateInstanceLayerProperties (&nLayer, avail.data());
		for (auto &l : avail)
			if (!strcmp (l.layerName, "VK_LAYER_KHRONOS_validation")) {
				layers.push_back ("VK_LAYER_KHRONOS_validation");
				extensions.push_back (VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			}
		if (layers.empty())
			oapiWriteLog ((char*)"VulkanClient: validation layer not available");
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "Orbiter";
	app.pEngineName = "Orbiter VulkanClient";
	app.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)extensions.size();
	ici.ppEnabledExtensionNames = extensions.data();
	ici.enabledLayerCount = (uint32_t)layers.size();
	ici.ppEnabledLayerNames = layers.data();
	if (vkCreateInstance (&ici, nullptr, &instance) != VK_SUCCESS) {
		oapiWriteLogError ("VulkanClient: could not create Vulkan 1.3 instance");
		return false;
	}

	if (!layers.empty()) {
		auto createMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr (instance, "vkCreateDebugUtilsMessengerEXT");
		VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		dci.pfnUserCallback = DebugCallback;
		if (createMessenger) createMessenger (instance, &dci, nullptr, &debugMessenger);
	}

	if (!offscreen && !SDL_Vulkan_CreateSurface (window, instance, nullptr, &surface)) {
		oapiWriteLogError ("VulkanClient: could not create window surface: %s", SDL_GetError());
		return false;
	}

	// Physical device: prefer a discrete GPU with a graphics+present queue and Vulkan 1.3
	uint32_t nDev = 0;
	vkEnumeratePhysicalDevices (instance, &nDev, nullptr);
	std::vector<VkPhysicalDevice> devs (nDev);
	vkEnumeratePhysicalDevices (instance, &nDev, devs.data());
	int bestScore = -1;
	for (auto pd : devs) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties (pd, &props);
		if (props.apiVersion < VK_API_VERSION_1_3) continue;
		uint32_t nq = 0;
		vkGetPhysicalDeviceQueueFamilyProperties (pd, &nq, nullptr);
		std::vector<VkQueueFamilyProperties> qprops (nq);
		vkGetPhysicalDeviceQueueFamilyProperties (pd, &nq, qprops.data());
		for (uint32_t q = 0; q < nq; q++) {
			VkBool32 present = offscreen ? VK_TRUE : VK_FALSE;
			if (!offscreen) vkGetPhysicalDeviceSurfaceSupportKHR (pd, q, surface, &present);
			if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
				int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
				             props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1);
				if (score > bestScore) {
					bestScore = score;
					physicalDevice = pd;
					queueFamily = q;
				}
				break;
			}
		}
	}
	if (!physicalDevice) {
		oapiWriteLogError ("VulkanClient: no Vulkan 1.3 device with graphics and present support found");
		return false;
	}
	vkGetPhysicalDeviceProperties (physicalDevice, &deviceProps);
	vkGetPhysicalDeviceFeatures (physicalDevice, &deviceFeatures);
	oapiWriteLogV ("VulkanClient: using device %s", deviceProps.deviceName);

	// Logical device
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.dynamicRendering = VK_TRUE;
	f13.synchronization2 = VK_TRUE;
	f13.shaderDemoteToHelperInvocation = VK_TRUE; // discard in the 2D shader
	VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &f13;
	f2.features.samplerAnisotropy = deviceFeatures.samplerAnisotropy;
	f2.features.textureCompressionBC = deviceFeatures.textureCompressionBC;
	f2.features.largePoints = deviceFeatures.largePoints;

	const char *devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &f2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = offscreen ? 0 : 1;
	dci.ppEnabledExtensionNames = devExt;
	if (vkCreateDevice (physicalDevice, &dci, nullptr, &device) != VK_SUCCESS) {
		oapiWriteLogError ("VulkanClient: could not create logical device");
		return false;
	}
	vkGetDeviceQueue (device, queueFamily, 0, &queue);

	VmaAllocatorCreateInfo aci{};
	aci.vulkanApiVersion = VK_API_VERSION_1_3;
	aci.physicalDevice = physicalDevice;
	aci.device = device;
	aci.instance = instance;
	VK_CHECK(vmaCreateAllocator (&aci, &allocator));

	// Per-frame resources
	for (auto &f : frames) {
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = queueFamily;
		VK_CHECK(vkCreateCommandPool (device, &pci, nullptr, &f.pool));
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = f.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers (device, &cai, &f.cmd));
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VK_CHECK(vkCreateSemaphore (device, &sci, nullptr, &f.imageAvailable));
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK(vkCreateFence (device, &fci, nullptr, &f.inFlight));
	}
	VkCommandPoolCreateInfo upci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	upci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	upci.queueFamilyIndex = queueFamily;
	VK_CHECK(vkCreateCommandPool (device, &upci, nullptr, &uploadPool));

	return CreateSwapchain ();
}

void Context::Drain ()
{
	if (!device) return;
	SubmitPending ();
	vkDeviceWaitIdle (device);
	for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) FlushDeferred (i);
}

void Context::Shutdown ()
{
	if (!device) return;
	vkDeviceWaitIdle (device);
	for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
		FlushDeferred (i);
		for (auto &b : frames[i].transient) DestroyBuffer (b);
		frames[i].transient.clear();
		vkDestroyCommandPool (device, frames[i].pool, nullptr);
		vkDestroySemaphore (device, frames[i].imageAvailable, nullptr);
		vkDestroyFence (device, frames[i].inFlight, nullptr);
	}
	vkDestroyCommandPool (device, uploadPool, nullptr);
	DestroySwapchain ();
	vmaDestroyAllocator (allocator);
	vkDestroyDevice (device, nullptr);
	if (surface) vkDestroySurfaceKHR (instance, surface, nullptr);
	if (debugMessenger) {
		auto destroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr (instance, "vkDestroyDebugUtilsMessengerEXT");
		if (destroyMessenger) destroyMessenger (instance, debugMessenger, nullptr);
	}
	vkDestroyInstance (instance, nullptr);
	device = VK_NULL_HANDLE;
}

// ======================================================================
// Swapchain

bool Context::CreateSwapchain ()
{
	if (offscreen) {
		// render targets in place of a swapchain, one per frame in flight
		colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
		extent = offscreenExtent;
		swapchainImageCount = FRAMES_IN_FLIGHT;
		offImages.resize (swapchainImageCount);
		swapImages.resize (swapchainImageCount);
		swapViews.resize (swapchainImageCount);
		for (uint32_t i = 0; i < swapchainImageCount; i++) {
			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = colorFormat;
			ici.extent = { extent.width, extent.height, 1 };
			ici.mipLevels = 1;
			ici.arrayLayers = 1;
			ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_OPTIMAL;
			ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			VmaAllocationCreateInfo aci{};
			aci.usage = VMA_MEMORY_USAGE_AUTO;
			VK_CHECK(vmaCreateImage (allocator, &ici, &aci, &offImages[i].image, &offImages[i].alloc, nullptr));
			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = offImages[i].image;
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = colorFormat;
			vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VK_CHECK(vkCreateImageView (device, &vci, nullptr, &offImages[i].view));
			swapImages[i] = offImages[i].image;
			swapViews[i] = offImages[i].view;
		}
	} else {
		VkSurfaceCapabilitiesKHR caps;
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR (physicalDevice, surface, &caps));

		uint32_t nFmt = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR (physicalDevice, surface, &nFmt, nullptr);
		std::vector<VkSurfaceFormatKHR> fmts (nFmt);
		vkGetPhysicalDeviceSurfaceFormatsKHR (physicalDevice, surface, &nFmt, fmts.data());
		VkSurfaceFormatKHR fmt = fmts[0];
		for (auto &f : fmts)
			if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
				f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; break; }
		colorFormat = fmt.format;

		if (caps.currentExtent.width != 0xFFFFFFFF) {
			extent = caps.currentExtent;
		} else {
			int w, h;
			SDL_GetWindowSizeInPixels (window, &w, &h);
			extent.width = std::clamp ((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
			extent.height = std::clamp ((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
		}
		if (!extent.width || !extent.height) return false; // minimised

		uint32_t nImg = std::max (caps.minImageCount + 1, 3u);
		if (caps.maxImageCount) nImg = std::min (nImg, caps.maxImageCount);

		VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sci.surface = surface;
		sci.minImageCount = nImg;
		sci.imageFormat = fmt.format;
		sci.imageColorSpace = fmt.colorSpace;
		sci.imageExtent = extent;
		sci.imageArrayLayers = 1;
		sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sci.preTransform = caps.currentTransform;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync, always supported
		sci.clipped = VK_TRUE;
		if (vkCreateSwapchainKHR (device, &sci, nullptr, &swapchain) != VK_SUCCESS) {
			oapiWriteLogError ("VulkanClient: could not create swapchain");
			return false;
		}

		vkGetSwapchainImagesKHR (device, swapchain, &swapchainImageCount, nullptr);
		swapImages.resize (swapchainImageCount);
		vkGetSwapchainImagesKHR (device, swapchain, &swapchainImageCount, swapImages.data());
		swapViews.resize (swapchainImageCount);
		renderFinished.resize (swapchainImageCount);
		for (uint32_t i = 0; i < swapchainImageCount; i++) {
			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = swapImages[i];
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = colorFormat;
			vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VK_CHECK(vkCreateImageView (device, &vci, nullptr, &swapViews[i]));
			VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			VK_CHECK(vkCreateSemaphore (device, &semi, nullptr, &renderFinished[i]));
		}

	}

	// Depth buffer
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = depthFormat;
	ici.extent = { extent.width, extent.height, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (allocator, &ici, &aci, &depth.image, &depth.alloc, nullptr));
	VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	dvci.image = depth.image;
	dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	dvci.format = depthFormat;
	dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (device, &dvci, nullptr, &depth.view));
	return true;
}

void Context::DestroySwapchain ()
{
	DestroyImage (depth);
	if (offscreen) {
		for (auto &img : offImages) DestroyImage (img);
		offImages.clear();
		swapImages.clear();
		swapViews.clear();
		return;
	}
	for (auto v : swapViews) vkDestroyImageView (device, v, nullptr);
	for (auto s : renderFinished) vkDestroySemaphore (device, s, nullptr);
	swapViews.clear();
	renderFinished.clear();
	if (swapchain) vkDestroySwapchainKHR (device, swapchain, nullptr);
	swapchain = VK_NULL_HANDLE;
}

bool Context::RecreateSwapchain ()
{
	vkDeviceWaitIdle (device);
	DestroySwapchain ();
	swapchainDirty = false;
	return CreateSwapchain ();
}

// ======================================================================
// Frames

void ImageBarrier (VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
	VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = srcStage;
	b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

VkCommandBuffer Context::Rec ()
{
	Frame &f = frames[frameIndex];
	if (!recording) {
		vkWaitForFences (device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
		FlushDeferred (frameIndex);
		f.transientUsed = 0;
		f.transientChunk = 0;
		vkResetCommandBuffer (f.cmd, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer (f.cmd, &bi));
		recording = true;
		bbState = BB_NONE;
	}
	return f.cmd;
}

bool Context::BeginFrame ()
{
	if (inFrame) return true;
	if ((!offscreen && !swapchain) || swapchainDirty) {
		if (!RecreateSwapchain ()) return false;
	}
	Rec ();
	Frame &f = frames[frameIndex];
	if (offscreen) {
		imageIndex = frameIndex;
		inFrame = true;
		bbState = BB_NONE;
		return true;
	}
	VkResult res = vkAcquireNextImageKHR (device, swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
	if (res == VK_ERROR_OUT_OF_DATE_KHR) {
		swapchainDirty = true;
		return false;
	}
	if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
		VK_CHECK(res);
		return false;
	}
	inFrame = true;
	bbState = BB_NONE;
	return true;
}

void Context::SetViewportScissor (VkExtent2D ext)
{
	VkCommandBuffer cmd = frames[frameIndex].cmd;
	VkViewport vp{ 0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
	VkRect2D sc{ {0, 0}, ext };
	vkCmdSetViewport (cmd, 0, 1, &vp);
	vkCmdSetScissor (cmd, 0, 1, &sc);
}

void Context::BindBackbuffer (bool withDepth)
{
	if (!inFrame) return;
	if (cur.backbuffer && cur.depth == withDepth) return;
	EndTarget ();
	VkCommandBuffer cmd = frames[frameIndex].cmd;

	bool clearColor = !(bbState & BB_COLOR);
	if (clearColor) {
		ImageBarrier (cmd, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		bbState |= BB_COLOR;
	}
	VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	color.imageView = swapViews[imageIndex];
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

	VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	if (withDepth) {
		bool clearDepth = !(bbState & BB_DEPTH);
		if (clearDepth) {
			ImageBarrier (cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
			bbState |= BB_DEPTH;
		}
		dep.imageView = depth.view;
		dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		dep.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		dep.clearValue.depthStencil = { 0.0f, 0 }; // reversed z: far = 0
	}

	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = { {0, 0}, extent };
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &color;
	ri.pDepthAttachment = withDepth ? &dep : nullptr;
	vkCmdBeginRendering (cmd, &ri);
	SetViewportScissor (extent);
	cur = CurrentTarget();
	cur.active = true;
	cur.backbuffer = true;
	cur.depth = withDepth;
	cur.format = colorFormat;
	cur.extent = extent;
}

void Context::BindTarget (const Target &t)
{
	if (cur.active && !cur.backbuffer && cur.image == t.image) return;
	EndTarget ();
	VkCommandBuffer cmd = Rec ();
	// surfaces rest in shader-read layout between rendering scopes
	ImageBarrier (cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

	VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	color.imageView = t.view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = { {0, 0}, t.extent };
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &color;
	vkCmdBeginRendering (cmd, &ri);
	SetViewportScissor (t.extent);
	cur = CurrentTarget();
	cur.active = true;
	cur.image = t.image;
	cur.format = t.format;
	cur.extent = t.extent;
}

void Context::BindDepthTarget (const Target &t)
{
	EndTarget ();
	VkCommandBuffer cmd = Rec ();
	ImageBarrier (cmd, t.image, VK_IMAGE_ASPECT_DEPTH_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	dep.imageView = t.view;
	dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	dep.clearValue.depthStencil = { 1.0f, 0 };
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = { {0, 0}, t.extent };
	ri.layerCount = 1;
	ri.pDepthAttachment = &dep;
	vkCmdBeginRendering (cmd, &ri);
	SetViewportScissor (t.extent);
	cur = CurrentTarget();
	cur.active = true;
	cur.depthOnly = true;
	cur.image = t.image;
	cur.format = t.format;
	cur.extent = t.extent;
}

void Context::EndTarget ()
{
	if (!cur.active) return;
	VkCommandBuffer cmd = frames[frameIndex].cmd;
	vkCmdEndRendering (cmd);
	if (cur.depthOnly) {
		ImageBarrier (cmd, cur.image, VK_IMAGE_ASPECT_DEPTH_BIT,
			VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	} else if (!cur.backbuffer) {
		ImageBarrier (cmd, cur.image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	}
	cur = CurrentTarget();
}

Context::Transient Context::AllocTransient (VkDeviceSize size)
{
	constexpr VkDeviceSize CHUNK = 4u << 20;
	Rec ();
	Frame &f = frames[frameIndex];
	size = (size + 15) & ~(VkDeviceSize)15;
	while (true) {
		if (f.transientChunk < f.transient.size()) {
			Buffer &b = f.transient[f.transientChunk];
			if (f.transientUsed + size <= b.size) {
				Transient t{ b.buffer, f.transientUsed, (uint8_t*)b.mapped + f.transientUsed };
				f.transientUsed += size;
				return t;
			}
			f.transientChunk++;
			f.transientUsed = 0;
			continue;
		}
		f.transient.push_back (CreateBuffer (std::max (CHUNK, size),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	}
}

void Context::UpdateBuffer (VkBuffer dst, VkDeviceSize offset, const void *data, VkDeviceSize size)
{
	if (!size) return;
	Transient src = AllocTransient (size);
	memcpy (src.ptr, data, size);
	VkCommandBuffer cmd = Rec ();
	EndTarget (); // transfers are not allowed inside a rendering scope

	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
	mb.srcAccessMask = 0; // write-after-read: execution dependency only
	mb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2 (cmd, &dep);

	VkBufferCopy region{ src.offset, offset, size };
	vkCmdCopyBuffer (cmd, src.buffer, dst, 1, &region);

	mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
	mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

void Context::EndFrame ()
{
	if (!inFrame) { SubmitPending (); return; }
	Frame &f = frames[frameIndex];
	BindBackbuffer (false); // make sure the image was initialised even if nothing was drawn
	EndTarget ();

	Buffer capture;
	if (captureCb) {
		// copy the rendered image into a host buffer before presenting it
		capture = CreateBuffer ((VkDeviceSize)extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
		ImageBarrier (f.cmd, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { extent.width, extent.height, 1 };
		vkCmdCopyImageToBuffer (f.cmd, swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture.buffer, 1, &region);
		if (!offscreen)
			ImageBarrier (f.cmd, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
	} else if (!offscreen) {
		ImageBarrier (f.cmd, swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
	}
	VK_CHECK(vkEndCommandBuffer (f.cmd));
	recording = false;

	VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	wait.semaphore = f.imageAvailable;
	wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	signal.semaphore = offscreen ? VK_NULL_HANDLE : renderFinished[imageIndex];
	signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = f.cmd;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = offscreen ? 0 : 1;
	si.pWaitSemaphoreInfos = &wait;
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	si.signalSemaphoreInfoCount = offscreen ? 0 : 1;
	si.pSignalSemaphoreInfos = &signal;
	vkResetFences (device, 1, &f.inFlight);
	VK_CHECK(vkQueueSubmit2 (queue, 1, &si, f.inFlight));

	if (capture.buffer) {
		vkWaitForFences (device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
		vmaInvalidateAllocation (allocator, capture.alloc, 0, VK_WHOLE_SIZE);
		CaptureFunc cb = std::move (captureCb);
		captureCb = nullptr;
		cb ((const uint8_t*)capture.mapped, extent.width, extent.height, colorFormat);
		DestroyBuffer (capture);
	}

	if (offscreen) {
		inFrame = false;
		frameIndex = (frameIndex + 1) % FRAMES_IN_FLIGHT;
		return;
	}
	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &renderFinished[imageIndex];
	pi.swapchainCount = 1;
	pi.pSwapchains = &swapchain;
	pi.pImageIndices = &imageIndex;
	VkResult res = vkQueuePresentKHR (queue, &pi);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		swapchainDirty = true;

	inFrame = false;
	frameIndex = (frameIndex + 1) % FRAMES_IN_FLIGHT;
}

void Context::SubmitPending ()
{
	if (!recording || inFrame) return;
	Frame &f = frames[frameIndex];
	EndTarget ();
	VK_CHECK(vkEndCommandBuffer (f.cmd));
	recording = false;
	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = f.cmd;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	vkResetFences (device, 1, &f.inFlight);
	VK_CHECK(vkQueueSubmit2 (queue, 1, &si, f.inFlight));
	frameIndex = (frameIndex + 1) % FRAMES_IN_FLIGHT;
}

void Context::FlushDeferred (uint32_t frame)
{
	auto &list = frames[frame].deferred;
	for (auto &fn : list) fn();
	list.clear();
}

void Context::DeferDestroy (std::function<void()> fn)
{
	// destroyed when this frame slot comes round again, i.e. after its fence has signalled
	frames[frameIndex].deferred.push_back (std::move (fn));
}

// ======================================================================
// Resource helpers

Buffer Context::CreateBuffer (VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
	Buffer buf;
	buf.size = size;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = usage;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if (hostVisible)
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info{};
	VK_CHECK(vmaCreateBuffer (allocator, &bci, &aci, &buf.buffer, &buf.alloc, &info));
	buf.mapped = info.pMappedData;
	return buf;
}

void Context::DestroyBuffer (Buffer &buf)
{
	if (buf.buffer) vmaDestroyBuffer (allocator, buf.buffer, buf.alloc);
	buf = Buffer();
}

Buffer Context::CreateDeviceBuffer (const void *data, VkDeviceSize size, VkBufferUsageFlags usage)
{
	Buffer staging = CreateBuffer (size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
	memcpy (staging.mapped, data, size);
	Buffer buf = CreateBuffer (size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	ImmediateSubmit ([&](VkCommandBuffer cmd) {
		VkBufferCopy region{ 0, 0, size };
		vkCmdCopyBuffer (cmd, staging.buffer, buf.buffer, 1, &region);
	});
	DestroyBuffer (staging);
	return buf;
}

void Context::DestroyImage (Image &img)
{
	if (img.view) vkDestroyImageView (device, img.view, nullptr);
	if (img.image) vmaDestroyImage (allocator, img.image, img.alloc);
	img = Image();
}

void Context::ImmediateSubmit (const std::function<void(VkCommandBuffer)> &record)
{
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = uploadPool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VK_CHECK(vkAllocateCommandBuffers (device, &cai, &cmd));
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer (cmd, &bi);
	record (cmd);
	vkEndCommandBuffer (cmd);
	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = cmd;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	VK_CHECK(vkQueueSubmit2 (queue, 1, &si, VK_NULL_HANDLE));
	vkQueueWaitIdle (queue);
	vkFreeCommandBuffers (device, uploadPool, 1, &cmd);
}

VkShaderModule Context::CreateShaderModule (const unsigned char *code, size_t size)
{
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = size;
	ci.pCode = reinterpret_cast<const uint32_t*>(code);
	VkShaderModule mod = VK_NULL_HANDLE;
	VK_CHECK(vkCreateShaderModule (device, &ci, nullptr, &mod));
	return mod;
}

} // namespace vkc
