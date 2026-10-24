// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VkTexture.h"
#include "ImageIO.h"
#include <cctype>
#include "OrbiterAPI.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// ======================================================================
// DDS file format (see the DirectX documentation of DDS_HEADER)

namespace {

	struct DDSPixelFormat {
		uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
	};

	struct DDSHeader {
		uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
		uint32_t reserved1[11];
		DDSPixelFormat pf;
		uint32_t caps, caps2, caps3, caps4, reserved2;
	};
	static_assert(sizeof(DDSHeader) == 124, "DDS header size");

	constexpr uint32_t FourCC (char a, char b, char c, char d)
	{
		return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
	}

	constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
	constexpr uint32_t DDPF_FOURCC = 0x4;
	constexpr uint32_t DDPF_RGB = 0x40;

	int strcasecmp_portable (const char *a, const char *b)
	{
		for (; *a && *b; a++, b++) {
			int d = tolower ((unsigned char)*a) - tolower ((unsigned char)*b);
			if (d) return d;
		}
		return (unsigned char)*a - (unsigned char)*b;
	}

	// Extract a channel given its bit mask, scaled to 8 bits
	inline uint8_t Channel (uint32_t pixel, uint32_t mask)
	{
		if (!mask) return 0;
		int shift = 0;
		while (!((mask >> shift) & 1)) shift++;
		uint32_t v = (pixel & mask) >> shift;
		uint32_t max = mask >> shift;
		return (uint8_t)((v * 255 + max / 2) / max);
	}

} // namespace

// ======================================================================

bool TextureManager::Init (vkc::Context *c)
{
	ctx = c;

	VkDescriptorSetLayoutBinding b{};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 1;
	lci.pBindings = &b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &layout));

	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pci.maxSets = 8192;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	if (ctx->deviceFeatures.samplerAnisotropy) {
		sci.anisotropyEnable = VK_TRUE;
		sci.maxAnisotropy = std::min (8.0f, ctx->deviceProps.limits.maxSamplerAnisotropy);
	}
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	const uint8_t px[4] = { 255, 255, 255, 255 };
	white = CreateRGBA (1, 1, px);
	return true;
}

void TextureManager::Shutdown ()
{
	if (!ctx) return;
	for (auto &it : cache) Destroy (it.second);
	cache.clear();
	if (white) Destroy (white);
	white = nullptr;
	vkDestroySampler (ctx->device, sampler, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, layout, nullptr);
	ctx = nullptr;
}

VkTexture *TextureManager::Load (const char *fname, bool shared, bool fullPath)
{
	std::string key = (fullPath ? std::string (":") : std::string()) + fname;
	if (shared) {
		auto it = cache.find (key);
		if (it != cache.end()) {
			it->second->refs++;
			return it->second;
		}
	}
	std::string path = fullPath ? std::string (fname) : std::string ("Textures\\") + fname;
	VkTexture *tex = nullptr;
	size_t len = path.size();
	if (len > 4 && (!strcasecmp_portable (path.c_str() + len - 4, ".png"))) {
		uint32_t w, h;
		std::vector<uint8_t> rgba;
		if (image::ReadPNG (path, w, h, rgba))
			tex = CreateRGBA (w, h, rgba.data());
		else
			oapiWriteLogV ("VulkanClient: could not read %s", path.c_str());
	} else {
		tex = LoadDDS (path);
	}
	if (tex && shared) {
		tex->name = key;
		cache[key] = tex;
	}
	return tex;
}

void TextureManager::Release (VkTexture *tex)
{
	if (!tex || tex == white || !ctx) return;
	if (--tex->refs > 0) return;
	if (!tex->name.empty()) cache.erase (tex->name);
	vkc::Context *c = ctx;
	VkDescriptorPool p = pool;
	c->DeferDestroy ([c, p, tex]() {
		if (tex->set) vkFreeDescriptorSets (c->device, p, 1, &tex->set);
		c->DestroyImage (tex->img);
		delete tex;
	});
}

void TextureManager::Destroy (VkTexture *tex)
{
	if (tex->set) vkFreeDescriptorSets (ctx->device, pool, 1, &tex->set);
	ctx->DestroyImage (tex->img);
	delete tex;
}

VkTexture *TextureManager::CreateRGBA (uint32_t w, uint32_t h, const uint8_t *rgba)
{
	std::vector<size_t> ofs = { 0 };
	return CreateImage (VK_FORMAT_R8G8B8A8_SRGB, w, h, 1, rgba, ofs, (size_t)w * h * 4, true);
}

