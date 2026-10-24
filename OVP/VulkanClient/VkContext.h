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

	/// An offscreen colour image that can be rendered to. Between rendering
	/// scopes it is kept in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
	struct Target {
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkExtent2D extent{};
	};

	void ImageBarrier (VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
		VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
		VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

	class Context {
	public:
		/// offscreen: render into images instead of a window swapchain
		/// (no presentation; the size is set with SetOffscreenExtent)
		bool Init (SDL_Window *window, bool enableValidation, bool offscreen = false);
		void SetOffscreenExtent (uint32_t w, uint32_t h) { offscreenExtent = { w, h }; }
		bool Offscreen () const { return offscreen; }
		void Shutdown ();
		/// Wait for the GPU and run all pending deferred destructions
		void Drain ();

		// Frame control. All GPU work is recorded into the command buffer of
		// the current frame slot, which is opened on demand (Rec), so that
		// offscreen surfaces can be drawn to between frames as well.
		// BeginFrame acquires a swapchain image; it returns false if none
		// could be acquired (e.g. the window is minimised). EndFrame presents
		// the image, or just submits the recorded work if no frame was begun.
		VkCommandBuffer Rec ();
		bool BeginFrame ();
		void EndFrame ();
		void SubmitPending ();
		bool InFrame () const { return inFrame; }

		// Rendering scopes. Binding a target ends the current scope if it
		// refers to a different target. The backbuffer is cleared on its
		// first use in a frame.
		void BindBackbuffer (bool withDepth);
		void BindTarget (const Target &t);
		void EndTarget ();
		bool BoundTo (VkImage image) const { return cur.active && !cur.backbuffer && cur.image == image; }
		VkFormat BoundFormat () const { return cur.format; }
		VkExtent2D BoundExtent () const { return cur.extent; }

		/// Host-visible memory valid for the current frame (vertex/index/uniform data)
		struct Transient { VkBuffer buffer; VkDeviceSize offset; void *ptr; };
		Transient AllocTransient (VkDeviceSize size);

		/// Record an update of a device buffer region used as vertex/index
		/// data, ordered with respect to earlier and later draws
		void UpdateBuffer (VkBuffer dst, VkDeviceSize offset, const void *data, VkDeviceSize size);

		VkCommandBuffer Cmd () { return Rec (); }
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
		void SetViewportScissor (VkExtent2D ext);

		struct Frame {
			VkCommandPool pool = VK_NULL_HANDLE;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkSemaphore imageAvailable = VK_NULL_HANDLE;
			VkFence inFlight = VK_NULL_HANDLE;
			std::vector<std::function<void()>> deferred;
			std::vector<Buffer> transient;
			size_t transientChunk = 0;
			VkDeviceSize transientUsed = 0;
		} frames[FRAMES_IN_FLIGHT];

		struct CurrentTarget {
			bool active = false;
			bool backbuffer = false;
			bool depth = false;
			VkImage image = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkExtent2D extent{};
		} cur;
		enum { BB_NONE = 0, BB_COLOR = 1, BB_DEPTH = 2 };
		int bbState = BB_NONE;   // backbuffer attachments initialised in this frame
		bool recording = false;
		bool offscreen = false;
		VkExtent2D offscreenExtent{ 1280, 800 };
		std::vector<Image> offImages;

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
