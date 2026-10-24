// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Textures and texture loading
// =======================================================================

#ifndef __VKTEXTURE_H
#define __VKTEXTURE_H

#include "VkContext.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class VkTexture {
public:
	static constexpr uint32_t NO_CKEY = 0xFFFFFFFF;

	vkc::Image img;
	VkDescriptorSet set = VK_NULL_HANDLE; // sampler binding (mesh, 2D and ImGui pipelines)
	uint32_t width = 0, height = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	bool renderTarget = false;            // single-level RGBA8 image usable as colour attachment
	uint32_t ckey = NO_CKEY;              // colour key for blits (0xRRGGBB)
	std::string name;                     // cache key (empty if not cached)
	int refs = 1;

	vkc::Target AsTarget () const { return { img.image, img.view, format, { width, height } }; }
};

class TextureManager {
public:
	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// Load a texture file from the Textures directory. Shared textures are
	/// cached by name; unshared ones are private copies (e.g. for drawing on).
	/// With fullPath, fname is relative to the Orbiter root instead.
	VkTexture *Load (const char *fname, bool shared = true, bool fullPath = false);

	/// Create a texture from RGBA8 pixel data (sRGB)
	VkTexture *CreateRGBA (uint32_t w, uint32_t h, const uint8_t *rgba);

	/// Create a texture from a DDS file in memory. deferred: record the upload
	/// into the current frame instead of waiting for it (texture streaming)
	VkTexture *CreateFromDDS (const uint8_t *file, size_t size, bool deferred);

	/// Create a render-target surface, cleared to opaque black
	VkTexture *CreateSurface (uint32_t w, uint32_t h);

	/// Create a coverage texture from 8-bit values, sampled as (1,1,1,value)
	VkTexture *CreateCoverage (uint32_t w, uint32_t h, const uint8_t *data);

	/// Create an RGBA8 render-target image (contents undefined, layout UNDEFINED)
	bool CreateTargetImage (uint32_t w, uint32_t h, vkc::Image &img);

	/// Replace the image of a texture by a render-target image of the same
	/// size (in shader-read layout). The old image is released when the GPU
	/// no longer uses it.
	void ReplaceImage (VkTexture *tex, const vkc::Image &img);

	void AddRef (VkTexture *tex) { if (tex) tex->refs++; }
	void Release (VkTexture *tex);

	VkDescriptorSetLayout Layout () const { return layout; }
	VkTexture *White () const { return white; }

private:
	struct DDSImage {
		VkFormat fmt = VK_FORMAT_UNDEFINED;
		uint32_t w = 0, h = 0, nMip = 0;
		std::vector<size_t> ofs;
		const uint8_t *data = nullptr;
		size_t size = 0;
		bool genMips = false;
		std::vector<uint8_t> converted;
	};
	bool ParseDDS (const uint8_t *file, size_t size, DDSImage &img) const;
	VkTexture *LoadDDS (const std::string &path);
	VkTexture *CreateImage (VkFormat fmt, uint32_t w, uint32_t h, uint32_t nMip,
		const uint8_t *data, const std::vector<size_t> &mipOffsets, size_t dataSize, bool genMips,
		const VkComponentMapping *swizzle = nullptr, bool deferred = false);
	VkDescriptorSet AllocSet (VkImageView view);
	void Destroy (VkTexture *tex);

	vkc::Context *ctx = nullptr;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkTexture *white = nullptr;
	std::unordered_map<std::string, VkTexture*> cache;
	std::unordered_set<VkTexture*> live; // all existing textures
};

#endif // !__VKTEXTURE_H
