// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Textures and texture loading
// =======================================================================

#ifndef __VKTEXTURE_H
#define __VKTEXTURE_H

#include "VkContext.h"
#include <string>
#include <unordered_map>

class VkTexture {
public:
	vkc::Image img;
	VkDescriptorSet set = VK_NULL_HANDLE; // sampler binding for the mesh pipeline
	uint32_t width = 0, height = 0;
	std::string name;                     // cache key (empty if not cached)
	int refs = 1;
};

class TextureManager {
public:
	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// Load a texture file from the Textures directory (cached by name)
	VkTexture *Load (const char *fname);

	/// Create a texture from RGBA8 pixel data (sRGB)
	VkTexture *CreateRGBA (uint32_t w, uint32_t h, const uint8_t *rgba);

	void AddRef (VkTexture *tex) { if (tex) tex->refs++; }
	void Release (VkTexture *tex);

	VkDescriptorSetLayout Layout () const { return layout; }
	VkTexture *White () const { return white; }

private:
	VkTexture *LoadDDS (const std::string &path);
	VkTexture *CreateImage (VkFormat fmt, uint32_t w, uint32_t h, uint32_t nMip,
		const uint8_t *data, const std::vector<size_t> &mipOffsets, size_t dataSize, bool genMips);
	void Destroy (VkTexture *tex);

	vkc::Context *ctx = nullptr;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkTexture *white = nullptr;
	std::unordered_map<std::string, VkTexture*> cache;
};

#endif // !__VKTEXTURE_H
