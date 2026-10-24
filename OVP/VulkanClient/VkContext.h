// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Vulkan device, swapchain and per-frame resources
// =======================================================================

#ifndef __VKCONTEXT_H
#define __VKCONTEXT_H

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <functional>
#include <vector>

struct SDL_Window;

namespace vkc {

	void Check (VkResult res, const char *what);
#define VK_CHECK(expr) vkc::Check((expr), #expr)

	constexpr uint32_t FRAMES_IN_FLIGHT = 2;

	struct Buffer {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation alloc = VK_NULL_HANDLE;
		void *mapped = nullptr; // persistently mapped host-visible buffers
		VkDeviceSize size = 0;
	};

	struct Image {
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VmaAllocation alloc = VK_NULL_HANDLE;
	};

	class Context {
	public:
		bool Init (SDL_Window *window, bool enableValidation);
		void Shutdown ();

		// Frame control. BeginFrame returns false if no image could be acquired
		// (e.g. the window is minimised); the frame is then skipped.
		bool BeginFrame ();
		void BeginRendering ();
		void EndFrame ();
		bool InFrame () const { return inFrame; }

		VkCommandBuffer Cmd () const { return frames[frameIndex].cmd; }
		uint32_t FrameIndex () const { return frameIndex; }
		VkExtent2D Extent () const { return extent; }

		// Resource helpers
		Buffer CreateBuffer (VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
		void DestroyBuffer (Buffer &buf);
		Buffer CreateDeviceBuffer (const void *data, VkDeviceSize size, VkBufferUsageFlags usage);
		void DestroyImage (Image &img);

		/// Record and execute a command buffer, waiting for completion (resource uploads)
		void ImmediateSubmit (const std::function<void(VkCommandBuffer)> &record);

		/// Destroy a resource once the GPU no longer uses it
		void DeferDestroy (std::function<void()> fn);

		VkShaderModule CreateShaderModule (const unsigned char *code, size_t size);

		/// Read back the next presented frame. The callback receives the pixels
		/// (4 bytes per pixel in colorFormat, rows top to bottom).
		using CaptureFunc = std::function<void(const uint8_t *pixels, uint32_t w, uint32_t h, VkFormat fmt)>;
		void RequestCapture (CaptureFunc cb) { captureCb = std::move (cb); }

		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t queueFamily = 0;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkFormat colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
		VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
		VkPhysicalDeviceProperties deviceProps{};
		VkPhysicalDeviceFeatures deviceFeatures{};
		uint32_t swapchainImageCount = 0;

	private:
		bool CreateSwapchain ();
		void DestroySwapchain ();
		bool RecreateSwapchain ();
		void FlushDeferred (uint32_t frame);

		struct Frame {
			VkCommandPool pool = VK_NULL_HANDLE;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkSemaphore imageAvailable = VK_NULL_HANDLE;
			VkFence inFlight = VK_NULL_HANDLE;
			std::vector<std::function<void()>> deferred;
		} frames[FRAMES_IN_FLIGHT];

		SDL_Window *window = nullptr;
		VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		std::vector<VkImage> swapImages;
		std::vector<VkImageView> swapViews;
		std::vector<VkSemaphore> renderFinished; // per swapchain image
		Image depth;
		VkExtent2D extent{};
		VkCommandPool uploadPool = VK_NULL_HANDLE;
		uint32_t frameIndex = 0;
		uint32_t imageIndex = 0;
		bool inFrame = false;
		bool swapchainDirty = false;
		CaptureFunc captureCb;
	};

} // namespace vkc

#endif // !__VKCONTEXT_H