VkTexture *TextureManager::CreateCoverage (uint32_t w, uint32_t h, const uint8_t *data)
{
	std::vector<size_t> ofs = { 0 };
	VkComponentMapping swz = { VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R };
	return CreateImage (VK_FORMAT_R8_UNORM, w, h, 1, data, ofs, (size_t)w * h, false, &swz);
}

bool TextureManager::CreateTargetImage (uint32_t w, uint32_t h, vkc::Image &img)
{
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_SRGB;
	ici.extent = { w, h, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if (vmaCreateImage (ctx->allocator, &ici, &aci, &img.image, &img.alloc, nullptr) != VK_SUCCESS)
		return false;
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ici.format;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &img.view));
	return true;
}

VkTexture *TextureManager::CreateSurface (uint32_t w, uint32_t h)
{
	if (!w || !h) return nullptr;
	VkTexture *tex = new VkTexture;
	if (!CreateTargetImage (w, h, tex->img)) {
		delete tex;
		return nullptr;
	}
	tex->width = w, tex->height = h;
	tex->format = VK_FORMAT_R8G8B8A8_SRGB;
	tex->renderTarget = true;
	VkImage image = tex->img.image;
	ctx->ImmediateSubmit ([image](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkClearColorValue black = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage (cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		vkc::ImageBarrier (cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	tex->set = AllocSet (tex->img.view);
	return tex;
}

void TextureManager::ReplaceImage (VkTexture *tex, const vkc::Image &img)
{
	// Descriptor sets may still be referenced by recorded or executing
	// command buffers, so the texture gets a new set rather than an update.
	vkc::Context *c = ctx;
	VkDescriptorPool p = pool;
	vkc::Image old = tex->img;
	VkDescriptorSet oldSet = tex->set;
	c->DeferDestroy ([c, p, old, oldSet]() mutable {
		if (oldSet) vkFreeDescriptorSets (c->device, p, 1, &oldSet);
		c->DestroyImage (old);
	});
	tex->img = img;
	tex->set = AllocSet (img.view);
	tex->format = VK_FORMAT_R8G8B8A8_SRGB;
	tex->renderTarget = true;
}

VkDescriptorSet TextureManager::AllocSet (VkImageView view)
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &layout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));
	VkDescriptorImageInfo dii{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = set;
	wr.dstBinding = 0;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	return set;
}

// ======================================================================
// DDS loading

VkTexture *TextureManager::LoadDDS (const std::string &path)
{
	FILE *f = fopen (path.c_str(), "rb");
	if (!f) return nullptr;
	std::vector<uint8_t> file;
	fseek (f, 0, SEEK_END);
	long fsize = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (fsize > 0) {
		file.resize (fsize);
		if (fread (file.data(), 1, fsize, f) != (size_t)fsize) file.clear();
	}
	fclose (f);

	if (file.size() < 4 + sizeof(DDSHeader) || memcmp (file.data(), "DDS ", 4)) {
		oapiWriteLogV ("VulkanClient: not a DDS file: %s", path.c_str());
		return nullptr;
	}
	DDSHeader hdr;
	memcpy (&hdr, file.data() + 4, sizeof(hdr));
	const uint8_t *data = file.data() + 4 + sizeof(hdr);
	size_t dataSize = file.size() - 4 - sizeof(hdr);
	uint32_t w = hdr.width, h = hdr.height;
	uint32_t nMip = std::max (1u, hdr.mipMapCount);
	if (!w || !h) return nullptr;

	if (hdr.pf.flags & DDPF_FOURCC) {
		VkFormat fmt;
		uint32_t blockBytes;
		switch (hdr.pf.fourCC) {
		case FourCC('D','X','T','1'): fmt = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; blockBytes = 8;  break;
		case FourCC('D','X','T','2'):
		case FourCC('D','X','T','3'): fmt = VK_FORMAT_BC2_SRGB_BLOCK;      blockBytes = 16; break;
		case FourCC('D','X','T','4'):
		case FourCC('D','X','T','5'): fmt = VK_FORMAT_BC3_SRGB_BLOCK;      blockBytes = 16; break;
		default:
			oapiWriteLogV ("VulkanClient: unsupported DDS format in %s", path.c_str());
			return nullptr;
		}
		if (!ctx->deviceFeatures.textureCompressionBC) {
			oapiWriteLogV ("VulkanClient: device lacks BC texture support, cannot load %s", path.c_str());
			return nullptr;
		}
		std::vector<size_t> ofs;
		size_t pos = 0;
		uint32_t mw = w, mh = h;
		for (uint32_t i = 0; i < nMip; i++) {
			size_t sz = (size_t)std::max (1u, (mw + 3) / 4) * std::max (1u, (mh + 3) / 4) * blockBytes;
			if (pos + sz > dataSize) { nMip = i; break; }
			ofs.push_back (pos);
			pos += sz;
			mw = std::max (1u, mw / 2), mh = std::max (1u, mh / 2);
		}
		if (!nMip) return nullptr;
		return CreateImage (fmt, w, h, nMip, data, ofs, pos, false);
	}

	if (hdr.pf.flags & DDPF_RGB) {
		// uncompressed: convert the top level to RGBA8 and generate the mip chain
		uint32_t bpp = hdr.pf.rgbBitCount / 8;
		if (bpp < 2 || bpp > 4 || dataSize < (size_t)w * h * bpp) {
			oapiWriteLogV ("VulkanClient: unsupported DDS pixel format in %s", path.c_str());
			return nullptr;
		}
		bool hasAlpha = (hdr.pf.flags & DDPF_ALPHAPIXELS) && hdr.pf.aMask;
		std::vector<uint8_t> rgba ((size_t)w * h * 4);
		for (size_t i = 0; i < (size_t)w * h; i++) {
			uint32_t px = 0;
			memcpy (&px, data + i * bpp, bpp);
			rgba[i*4+0] = Channel (px, hdr.pf.rMask);
			rgba[i*4+1] = Channel (px, hdr.pf.gMask);
			rgba[i*4+2] = Channel (px, hdr.pf.bMask);
			rgba[i*4+3] = hasAlpha ? Channel (px, hdr.pf.aMask) : 255;
		}
		return CreateRGBA (w, h, rgba.data());
	}

	oapiWriteLogV ("VulkanClient: unsupported DDS pixel format in %s", path.c_str());
	return nullptr;
}

// ======================================================================

VkTexture *TextureManager::CreateImage (VkFormat fmt, uint32_t w, uint32_t h, uint32_t nMip,
	const uint8_t *data, const std::vector<size_t> &mipOffsets, size_t dataSize, bool genMips,
	const VkComponentMapping *swizzle)
{
	if (genMips && nMip == 1) {
		uint32_t m = std::max (w, h);
		while (m > 1) { m >>= 1; nMip++; }
	}

	VkTexture *tex = new VkTexture;
	tex->width = w, tex->height = h;
	tex->format = fmt;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = { w, h, 1 };
	ici.mipLevels = nMip;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if (vmaCreateImage (ctx->allocator, &ici, &aci, &tex->img.image, &tex->img.alloc, nullptr) != VK_SUCCESS) {
		delete tex;
		return nullptr;
	}

	vkc::Buffer staging = ctx->CreateBuffer (dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
	memcpy (staging.mapped, data, dataSize);
	uint32_t nUpload = (uint32_t)mipOffsets.size();

	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		auto barrier = [&](uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
			VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
			VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
			b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
			b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
			b.oldLayout = from; b.newLayout = to;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = tex->img.image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1 };
			VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = &b;
			vkCmdPipelineBarrier2 (cmd, &dep);
		};

		barrier (0, nMip, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

		std::vector<VkBufferImageCopy> regions;
		for (uint32_t i = 0; i < nUpload; i++) {
			VkBufferImageCopy r{};
			r.bufferOffset = mipOffsets[i];
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
			r.imageExtent = { std::max (1u, w >> i), std::max (1u, h >> i), 1 };
			regions.push_back (r);
		}
		vkCmdCopyBufferToImage (cmd, staging.buffer, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			(uint32_t)regions.size(), regions.data());

		// generate remaining mip levels by successive blits
		for (uint32_t i = nUpload; i < nMip; i++) {
			barrier (i-1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			VkImageBlit blit{};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i-1, 0, 1 };
			blit.srcOffsets[1] = { (int32_t)std::max (1u, w >> (i-1)), (int32_t)std::max (1u, h >> (i-1)), 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
			blit.dstOffsets[1] = { (int32_t)std::max (1u, w >> i), (int32_t)std::max (1u, h >> i), 1 };
			vkCmdBlitImage (cmd, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
			barrier (i-1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		}
		uint32_t first = (nUpload < nMip ? nMip - 1 : 0);
		barrier (first, nMip - first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	ctx->DestroyBuffer (staging);

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = tex->img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, nMip, 0, 1 };
	if (swizzle) vci.components = *swizzle;
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &tex->img.view));
	tex->set = AllocSet (tex->img.view);
	return tex;
}
