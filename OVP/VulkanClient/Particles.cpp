// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Particles.h"
#include "GfxSettings.h"
#include "Scene.h"
#include "VulkanClient.h"
#include "VkTexture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "particle_emit.comp.h"
#include "particle_update.comp.h"
#include "particle_update_rt.comp.h"
#include "particle_light.comp.h"
#include "particle_light_rt.comp.h"
#include "particle_shadow_splat.comp.h"
#include "particle_shadow_sum.comp.h"
#include "flame_splat.comp.h"
#include "flame_trans.comp.h"
#include "particle_sortkeys.comp.h"
#include "particle_sort.comp.h"
#include "particle_sort_local.comp.h"
#include "smoke_splat.comp.h"
#include "smoke_resolve.comp.h"
#include "smoke_march.frag.h"
#include "smoke_march.comp.h"
#include "smoke_thick.frag.h"
#include "smoke_blur.comp.h"
#include "smoke_screen.frag.h"
#include "post.vert.h"
#include "gpuparticle.vert.h"
#include "gpuparticle.frag.h"

namespace {
	constexpr size_t PARTICLE_SIZE = 64; // (Particle in particles.glsl)
	constexpr double NOISE_SCALE = 0.08; // turbulence noise coordinates per metre (eddies of about 12 m)
	constexpr double MARCH_NOISE_SCALE = 0.016; // smoke volume erosion noise per metre (smoke_march.comp: SmokeNoise, a tile of 62 m, octaves 1:3)
	constexpr double MARCH_NOISE_RISE = 0.02;  // its drift upwards [noise units/s]
	// smoke shadows: the grid (particle_common.glsl) around the camera
	constexpr uint32_t SHADOW_NX = 192, SHADOW_NZ = 64, SHADOW_CASCADES = 3; // (cascades stacked along z)
	constexpr uint32_t SHADOW_TILE_COUNT = 2048; // (per frame: 3 x 24 x 24 tiles of 8 x 8 columns)
	constexpr double SHADOW_HALF_WIDTH = 1200.0, SHADOW_HALF_DEPTH = 1500.0; // [m]
	constexpr double SMOKE_EXTINCTION = 0.2; // optical depth (towards the sun) per unit of particle opacity
	constexpr uint32_t SORT_TILE = 512; // keys sorted in shared memory (particle_sort_local.comp)
	// smoke volume (smokevol.glsl)
	constexpr uint32_t VOL_X = 240, VOL_Y = 135, VOL_Z = 128;
	constexpr uint32_t VOLM_X = 120, VOLM_Y = 68, VOLM_Z = 64;  // half-resolution grid
	constexpr uint32_t VOLC_X = 60, VOLC_Y = 34, VOLC_Z = 32;   // quarter-resolution grid
	constexpr uint32_t VOL_TILES_X = 30, VOL_TILES_Y = 17, VOL_TILE_COUNT = 512; // column tiles (8 x 8)
	constexpr double VOL_REACH = 20000.0; // [m] (smokevol.glsl VOL_FAR)
}

// ======================================================================
// ParticleSystem

namespace {
	// the smoke volume's erosion noise: tiling gradient noise (quintic fade:
	// no creases), two octaves of 4 and 8 cells across, at N^3 texels (8 to
	// 16 texels a cell: trilinear filtering stays smooth), remapped to a
	// mean of 0.476 and a spread of 0.12 (as the Worley noise it replaces,
	// whose cell ridges and creases showed as straight lines in thin smoke)
	std::vector<uint8_t> SmokeNoise (int N)
	{
		auto hash = [](int x, int y, int z, uint32_t seed) {
			uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
			h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
			return h;
		};
		auto grad = [&](int x, int y, int z, double fx, double fy, double fz, uint32_t seed) {
			uint32_t h = hash (x, y, z, seed);
			double gx = (h & 0x3ff) / 511.5 - 1.0, gy = ((h >> 10) & 0x3ff) / 511.5 - 1.0, gz = ((h >> 20) & 0x3ff) / 511.5 - 1.0;
			double l = std::sqrt (gx * gx + gy * gy + gz * gz) + 1e-9;
			return (gx * fx + gy * fy + gz * fz) / l;
		};
		auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
		auto perlin = [&](double px, double py, double pz, int P, uint32_t seed) {
			double qx = px * P, qy = py * P, qz = pz * P;
			int cx = (int)std::floor (qx), cy = (int)std::floor (qy), cz = (int)std::floor (qz);
			double fx = qx - cx, fy = qy - cy, fz = qz - cz;
			double u = fade (fx), v = fade (fy), w = fade (fz), n[8];
			for (int k = 0; k < 8; k++) {
				int ox = k & 1, oy = (k >> 1) & 1, oz = k >> 2;
				n[k] = grad (((cx + ox) % P + P) % P, ((cy + oy) % P + P) % P, ((cz + oz) % P + P) % P, fx - ox, fy - oy, fz - oz, seed);
			}
			auto l = [](double a, double b, double t) { return a + (b - a) * t; };
			return l (l (l (n[0], n[1], u), l (n[2], n[3], u), v), l (l (n[4], n[5], u), l (n[6], n[7], u), v), w);
		};
		// (each octave the sum of four, their lattices offset by half cells: a
		// gradient noise's variance is nil at its lattice's points, largest
		// between them - through the erosion's threshold, integrated along the
		// view rays, a mean that repeats with the cells, seen as a fine
		// cross-hatch over the smoke)
		const double OFS[4][3] = { { 0, 0, 0 }, { 0.5, 0.5, 0 }, { 0.5, 0, 0.5 }, { 0, 0.5, 0.5 } };
		auto octave = [&](double px, double py, double pz, int P, uint32_t seed) {
			double v = 0.0;
			for (int k = 0; k < 4; k++) v += perlin (px + OFS[k][0] / P, py + OFS[k][1] / P, pz + OFS[k][2] / P, P, seed + k);
			return v;
		};
		std::vector<double> f ((size_t)N * N * N);
		double sum = 0.0, sum2 = 0.0;
		for (int z = 0; z < N; z++)
			for (int y = 0; y < N; y++)
				for (int x = 0; x < N; x++) {
					double px = (x + 0.5) / N, py = (y + 0.5) / N, pz = (z + 0.5) / N;
					double v = octave (px, py, pz, 4, 0) + 0.5 * octave (px, py, pz, 8, 4);
					f[((size_t)z * N + y) * N + x] = v, sum += v, sum2 += v * v;
				}
		double n = (double)f.size (), mean = sum / n, sd = std::sqrt (std::max (sum2 / n - mean * mean, 1e-12));
		std::vector<uint8_t> out (f.size ());
		for (size_t k = 0; k < f.size (); k++)
			out[k] = (uint8_t)std::lround (std::clamp (0.476 + 0.12 * (f[k] - mean) / sd, 0.0, 1.0) * 255.0);
		return out;
	}
}

struct ParticleSystem::GpuEmission { // particles.glsl
	float p0[4], p1[4], vel[4], dir[4], shape[4];
	uint32_t slab[4], misc[4];
};

struct ParticleSystem::UpdateParams { // particle_common.glsl
	float camShift[4], bodyPos[4], bodyVel[4], omega[4], noiseOrg[4];
	float lightX[4], lightY[4], lightZ[4], ground[4], trailNoise[4];
	float noiseRot[3][4], vol[4];
	float atm[4]; // the air of the planet nearby: density at the mean radius, scale height (particle_common.glsl AirShare)
};


bool ParticleSystem::Init (vkc::Context *c, TextureManager *tm, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
	VkDescriptorSetLayout texLayout, VkImageView shapeNoise, VkImageView detailNoise)
{
	ctx = c, texmgr = tm;
	static_assert (sizeof(UpdateParams) <= 256 && sizeof(UpdateParams) <= sizeof(lastUp), "push constants");
	{
		// smoke puffs: clusters of soft blobs (cauliflower-like billows) as a
		// height field: normal (rg, sprite axes), thickness (b) and coverage
		// (a), so that they can be lit and eroded (gpuparticle.frag)
		const int N = 256, Q = N / 2;
		std::vector<uint8_t> px (N * N * 4);
		std::mt19937 rng (4242);
		std::uniform_real_distribution<double> uni (0.0, 1.0);
		auto hash = [](int x, int y, int o) {
			uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ (uint32_t)(o * 83492791);
			h ^= h >> 13; h *= 0x5bd1e995; h ^= h >> 15;
			return (h & 0xffff) / 65535.0;
		};
		auto vnoise = [&](double x, double y, int o) { // value noise, tiling not needed
			int ix = (int)std::floor (x), iy = (int)std::floor (y);
			double fx = x - ix, fy = y - iy;
			fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
			double a = hash (ix, iy, o), b = hash (ix + 1, iy, o), c2 = hash (ix, iy + 1, o), d = hash (ix + 1, iy + 1, o);
			return (a + (b - a) * fx) + ((c2 + (d - c2) * fx) - (a + (b - a) * fx)) * fy;
		};
		for (int q = 0; q < 4; q++) {
			struct Blob { double x, y, r; };
			std::vector<Blob> blobs{ { 0.0, 0.0, 0.28 } };
			for (int k = 0; k < 16; k++) {
				double ang = uni (rng) * 6.2831853, d = 0.15 + 0.45 * uni (rng);
				double r = 0.08 + 0.17 * uni (rng) * (1.0 - 0.6 * d);
				blobs.push_back ({ d * cos (ang), d * sin (ang), r });
			}
			std::vector<double> H ((size_t)Q * Q), A ((size_t)Q * Q);
			for (int y = 0; y < Q; y++)
				for (int x = 0; x < Q; x++) {
					double u = (x + 0.5) / Q * 2.0 - 1.0, v = (y + 0.5) / Q * 2.0 - 1.0;
					// height: the union of spherical billows, roughened
					double h = 0.0;
					for (auto &b : blobs) {
						double dx = u - b.x, dy = v - b.y, r2 = b.r * b.r - dx * dx - dy * dy;
						if (r2 > 0.0) h = std::max (h, std::sqrt (r2) + 0.3 * b.r);
					}
					double n = 0.0, amp = 0.5, f = 6.0;
					for (int o = 0; o < 4; o++, amp *= 0.5, f *= 2.0) n += amp * vnoise (u * f + q * 17.0, v * f, o);
					h *= 0.8 + 0.4 * n;
					double edge = std::clamp ((1.0 - std::sqrt (u * u + v * v)) * 5.0, 0.0, 1.0);
					H[(size_t)y * Q + x] = h * edge;
					A[(size_t)y * Q + x] = std::clamp (h * 3.0, 0.0, 1.0) * edge;
				}
			for (int y = 0; y < Q; y++)
				for (int x = 0; x < Q; x++) {
					auto at = [&](int i, int j) { return H[(size_t)std::clamp (j, 0, Q - 1) * Q + std::clamp (i, 0, Q - 1)]; };
					double dhdu = (at (x + 1, y) - at (x - 1, y)) * Q / 4.0, dhdv = (at (x, y + 1) - at (x, y - 1)) * Q / 4.0;
					double nx = -dhdu, ny = -dhdv, nz = 1.0, l = std::sqrt (nx * nx + ny * ny + nz * nz);
					uint8_t *p = &px[((y + (q >> 1) * Q) * N + x + (q & 1) * Q) * 4];
					p[0] = (uint8_t)std::lround ((nx / l * 0.5 + 0.5) * 255.0);
					p[1] = (uint8_t)std::lround ((ny / l * 0.5 + 0.5) * 255.0);
					p[2] = (uint8_t)std::lround (std::clamp (H[(size_t)y * Q + x] * 1.8, 0.0, 1.0) * 255.0);
					p[3] = (uint8_t)std::lround (A[(size_t)y * Q + x] * 255.0);
				}
		}
		smokeTex = texmgr->CreateRGBALinear (N, N, px.data());
	}
	shapeView = shapeNoise, detailView = detailNoise;
	enabled = vkc::Gfx ().particles;
	density = std::clamp (vkc::Gfx ().particleDensity, 1.0f, 16.0f);

	pool = ctx->CreateBuffer (CAPACITY * PARTICLE_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	for (auto &b : emitBuf) b = ctx->CreateBuffer (MAX_EMISSIONS * sizeof(GpuEmission), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	VkBuffer pb = pool.buffer;
	ctx->ImmediateSubmit ([pb](VkCommandBuffer cmd) {
		vkCmdFillBuffer (cmd, pb, 0, VK_WHOLE_SIZE, 0); // (lifetime 0: no particles)
	});
	// draw order: sorted slots, keys, the ring of each block (per frame)
	sortVals = ctx->CreateBuffer (CAPACITY * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
	sortKeys = ctx->CreateBuffer (CAPACITY * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	for (auto &b : blockListBuf) b = ctx->CreateBuffer ((CAPACITY / BLOCK) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	for (auto &b : drawArgs)
		b = ctx->CreateBuffer (4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
	for (auto &b : drawRead) {
		b = ctx->CreateReadbackBuffer (4 * sizeof(uint32_t));
		memset (b.mapped, 0, 4 * sizeof(uint32_t));
	}
	for (auto &b : ringInfoBuf) {
		b = ctx->CreateBuffer ((CAPACITY / BLOCK) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		memset (b.mapped, 0, (CAPACITY / BLOCK) * sizeof(uint32_t));
	}
	if (const char *e = getenv ("ORBITER_VK_PARTICLE_STATS")) stats.every = std::max (atof (e), 0.0);
	if (const char *e = getenv ("ORBITER_VK_PARTICLE_RINGLOG")) stats.ringLog = atoi (e) != 0; // (each ring allocated, with the stats)
	shadowOcc = ctx->CreateBuffer (2 * SHADOW_TILE_COUNT * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	{
		VkBuffer ob = shadowOcc.buffer;
		ctx->ImmediateSubmit ([ob](VkCommandBuffer cmd) { vkCmdFillBuffer (cmd, ob, 0, VK_WHOLE_SIZE, 0); });
	}
	for (auto &b : windBuf) {
		// (+ nearVol and the counts, + the counts by class, + the ground jets' count and theirs)
		const size_t n = (WIND_LAYERS + 5 + 1 + 3 * MAX_GROUND_JETS + 4 + FLAME_POINTS + 2 + 3 + 1) * 4; // (+ the flames' dust grid: FlameBox; telemetry; the carry; the camera's height)
		b = ctx->CreateBuffer (n * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		memset (b.mapped, 0, n * sizeof(float));
	}

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &noiseSampler));
	{
		// the smoke volume's erosion noise (smoke_march.comp: no mips - smooth
		// over its cells of eight texels, its octaves faded by the pixels'
		// footprint instead; a mip's cells of a texel or two showed the
		// trilinear filter's lattice)
		const int N = SMOKE_NOISE_SIZE;
		const uint32_t levels = 1;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_3D, ici.format = VK_FORMAT_R8_UNORM, ici.extent = { (uint32_t)N, (uint32_t)N, (uint32_t)N };
		ici.mipLevels = levels, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &smokeNoise.image, &smokeNoise.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = smokeNoise.image, vci.viewType = VK_IMAGE_VIEW_TYPE_3D, vci.format = VK_FORMAT_R8_UNORM;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &smokeNoise.view));
		std::vector<std::vector<uint8_t>> lv{ SmokeNoise (N) };
		size_t total = 0;
		for (auto &v : lv) total += v.size ();
		vkc::Buffer stage = ctx->CreateBuffer (total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		std::vector<VkBufferImageCopy> copies;
		size_t ofs = 0;
		for (uint32_t l = 0; l < levels; l++) {
			memcpy ((uint8_t*)stage.mapped + ofs, lv[l].data (), lv[l].size ());
			uint32_t n = (uint32_t)(N >> l);
			VkBufferImageCopy c{};
			c.bufferOffset = ofs, c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, c.imageExtent = { n, n, n };
			copies.push_back (c);
			ofs += lv[l].size ();
		}
		vmaFlushAllocation (ctx->allocator, stage.alloc, 0, VK_WHOLE_SIZE);
		VkImage im = smokeNoise.image;
		VkBuffer sb = stage.buffer;
		ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
			VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
			b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = im, b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
			VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.imageMemoryBarrierCount = 1, dep.pImageMemoryBarriers = &b;
			vkCmdPipelineBarrier2 (cmd, &dep);
			vkCmdCopyBufferToImage (cmd, sb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size (), copies.data ());
			b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier2 (cmd, &dep);
		});
		ctx->DestroyBuffer (stage);
		VkSamplerCreateInfo msi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		msi.magFilter = msi.minFilter = VK_FILTER_LINEAR, msi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		msi.addressModeU = msi.addressModeV = msi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		msi.maxLod = VK_LOD_CLAMP_NONE;
		VK_CHECK(vkCreateSampler (ctx->device, &msi, nullptr, &smokeNoiseSampler));
	}
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &clampSampler));
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &volSampler));

	// smoke shadow grid: smoke per cell, optical depth towards the sun
	auto image3D = [this](VkFormat fmt, VkImageUsageFlags usage) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_3D;
		ici.format = fmt;
		ici.extent = { SHADOW_NX, SHADOW_NX, SHADOW_NZ * SHADOW_CASCADES };
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_3D, vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	shadowAccum = image3D (VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	// smoke volume: accumulation (extinction, light), the resolved volume, columns
	auto image = [this](VkImageType type, VkFormat fmt, VkExtent3D e, VkImageUsageFlags usage) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = type, ici.format = fmt, ici.extent = e;
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	// (the volume itself: AllocVolume, when smoke is near; until then the
	// sprites' fog is none)
	volDummy = image (VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, { 1, 1, 1 }, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	{
		VkImage im = volDummy.image;
		ctx->ImmediateSubmit ([im](VkCommandBuffer cmd) {
			VkClearColorValue none{ { 0.0f, 0.0f, 0.0f, 1.0f } };
			VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im, VK_IMAGE_LAYOUT_GENERAL, &none, 1, &r);
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
	}
	shadowDepth = image3D (VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	// the dust around a vessel's flames (FlameBox): its extinction, the
	// transmittance towards them (all clear: 1)
	flameAccum = image (VK_IMAGE_TYPE_3D, VK_FORMAT_R32_UINT, { FLAME_NX, FLAME_NX, FLAME_NZ }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	flameOcc = image (VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, { FLAME_NX, FLAME_NX, FLAME_NZ }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	{
		VkImage fa = flameAccum.image, fo = flameOcc.image;
		ctx->ImmediateSubmit ([fa, fo](VkCommandBuffer cmd) {
			VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkClearColorValue zero{}, one{ { 1.0f, 1.0f, 1.0f, 1.0f } };
			for (VkImage im : { fa, fo })
				vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, fa, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
			vkCmdClearColorImage (cmd, fo, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &r);
			vkc::ImageBarrier (cmd, fo, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
	}
	VkImage gi[2] = { shadowAccum.image, shadowDepth.image };
	ctx->ImmediateSubmit ([gi](VkCommandBuffer cmd) {
		VkClearColorValue zero{};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (VkImage im : gi) {
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
		}
	});

	// pool, emissions, noise volumes, prepass, shadow grid
	const VkShaderStageFlags CS = VK_SHADER_STAGE_COMPUTE_BIT;
	// (8-10: sorting for drawing far to near: values, keys, ring of each block)
	const VkShaderStageFlags VS = VK_SHADER_STAGE_VERTEX_BIT, FS = VK_SHADER_STAGE_FRAGMENT_BIT;
	const VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	std::vector<VkDescriptorSetLayoutBinding> b = {
		{ 0, SB, 1, CS | VS, nullptr }, { 1, SB, 1, CS, nullptr },
		{ 2, CI, 1, CS, nullptr }, { 3, CI, 1, CS | FS, nullptr }, { 4, CI, 1, FS | CS, nullptr }, // (noise, prepass)
		{ 5, SI, 1, CS, nullptr }, { 6, SI, 1, CS, nullptr }, { 7, CI, 1, CS, nullptr }, // (smoke shadow grid)
		// (8-10: the sprites' sort: values, keys; the allocated blocks)
		{ 8, SB, 1, CS | VS, nullptr }, { 9, SB, 1, CS, nullptr }, { 10, SB, 1, CS, nullptr },
		// (11-18, 24-43: smoke volume: AllocVolume; 19: screen-space smoke)
		{ 11, SI, 8, CS, nullptr }, // (the fine accumulators: smokevol.glsl VA_*)
		{ 15, SI, 1, CS, nullptr }, { 16, SI, 1, CS, nullptr }, { 17, CI, 1, FS | CS, nullptr }, { 18, CI, 1, FS | CS, nullptr },
		{ 19, CI, 1, FS, nullptr },
		{ 20, SB, 1, CS | VS, nullptr }, // (the wind's layers; streaks relative to the air)
		// (21-23: the sprites' draw: its arguments, how each ring is drawn, the smoke in front)
		{ 21, SB, 1, CS, nullptr }, { 22, SB, 1, VS, nullptr }, { 23, CI, 1, FS, nullptr },
		{ 24, SI, 1, CS, nullptr }, { 28, SB, 1, CS, nullptr }, { 26, SI, 8, CS, nullptr }, { 29, SI, 8, CS, nullptr }, // (mid, coarse accumulators)
		{ 31, SI, 2, CS, nullptr }, { 32, SI, 2, CS, nullptr }, { 33, CI, 2, CS, nullptr }, { 34, CI, 2, CS, nullptr },
		{ 35, SI, 1, CS, nullptr }, { 36, CI, 1, CS, nullptr }, { 37, SB, 1, CS, nullptr },
		{ 38, SI, 1, CS, nullptr }, { 39, SI, 1, CS, nullptr }, { 40, SI, 1, CS, nullptr },
		{ 41, CI, 1, FS, nullptr }, { 42, CI, 1, FS, nullptr }, { 43, CI, 1, FS, nullptr },
		{ 44, CI, 1, CS, nullptr }, // (the smoke volume's erosion noise)
		{ 45, SI, 1, CS, nullptr }, { 46, SI, 1, CS, nullptr }, { 47, CI, 1, CS, nullptr }, // (the local lights' occlusion: the fine grid's extinction)
		{ 48, CI, 1, FS, nullptr }, // (the crash fireballs' occlusion: FireballRenderer)
		{ 49, CI, 1, CS, nullptr }, { 50, CI, 1, CS, nullptr }, // (the march's history: the other slot's near, far)
		{ 51, SI, 2, CS, nullptr }, { 52, CI, 2, CS, nullptr }, // (mid, coarse: engine smoke's speed)
		{ 53, CI, 1, FS, nullptr }, { 54, CI, 1, FS, nullptr }, // (the plumes' light and transmittance, their distance: PlumeRenderer::LayerViews)
		{ 55, SI, 1, CS, nullptr }, { 56, SI, 1, CS, nullptr } }; // (the dust around a vessel's flames: FlameBox)
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = (uint32_t)b.size (), lci.pBindings = b.data ();
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	{
		// the rings' textures
		VkDescriptorSetLayoutBinding tb{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEX_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		tci.bindingCount = 1, tci.pBindings = &tb;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &tci, nullptr, &texSetLayout));
	}
	VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12 * vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (22 + TEX_SLOTS) * vkc::FRAMES_IN_FLIGHT + 2 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 50 * vkc::FRAMES_IN_FLIGHT + 2 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2 * vkc::FRAMES_IN_FLIGHT + 2, pci.poolSizeCount = 3, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &dpool));
	VkDescriptorSetLayout ls[vkc::FRAMES_IN_FLIGHT];
	for (auto &l : ls) l = setLayout;
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = dpool, dai.descriptorSetCount = vkc::FRAMES_IN_FLIGHT, dai.pSetLayouts = ls;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		VkDescriptorBufferInfo bi[2] = { { pool.buffer, 0, VK_WHOLE_SIZE }, { emitBuf[i].buffer, 0, VK_WHOLE_SIZE } };
		VkDescriptorImageInfo ii[2] = { { noiseSampler, shapeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ noiseSampler, detailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
		VkDescriptorImageInfo gii[3] = { { VK_NULL_HANDLE, shadowAccum.view, VK_IMAGE_LAYOUT_GENERAL },
			{ VK_NULL_HANDLE, shadowDepth.view, VK_IMAGE_LAYOUT_GENERAL }, { volSampler, shadowDepth.view, VK_IMAGE_LAYOUT_GENERAL } }; // (clamped at the grid's faces)
		VkWriteDescriptorSet wr[7]{};
		for (uint32_t k = 0; k < 7; k++) {
			wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			wr[k].dstSet = sets[i], wr[k].descriptorCount = 1;
			wr[k].dstBinding = k < 4 ? k : k + 1;
			if (k < 2) wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[k].pBufferInfo = &bi[k];
			else if (k < 4) wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[k].pImageInfo = &ii[k - 2];
			else {
				wr[k].descriptorType = k < 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				wr[k].pImageInfo = &gii[k - 4];
			}
		}
		vkUpdateDescriptorSets (ctx->device, 7, wr, 0, nullptr);
		VkDescriptorBufferInfo sbi[3] = { { sortVals.buffer, 0, VK_WHOLE_SIZE }, { sortKeys.buffer, 0, VK_WHOLE_SIZE },
			{ blockListBuf[i].buffer, 0, VK_WHOLE_SIZE } };
		VkWriteDescriptorSet sw[3]{};
		for (uint32_t k = 0; k < 3; k++) {
			sw[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			sw[k].dstSet = sets[i], sw[k].dstBinding = 8 + k, sw[k].descriptorCount = 1;
			sw[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sw[k].pBufferInfo = &sbi[k];
		}
		vkUpdateDescriptorSets (ctx->device, 3, sw, 0, nullptr);
		VkDescriptorBufferInfo wbi[2] = { { windBuf[i].buffer, 0, VK_WHOLE_SIZE }, { shadowOcc.buffer, 0, VK_WHOLE_SIZE } };
		VkWriteDescriptorSet ww[2]{};
		for (uint32_t k = 0; k < 2; k++) {
			ww[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			ww[k].dstSet = sets[i], ww[k].dstBinding = k ? 28 : 20, ww[k].descriptorCount = 1;
			ww[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ww[k].pBufferInfo = &wbi[k];
		}
		vkUpdateDescriptorSets (ctx->device, 2, ww, 0, nullptr);
		VkDescriptorImageInfo ni{ smokeNoiseSampler, smokeNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet nw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		nw.dstSet = sets[i], nw.dstBinding = 44, nw.descriptorCount = 1;
		nw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nw.pImageInfo = &ni;
		vkUpdateDescriptorSets (ctx->device, 1, &nw, 0, nullptr);
		VkDescriptorImageInfo fii[2] = { { VK_NULL_HANDLE, flameAccum.view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, flameOcc.view, VK_IMAGE_LAYOUT_GENERAL } };
		VkWriteDescriptorSet fw[2]{};
		for (uint32_t k = 0; k < 2; k++) {
			fw[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			fw[k].dstSet = sets[i], fw[k].dstBinding = 55 + k, fw[k].descriptorCount = 1;
			fw[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, fw[k].pImageInfo = &fii[k];
		}
		vkUpdateDescriptorSets (ctx->device, 2, fw, 0, nullptr);
		VkDescriptorBufferInfo dbi[2] = { { drawArgs[i].buffer, 0, VK_WHOLE_SIZE }, { ringInfoBuf[i].buffer, 0, VK_WHOLE_SIZE } };
		VkDescriptorImageInfo dii{ volSampler, volDummy.view, VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet dw[3]{};
		for (uint32_t k = 0; k < 3; k++) {
			dw[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			dw[k].dstSet = sets[i], dw[k].dstBinding = 21 + k, dw[k].descriptorCount = 1;
			if (k < 2) dw[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, dw[k].pBufferInfo = &dbi[k];
			else dw[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, dw[k].pImageInfo = &dii;
		}
		vkUpdateDescriptorSets (ctx->device, 3, dw, 0, nullptr);
	}
	{
		// the texture table: the smoke texture in every slot to start with
		VkSamplerCreateInfo tsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		tsi.magFilter = tsi.minFilter = VK_FILTER_LINEAR;
		tsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		tsi.addressModeU = tsi.addressModeV = tsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		tsi.maxLod = VK_LOD_CLAMP_NONE;
		VK_CHECK(vkCreateSampler (ctx->device, &tsi, nullptr, &texSampler));
		VkDescriptorSetLayout tl[vkc::FRAMES_IN_FLIGHT];
		for (auto &l : tl) l = texSetLayout;
		VkDescriptorSetAllocateInfo tai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		tai.descriptorPool = dpool, tai.descriptorSetCount = vkc::FRAMES_IN_FLIGHT, tai.pSetLayouts = tl;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &tai, texSets));
		std::vector<VkDescriptorImageInfo> ti (TEX_SLOTS, { texSampler, smokeTex->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
		for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
			VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w.dstSet = texSets[i], w.dstBinding = 0, w.descriptorCount = TEX_SLOTS;
			w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = ti.data ();
			vkUpdateDescriptorSets (ctx->device, 1, &w, 0, nullptr);
			for (auto &t : setTex[i]) t = smokeTex;
		}
		slotTex[0] = smokeTex; // (always)
	}

	auto layoutOf = [this](std::initializer_list<VkDescriptorSetLayout> l, VkShaderStageFlags stages, uint32_t pcSize) {
		std::vector<VkDescriptorSetLayout> v (l);
		VkPushConstantRange pcr{ stages, 0, pcSize };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = (uint32_t)v.size(), plci.pSetLayouts = v.data();
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VkPipelineLayout pl;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &pl));
		return pl;
	};
	auto compute = [this](const unsigned char *code, size_t size, VkPipelineLayout l) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = l;
		VkPipeline p;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &p));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		return p;
	};
	emitLayout = layoutOf ({ setLayout }, CS, sizeof(uint32_t));
	updateLayout = layoutOf ({ setLayout, frameLayout, atmLayout }, CS, sizeof(UpdateParams));
	emitPipe = compute (spv_particle_emit_comp, spv_particle_emit_comp_size, emitLayout);
	updatePipe = compute (spv_particle_update_comp, spv_particle_update_comp_size, updateLayout);
	lightPipe = compute (spv_particle_light_comp, spv_particle_light_comp_size, updateLayout);
	splatPipe = compute (spv_particle_shadow_splat_comp, spv_particle_shadow_splat_comp_size, updateLayout);
	sumPipe = compute (spv_particle_shadow_sum_comp, spv_particle_shadow_sum_comp_size, updateLayout);
	flameSplatPipe = compute (spv_flame_splat_comp, spv_flame_splat_comp_size, updateLayout);
	flameTransPipe = compute (spv_flame_trans_comp, spv_flame_trans_comp_size, updateLayout);
	sortKeysPipe = compute (spv_particle_sortkeys_comp, spv_particle_sortkeys_comp_size, updateLayout);
	sortLayout = layoutOf ({ setLayout }, CS, 4 * sizeof(uint32_t));
	sortPipe = compute (spv_particle_sort_comp, spv_particle_sort_comp_size, sortLayout);
	sortLocalPipe = compute (spv_particle_sort_local_comp, spv_particle_sort_local_comp_size, sortLayout);
	volSplatPipe = compute (spv_smoke_splat_comp, spv_smoke_splat_comp_size, updateLayout);
	volResolvePipe = compute (spv_smoke_resolve_comp, spv_smoke_resolve_comp_size, updateLayout);
	marchPipe = compute (spv_smoke_march_comp, spv_smoke_march_comp_size, updateLayout);

	// drawing: quads (triangle strips) from the pool, premultiplied or additive,
	// with the reactive mask
	drawLayout = layoutOf ({ frameLayout, setLayout, texSetLayout }, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80);
	thickLayout = layoutOf ({ frameLayout, setLayout }, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80);
	VkShaderModule vs = ctx->CreateShaderModule (spv_gpuparticle_vert, spv_gpuparticle_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_gpuparticle_frag, spv_gpuparticle_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1, vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = ctx->sceneSamples;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
	VkFormat formats[2] = { ctx->sceneFormat, VK_FORMAT_R8_UNORM }; // (Scene::REACTIVE_FORMAT)
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 2, rci.pColorAttachmentFormats = formats;
	rci.depthAttachmentFormat = ctx->depthFormat;
	{
		// (premultiplied; emissive particles write alpha 0: added)
		VkPipelineColorBlendAttachmentState cb[2]{};
		cb[0].colorWriteMask = 0xF;
		cb[0].blendEnable = VK_TRUE;
		cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cb[0].colorBlendOp = VK_BLEND_OP_ADD;
		cb[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		cb[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[0].alphaBlendOp = VK_BLEND_OP_ADD;
		cb[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
		cb[1].blendEnable = VK_TRUE;
		cb[1].srcColorBlendFactor = cb[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[1].colorBlendOp = VK_BLEND_OP_MAX;
		cb[1].srcAlphaBlendFactor = cb[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[1].alphaBlendOp = VK_BLEND_OP_MAX;
		VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cbs.attachmentCount = 2, cbs.pAttachments = cb;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = stages;
		gpi.pVertexInputState = &vi;
		gpi.pInputAssemblyState = &ia;
		gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs;
		gpi.pMultisampleState = &ms;
		gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cbs;
		gpi.pDynamicState = &dy;
		gpi.layout = drawLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &drawBlend));
	}
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);

	// smoke volume: the march over the scene (full screen; scene with the reactive mask)
	ApplySettings ();
	volMarchLayout = layoutOf ({ frameLayout, setLayout }, VK_SHADER_STAGE_FRAGMENT_BIT, 16);
	{
		VkShaderModule mvs = ctx->CreateShaderModule (spv_post_vert, spv_post_vert_size);
		VkShaderModule mfs = ctx->CreateShaderModule (spv_smoke_march_frag, spv_smoke_march_frag_size);
		VkPipelineShaderStageCreateInfo ms2[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mvs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mfs, "main" } };
		VkPipelineInputAssemblyStateCreateInfo tri{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		tri.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineDepthStencilStateCreateInfo nods{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState cb[2]{};
		cb[0].colorWriteMask = 0xF;
		cb[0].blendEnable = VK_TRUE;
		cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cb[0].colorBlendOp = VK_BLEND_OP_ADD;
		cb[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		cb[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[0].alphaBlendOp = VK_BLEND_OP_ADD;
		cb[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
		cb[1].blendEnable = VK_TRUE;
		cb[1].srcColorBlendFactor = cb[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[1].colorBlendOp = VK_BLEND_OP_MAX;
		cb[1].srcAlphaBlendFactor = cb[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[1].alphaBlendOp = VK_BLEND_OP_MAX;
		VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cbs.attachmentCount = 2, cbs.pAttachments = cb;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = ms2;
		gpi.pVertexInputState = &vi;
		gpi.pInputAssemblyState = &tri;
		gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs;
		gpi.pMultisampleState = &ms;
		gpi.pDepthStencilState = &nods;
		gpi.pColorBlendState = &cbs;
		gpi.pDynamicState = &dy;
		gpi.layout = volMarchLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &volMarchPipe));
		// screen-space smoke: its composite (the same state)
		VkShaderModule sfs = ctx->CreateShaderModule (spv_smoke_screen_frag, spv_smoke_screen_frag_size);
		ms2[1].module = sfs;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &screenCompPipe));
		vkDestroyShaderModule (ctx->device, sfs, nullptr);
		vkDestroyShaderModule (ctx->device, mvs, nullptr);
		vkDestroyShaderModule (ctx->device, mfs, nullptr);
	}
	{
		// screen-space smoke: thickness of the volumetric particles (half
		// resolution, additive, no depth), and the blur
		VkShaderModule tvs = ctx->CreateShaderModule (spv_gpuparticle_vert, spv_gpuparticle_vert_size);
		VkShaderModule tfs = ctx->CreateShaderModule (spv_smoke_thick_frag, spv_smoke_thick_frag_size);
		VkPipelineShaderStageCreateInfo ts[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, tvs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, tfs, "main" } };
		VkPipelineColorBlendAttachmentState ab{};
		ab.colorWriteMask = 0xF;
		ab.blendEnable = VK_TRUE;
		ab.srcColorBlendFactor = ab.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		ab.srcAlphaBlendFactor = ab.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		ab.colorBlendOp = ab.alphaBlendOp = VK_BLEND_OP_ADD;
		VkPipelineColorBlendStateCreateInfo abs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		abs.attachmentCount = 1, abs.pAttachments = &ab;
		VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo nods{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkFormat tf = VK_FORMAT_R16G16B16A16_SFLOAT;
		VkPipelineRenderingCreateInfo trci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		trci.colorAttachmentCount = 1, trci.pColorAttachmentFormats = &tf;
		VkPipelineInputAssemblyStateCreateInfo strip{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		strip.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &trci;
		gpi.stageCount = 2, gpi.pStages = ts;
		gpi.pVertexInputState = &vi;
		gpi.pInputAssemblyState = &strip;
		gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs;
		gpi.pMultisampleState = &ms1;
		gpi.pDepthStencilState = &nods;
		gpi.pColorBlendState = &abs;
		gpi.pDynamicState = &dy;
		gpi.layout = thickLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &thickPipe));
		vkDestroyShaderModule (ctx->device, tvs, nullptr);
		vkDestroyShaderModule (ctx->device, tfs, nullptr);
		VkDescriptorSetLayoutBinding bb[2] = {
			{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo blci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		blci.bindingCount = 2, blci.pBindings = bb;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &blci, nullptr, &blurSetLayout));
		VkDescriptorSetLayout bl[2] = { blurSetLayout, blurSetLayout };
		VkDescriptorSetAllocateInfo bdai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		bdai.descriptorPool = dpool, bdai.descriptorSetCount = 2, bdai.pSetLayouts = bl;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &bdai, blurSets));
		blurLayout = layoutOf ({ blurSetLayout }, CS, 4 * sizeof(int32_t));
		blurPipe = compute (spv_smoke_blur_comp, spv_smoke_blur_comp_size, blurLayout);
	}
	return true;
}

void ParticleSystem::InitRT (VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex, VkDescriptorSetLayout atmLayout)
{
	VkDescriptorSetLayout l[5] = { setLayout, frameLayout, rtScene, rtTex, atmLayout };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpdateParams) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 5, plci.pSetLayouts = l;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &updateRTLayout));
	auto compute = [this](const unsigned char *code, size_t size, VkPipeline &pipe) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = updateRTLayout;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
	};
	compute (spv_particle_update_rt_comp, spv_particle_update_rt_comp_size, updateRTPipe);
	compute (spv_particle_light_rt_comp, spv_particle_light_rt_comp_size, lightRTPipe);
}

void ParticleSystem::Shutdown ()
{
	if (!ctx) return;
	ctx->WaitIdle ();
	FreeVolume (true);
	ReapVolume (true);
	for (VkPipeline p : { emitPipe, updatePipe, updateRTPipe, lightPipe, lightRTPipe, splatPipe, sumPipe, flameSplatPipe, flameTransPipe, sortKeysPipe, sortPipe, sortLocalPipe, drawBlend,
		volSplatPipe, volResolvePipe, volMarchPipe, marchPipe, thickPipe, blurPipe, screenCompPipe })
		if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	for (VkPipelineLayout l : { emitLayout, updateLayout, updateRTLayout, drawLayout, thickLayout, sortLayout, volMarchLayout, blurLayout })
		if (l) vkDestroyPipelineLayout (ctx->device, l, nullptr);
	vkDestroyDescriptorPool (ctx->device, dpool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, texSetLayout, nullptr);
	vkDestroySampler (ctx->device, texSampler, nullptr);
	for (uint32_t k = 1; k < TEX_SLOTS; k++) if (slotTex[k]) texmgr->Release (slotTex[k]);
	for (auto &b : drawArgs) ctx->DestroyBuffer (b);
	for (auto &b : drawRead) ctx->DestroyBuffer (b);
	for (auto &b : ringInfoBuf) ctx->DestroyBuffer (b);
	ctx->DestroyImage (volDummy);
	vkDestroySampler (ctx->device, noiseSampler, nullptr);
	vkDestroySampler (ctx->device, smokeNoiseSampler, nullptr);
	ctx->DestroyImage (smokeNoise);
	vkDestroySampler (ctx->device, clampSampler, nullptr);
	ctx->DestroyBuffer (pool);
	for (auto &b : emitBuf) ctx->DestroyBuffer (b);
	for (auto &t : thick) ctx->DestroyImage (t);
	if (blurSetLayout) vkDestroyDescriptorSetLayout (ctx->device, blurSetLayout, nullptr);
	if (volSampler) vkDestroySampler (ctx->device, volSampler, nullptr);
	ctx->DestroyBuffer (sortVals);
	ctx->DestroyBuffer (sortKeys);
	for (auto &b : blockListBuf) ctx->DestroyBuffer (b);
	for (auto &b : windBuf) ctx->DestroyBuffer (b);
	ctx->DestroyImage (shadowAccum);
	ctx->DestroyBuffer (shadowOcc);
	ctx->DestroyImage (shadowDepth);
	ctx->DestroyImage (flameAccum);
	ctx->DestroyImage (flameOcc);
	if (smokeTex) texmgr->Release (smokeTex);
	ctx = nullptr;
}

int ParticleSystem::Priority (Class c)
{
	switch (c) {
	case Class::Trail: return 0;
	case Class::EngineSmoke: case Class::Steam: case Class::Fire: case Class::FireballSmoke:
	case Class::WreckSmoke: case Class::PlasmaWake: case Class::Contrail: return 1;
	case Class::Dust: case Class::Spray: case Class::Legacy: return 2;
	default: return 3; // (sparks, fragments, puffs)
	}
}

ParticleSystem::Class ParticleSystem::Resolve (const Emission &e)
{
	if (e.cls != Class::Auto) return e.cls;
	if (e.trail || e.slowdown < 0.0) return Class::Trail;
	if (e.speck) return e.emissive ? Class::Spark : Class::Fragment;
	if (e.ballistic || (e.cold && e.tinted)) return Class::Dust;
	if (e.emissive && e.tinted) return Class::Fire;
	if (e.tinted && e.volumetric) return Class::EngineSmoke;
	return Class::Legacy;
}

uint32_t ParticleSystem::AllocRing (uint32_t want, uint32_t minWant, uint32_t &first, bool idleRelease)
{
	uint32_t got = slots.Allocate (want, minWant, first, idleRelease);
	if (!got) {
		stats.allocFails++, stats.failWant = std::max (stats.failWant, want);
		return 0;
	}
	if (got < want) stats.short_++;
	ringLook[first / BLOCK] = RingLook{};
	toClear.push_back ({ first, got });
	stats.allocs++;
	return got;
}

bool ParticleSystem::Allocate (uint32_t want, uint32_t &first, uint32_t &capacity)
{
	uint32_t got = AllocRing (want, std::max (BLOCK, want / 4), first, false);
	if (!got) {
		double now = oapiGetSysTime ();
		if (now > lastFailLog + 10.0) { // (at most a line in 10 s)
			oapiWriteLogV ("VulkanClient: particle pool full (%u particles wanted; %u of %u allocated)", want, slots.Allocated (), CAPACITY);
			lastFailLog = now;
		}
		return false;
	}
	capacity = got;
	return true;
}

void ParticleSystem::Free (uint32_t first, uint32_t capacity)
{
	// (particles left in the ring are cleared when it is allocated again)
	const ParticlePool::Ring *r = slots.RingAt (first);
	if (!r || r->capacity != capacity) return;
	slots.Free (first);
	stats.frees++;
}

bool ParticleSystem::Replace (ParticleRing &r, uint32_t cap, uint32_t minCap, double simt)
{
	uint32_t first;
	uint32_t got = AllocRing (cap, minCap, first, r.idleRelease);
	if (!got) return false;
	ParticlePool::Ring *old = slots.RingAt (r.first);
	double until = std::max (old ? old->lastEmit : r.lastEmit, r.lastEmit) + 1.5 * std::max (r.life, old ? old->life : 0.0) + 1.0;
	RingLook &l = ringLook[r.first / BLOCK];
	if (!l.tex) l.tex = smokeTex, l.emissive = false;
	slots.Retire (r.first, until, (uint64_t)(uintptr_t)l.tex | (l.emissive ? 1u : 0u));
	aliveUntil = std::max (aliveUntil, until);
	ringLook[first / BLOCK] = l;
	r.first = first, r.capacity = got, r.head = 0, r.id = slots.RingAt (first)->id;
	r.shortfall = std::min (1.0, got / std::max ((double)cap, 1.0));
	r.segStart = -1e30;
	ParticlePool::Ring *pr = slots.RingAt (first);
	pr->prio = r.prio, pr->lastEmit = simt, pr->life = r.life;
	return true;
}

bool ParticleSystem::Reserve (ParticleRing &r, double rate, double life, Class cls, double extra, VkTexture *tex, bool emissive, bool idleRelease)
{
	if (!enabled) return false;
	double simt = oapiGetSimTime ();
	if (r.sys != this) {
		r = ParticleRing{};
		r.sys = this;
	}
	if (r.id && !slots.Valid (r.first, r.id)) r.id = 0, r.capacity = 0, r.head = 0; // (released while idle)
	life = std::max (life, 0.01);
	r.prio = Priority (cls), r.idleRelease = idleRelease;
	r.life = std::max (r.life, life);
	double fill = slots.Fill ();
	double fp = ParticlePool::PressureFactor (fill, r.prio) * ParticlePool::LifeFactor (fill, r.prio);
	// The ring holds the emissions of a segment of time: short-lived
	// particles (1.5 lifetimes within it), a ring that wraps over the dead
	// ones; else, when it is full of live ones, the next segment (Emit), so
	// that the rings hold about the particles alive, whatever the history of
	// the rate
	double seg = ParticlePool::SegmentTime (life);
	r.chain = seg < 1.5 * life;
	double needNow = (std::max (rate, 0.0) * seg * 1.1 + std::max (extra, 0.0)) * fp;
	// (the need follows a rising rate at once, a falling one over the
	// segment's time)
	double dt = simt - r.needT;
	if (dt < 0.0 || dt > 1e5) r.need = 0.0;
	else r.need *= std::exp (-dt / seg);
	r.need = std::max (r.need, needNow);
	r.needT = simt;
	if (r.id) {
		// a wrapping ring follows the need (grown or shrunk: a new one);
		// segments do at the next segment
		uint32_t nc = r.chain ? 0u : ParticlePool::Resize (r.capacity, r.need, BLOCK);
		if (nc) {
			bool grow = nc > r.capacity; // (growing: at least larger than this one)
			uint32_t was = r.capacity;
			if (Replace (r, nc, grow ? r.capacity + BLOCK : nc, simt)) {
				stats.grows += grow ? 1 : 0, stats.shrinks += grow ? 0 : 1;
				if (stats.every > 0.0 && stats.ringLog)
					oapiWriteLogV ("VK particles: ring %u class %d rate %.0f/s life %.1f s: %s %u -> %u", r.first / BLOCK, (int)cls, rate, life,
						grow ? "grown" : "shrunk", was, r.capacity);
			}
		}
	} else {
		uint32_t want = (uint32_t)std::min (std::ceil (std::max (r.need, 1.0) * (r.chain ? 1.0 : 1.5) / BLOCK) * BLOCK, (double)CAPACITY);
		uint32_t first;
		uint32_t got = AllocRing (want, BLOCK, first, idleRelease);
		if (!got) {
			if (!r.failLogged) {
				oapiWriteLogV ("VulkanClient: particle pool full (an emitter of class %d wanted %u particles; %u of %u allocated)",
					(int)cls, want, slots.Allocated (), CAPACITY);
				r.failLogged = true;
			}
			return false;
		}
		r.first = first, r.capacity = got, r.head = 0, r.id = slots.RingAt (first)->id;
		r.shortfall = (double)got / want, r.segStart = -1e30;
		ParticlePool::Ring *pr = slots.RingAt (first);
		pr->prio = r.prio, pr->lastEmit = simt, pr->life = life;
		if (stats.every > 0.0 && stats.ringLog)
			oapiWriteLogV ("VK particles: ring %u class %d rate %.0f/s life %.1f s extra %.0f: %u of %u (%.0f %% full)",
				first / BLOCK, (int)cls, rate, life, extra, got, want, 100.0 * fill);
	}
	RingLook &l = ringLook[r.first / BLOCK];
	if (tex) l.tex = tex, l.emissive = emissive;
	else if (!l.tex) l.tex = smokeTex, l.emissive = false;
	// the fraction of the particles emitted: the pool's pressure, and the
	// ring's shortfall when it was allocated
	ParticlePool::Ring *pr = slots.RingAt (r.first);
	pr->thin = (float)(ParticlePool::PressureFactor (fill, r.prio) * r.shortfall);
	return true;
}

void ParticleSystem::AddGroundJet (const VECTOR3 &pos, const VECTOR3 &n, double speed, double r0, double reach, double thick)
{
	if (speed > 0.0 && reach > 0.0 && groundJets.size () < 64) groundJets.push_back ({ pos, n, speed, r0, reach, thick }); // (64: if no Simulate clears them)
}

void ParticleSystem::Emit (ParticleRing &r, const Emission &src, uint32_t n)
{
	Emission e = src; // (Queue thins and rescales it: not the caller's)
	if (!Valid (r) || !n) return;
	double simt = oapiGetSimTime ();
	bool used = r.segStart > -1e29; // (written into since allocated)
	if (used && (r.head + n > r.capacity || r.head == 0)) {
		// the ring wraps: over dead particles (the segment's first ones are
		// 1.5 lifetimes old), or the next segment, this one retired; (no
		// segment: over the oldest)
		bool alive = simt - r.segStart < 1.5 * std::max (r.life, e.lifetime);
		uint32_t cap = (uint32_t)std::ceil (std::max (r.need, 2.0 * n) / BLOCK) * BLOCK;
		if (r.chain && alive && Replace (r, cap, BLOCK, simt)) stats.segments++;
		else {
			if (alive) stats.overwrites++;
			r.segStart = simt;
		}
	}
	e.first = r.first, e.capacity = r.capacity, e.head = r.head, e.count = std::min (n, r.capacity);
	Queue (e);
	if (!e.count) return; // (thinned to none: the segment not begun; head 0 must not read as a wrap)
	if (r.segStart <= -1e29) r.segStart = simt;
	r.head = (r.head + e.count) % r.capacity;
	r.lastEmit = simt;
	r.life = std::max (r.life, e.lifetime);
}

void ParticleSystem::Release (ParticleRing &r)
{
	if (Valid (r)) {
		double simt = oapiGetSimTime ();
		const ParticlePool::Ring *pr = slots.RingAt (r.first);
		double until = std::max (pr->lastEmit, r.lastEmit) + 1.5 * std::max (pr->life, r.life) + 1.0;
		if (until > simt) Retire (r.first, r.capacity, until, nullptr, false);
		else Free (r.first, r.capacity);
	}
	r.id = 0, r.capacity = 0, r.sys = nullptr;
}

void ParticleSystem::Retire (uint32_t first, uint32_t capacity, double until, VkTexture *tex, bool emissive)
{
	const ParticlePool::Ring *r = slots.RingAt (first);
	if (!r || r->capacity != capacity) return;
	RingLook &l = ringLook[first / BLOCK];
	if (tex) l.tex = tex, l.emissive = emissive;
	else if (!l.tex) l.tex = smokeTex, l.emissive = false;
	slots.Retire (first, until, (uint64_t)(uintptr_t)l.tex | (l.emissive ? 1u : 0u));
	aliveUntil = std::max (aliveUntil, until);
}

void ParticleSystem::DrawRetired (VkDescriptorSet frameSet)
{
	slots.ForEachRing ([&](const ParticlePool::Ring &r) {
		if (r.retired) Draw (r.first, r.capacity, (VkTexture*)(uintptr_t)(r.tag & ~(uint64_t)1), (r.tag & 1) != 0, frameSet);
	});
	// the sprites of all rings registered (Scene::Render: after the volume)
	if (ctx) {
		DrawSprites (frameSet);
		ctx->GpuMark ("pt sprites");
	}
}

void ParticleSystem::Queue (Emission &e)
{
	if (!enabled) { e.count = 0; return; } // (legacy streams too)
	if (!e.count) return;
	e.cls = Resolve (e);
	int prio = Priority (e.cls);
	// a source's path over the step far longer than its velocity allows: its
	// start is stale (an emitter that skipped frames without moving it on);
	// in the global frame everything moves at tens of km/s, so the particles
	// would be spread along kilometres of that frame's motion — relative to
	// the ground, hanging in the sky (f5 moon). Started one step back
	// (the path less the step's motion at the emission's velocity: the source
	// moving relative to that, e.g. a vessel through the air, is far less
	// than half the frame's motion: tens of km/s)
	// (relative to the ground (FrameState's carry), where the frame's state is
	// for this step - not for emissions before Scene::Update, the legacy
	// streams': the source's path less what the ground's motion and its
	// velocity relative to it explain; in the global frame the threshold
	// grew with the orbital speed, 750 m at 0.05 s, and an origin shift threw
	// the source light-years. 200 m: a segment's chord (Emission::Segment)
	// passes)
	{
		const vkframe::FrameState *fs = frameState;
		bool rel = fs && fs->Continuous () && fs->Now ().t == oapiGetSimTime () && fs->Dt () > 0.0 && std::fabs (fs->Dt () - e.dt) < 1e-6 * std::max (e.dt, 1.0);
		VECTOR3 vr = rel ? e.vel - fs->FrameVel (e.p1) : e.vel;
		VECTOR3 dev = rel ? e.p1 - fs->CarryPrevToNow (e.p0) - vr * e.dt : (e.p1 - e.p0) - e.vel * e.dt;
		if (length (dev) > std::max (0.5 * length (vr) * e.dt, 200.0)) {
			e.p0 = e.p1 - e.vel * e.dt;
			stats.staleSrc++;
		}
	}
	// dust and spray are sprites: lit puffs with their own colour, not the
	// smoke volume
	if (e.cls == Class::Dust || e.cls == Class::Spray) e.volumetric = false;

	double fill = slots.Fill ();
	ParticlePool::Ring *rr = slots.RingAt (e.first);
	if (!rr || rr->retired) { e.count = 0; return; }
	if (ParticlePool::Shed (fill, prio)) { stats.shed += e.count, e.count = 0; return; }
	if (queued.size () >= MAX_EMISSIONS) { stats.dropped++, e.count = 0; return; }
	// fewer, larger particles while the pool is short (rings of Reserve: as
	// it found; others: the pool's pressure), while many are alive (the
	// live budget), and where they will be small on the screen (LOD: a few
	// pixels across at most over their life: the same coverage and light
	// from fewer)
	double thin = rr->idleRelease ? rr->thin : ParticlePool::PressureFactor (fill, prio);
	thin *= LiveFactor (prio);
	static const int dropCls = getenv ("ORBITER_VK_PARTICLE_DROP") ? atoi (getenv ("ORBITER_VK_PARTICLE_DROP")) : -1; // (debugging: a class not emitted)
	if ((int)e.cls == dropCls) { e.count = 0; return; }
	static const bool lodOn = !getenv ("ORBITER_VK_PARTICLE_LOD") || atoi (getenv ("ORBITER_VK_PARTICLE_LOD")) != 0; // (=0: off, comparisons)
	if (!e.speck && lodOn) {
		// (the camera now: positions are global, everything near moves at tens
		// of km/s in that frame; the last Simulate's camera is a frame behind)
		VECTOR3 cam;
		oapiCameraGlobalPos (&cam);
		double d = std::max (length (e.p1 - cam), 1.0);
		double px = (e.size + std::max (e.growth, 0.0) * e.lifetime * 0.5) * pxPerRad / d; // (its diameter, mid-life [px])
		// (not a fast line source - a trail laid along the source's path over
		// the step, longer than its particles: thinned, a step's whole
		// stretch of it went missing now and then - a reentering piece's
		// trail seen from afar dashed, 230 m gaps)
		double path = length ((e.p1 - e.p0) - e.vel * e.dt);
		if (px < LOD_PX && path < 2.0 * e.size) {
			double f = std::max ((px / LOD_PX) * (px / LOD_PX), LOD_MIN);
			thin *= f, stats.lodThinned += (uint32_t)(e.count * (1.0 - f));
		}
	}
	if (thin < 0.999) {
		float x = (float)(e.count * thin) + rr->thinAcc;
		uint32_t n = (uint32_t)x;
		rr->thinAcc = x - (float)n;
		stats.thinned += e.count - n;
		e.count = n;
		if (!n) return;
		if (!e.speck) {
			double k = std::pow (thin, -1.0 / 3.0);
			e.size *= k, e.growth *= k;
			e.alpha = std::min (e.alpha * k, std::max (e.alpha, 1.0));
		}
	}
	e.lifetime *= ParticlePool::LifeFactor (fill, prio);
	// dust thrown in vacuum: grains in a fast, thin sheet, no billows — nothing
	// to spread them, so they do not grow, and a few metres' puffs of it (lit,
	// against the black sky, far out along the sheet) read as floating rocks
	// (f5 moon). Smaller, the opacity kept about the same per particle's
	// path; drawn as streaks (gpuparticle.vert: longer without drag). After
	// the thinning above, whose larger particles must not undo it
	if (e.cls == Class::Dust && e.ballistic && e.slowdown <= 0.0 && !e.speck) {
		if (e.size > VACUUM_DUST_SIZE) e.alpha = std::min (1.0, e.alpha * std::sqrt (e.size / VACUUM_DUST_SIZE)), e.size = VACUUM_DUST_SIZE;
		e.growth = 0.0;
	}
	queued.push_back (e);
	slots.Touch (e.first, oapiGetSimTime (), e.lifetime);
	if (stats.every > 0.0) stats.particles += std::min (e.count, e.capacity), stats.reentry += e.reentry ? std::min (e.count, e.capacity) : 0;
}

double ParticleSystem::LiveFactor (int prio) const
{
	// the particles alive (read back) against the budget: new emissions
	// thinned above 70 % of it (trails: 85 %), to a quarter at the budget
	double r = (double)liveNow / LIVE_BUDGET, start = prio == 0 ? 0.85 : 0.7;
	if (r <= start) return 1.0;
	return std::max (0.25, 1.0 - 0.75 * (r - start) / (1.0 - start));
}

void ParticleSystem::LogStats (double simt)
{
	if (stats.every <= 0.0) return;
	double now = oapiGetSysTime ();
	if (now < stats.next) return;
	stats.next = now + stats.every;
	oapiWriteLogV ("VK particles: simt=%.1f frames=%u (sim %u idle %u vol %u, toggles %u) live=%u (max %u, vol %u) rings=%u alloc=%u/%u "
		"blocks=%zu free=%u in %zu (largest %u) retired=%u emissions=%u (max/frame %u, dropped %u) particles=%u (reentry %u) "
		"allocs=%u frees=%u allocFails=%u (want %u) short=%u grows=%u shrinks=%u segments=%u overwrites=%u idleFrees=%u thinned=%u shed=%u "
		"sprites=%u (max %u, sort max %u) lod=%u staleSrc=%u",
		simt, stats.frames, stats.simFrames, stats.idleFrames, stats.volFrames, stats.volToggles, stats.live, stats.liveMax, stats.liveVol,
		slots.Rings (), slots.Allocated (), CAPACITY, slots.Blocks ().size (), slots.FreeTotal (), slots.Fragments (), slots.Largest (), slots.RetiredCount (),
		stats.emissions, stats.maxEmissions, stats.dropped, stats.particles, stats.reentry, stats.allocs, stats.frees, stats.allocFails, stats.failWant,
		stats.short_, stats.grows, stats.shrinks, stats.segments, stats.overwrites, stats.idleFrees, stats.thinned, stats.shed,
		stats.drawn, stats.drawnMax, stats.sortMax, stats.lodThinned, stats.staleSrc);
	{
		// (the live particles by class: what lingers, e.g. smoke after a burst in vacuum)
		static const char *cn[14] = { "engine", "trail", "dust", "spray", "fire", "fbsmoke", "spark", "frag", "legacy", "plasma", "contrail", "wreck", "steam", "puff" };
		char buf[512]; int k = 0;
		for (int c = 0; c < 14; c++) if (stats.liveCls[c]) k += snprintf (buf + k, sizeof(buf) - k, " %s=%u", cn[c], stats.liveCls[c]);
		buf[k] = 0;
		oapiWriteLogV ("VK particles: simt=%.1f by class:%s", simt, k ? buf : " none");
	}
	if (stats.updateFrames) {
		double f = 1.0 / stats.updateFrames;
		oapiWriteLogV ("VK particles: simt=%.1f per update (%u): under ground %.1f, specks under %.1f, bounces %.2f, ground rays %.0f m; in air %.0f, mean speed relative to it %.3f m/s",
			simt, stats.updateFrames, stats.update[0] * f, stats.update[1] * f, stats.update[2] * f, (double)stats.update[3] * f,
			stats.update[4] * f, stats.update[4] ? 0.01 * stats.update[5] / stats.update[4] : 0.0);
	}
	memset (stats.update, 0, sizeof(stats.update)), stats.updateFrames = 0;
	if (stats.ringLog) {
		// (the pool by kind of ring: retired, priority, released when idle)
		uint32_t sum[2][4][2] = {}, cnt[2][4][2] = {}, big = 0;
		slots.ForEachRing ([&](const ParticlePool::Ring &r) {
			int p = std::clamp (r.prio, 0, 3);
			sum[r.retired][p][r.idleRelease] += r.capacity, cnt[r.retired][p][r.idleRelease]++;
			big = std::max (big, r.capacity);
		});
		for (int a = 0; a < 2; a++) for (int p = 0; p < 4; p++) for (int i = 0; i < 2; i++)
			if (cnt[a][p][i]) oapiWriteLogV ("VK particles:   %s prio %d %s: %u rings, %u particles", a ? "retired" : "live", p, i ? "idle-release" : "owned", cnt[a][p][i], sum[a][p][i]);
		oapiWriteLogV ("VK particles:   largest ring %u", big);
	}
	stats.frames = stats.simFrames = stats.idleFrames = stats.volFrames = stats.volToggles = 0;
	stats.emissions = stats.maxEmissions = stats.dropped = stats.particles = stats.reentry = 0;
	stats.allocs = stats.frees = stats.allocFails = stats.failWant = 0;
	stats.short_ = stats.grows = stats.shrinks = stats.segments = stats.overwrites = stats.idleFrees = stats.thinned = stats.shed = 0;
	stats.liveMax = stats.drawnMax = stats.sortMax = stats.lodThinned = stats.staleSrc = 0;
}

namespace {
	vkc::Image MakeImage (vkc::Context *ctx, VkImageType type, VkFormat fmt, VkExtent3D e, VkImageUsageFlags usage)
	{
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = type, ici.format = fmt, ici.extent = e;
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	}
	// images into general layout, cleared to a value each (recorded into
	// cmd: no waiting)
	void RecordClears (VkCommandBuffer cmd, const std::vector<std::pair<VkImage, VkClearColorValue>> &ims)
	{
		{
			VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			for (auto &im : ims) {
				vkc::ImageBarrier (cmd, im.first, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
				vkCmdClearColorImage (cmd, im.first, VK_IMAGE_LAYOUT_GENERAL, &im.second, 1, &r);
			}
			VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
			mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
			vkCmdPipelineBarrier2 (cmd, &dep);
		}
	}
}

void ParticleSystem::AllocVolume (VkCommandBuffer cmd)
{
	// (no waiting on the device, mid-frame: the clears go into this frame's
	// commands, each frame slot's set is written at its next Simulate, when
	// the frame that last used it is done)
	const VkImageUsageFlags ACC = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkImageUsageFlags OUT = ACC | VK_IMAGE_USAGE_SAMPLED_BIT;
	const VkImageType T3 = VK_IMAGE_TYPE_3D;
	for (auto &v : volAccum) v = MakeImage (ctx, T3, VK_FORMAT_R32_UINT, { VOL_X, VOL_Y, VOL_Z }, ACC);
	for (auto &v : volMid) v = MakeImage (ctx, T3, VK_FORMAT_R32_UINT, { VOLM_X, VOLM_Y, VOLM_Z }, ACC);
	for (auto &v : volCoarse) v = MakeImage (ctx, T3, VK_FORMAT_R32_UINT, { VOLC_X, VOLC_Y, VOLC_Z }, ACC);
	for (int l = 0; l < 2; l++) {
		VkExtent3D e = l ? VkExtent3D{ VOLC_X, VOLC_Y, VOLC_Z } : VkExtent3D{ VOLM_X, VOLM_Y, VOLM_Z };
		lvlL[l] = MakeImage (ctx, T3, VK_FORMAT_R16G16B16A16_SFLOAT, e, OUT);
		lvlD[l] = MakeImage (ctx, T3, VK_FORMAT_R16G16B16A16_SFLOAT, e, OUT);
		lvlV[l] = MakeImage (ctx, T3, VK_FORMAT_R8_UNORM, e, OUT); // (engine smoke's speed: smokevol.glsl VolCoreSpeed)
	}
	volOccl = MakeImage (ctx, T3, VK_FORMAT_R32_UINT, { VOLM_X, VOLM_Y, VOLM_Z }, ACC);
	occl = MakeImage (ctx, T3, VK_FORMAT_R16_SFLOAT, { VOLM_X, VOLM_Y, VOLM_Z }, OUT);
	volOut = MakeImage (ctx, T3, VK_FORMAT_R16G16B16A16_SFLOAT, { VOL_X, VOL_Y, VOL_Z }, OUT);
	volCore = MakeImage (ctx, T3, VK_FORMAT_R8G8_UNORM, { VOL_X, VOL_Y, VOL_Z }, OUT); // (core, speed: smokevol.glsl VolCoreSpeed)
	volInteg = MakeImage (ctx, T3, VK_FORMAT_R16G16B16A16_SFLOAT, { VOL_X, VOL_Y, VOL_Z }, OUT);
	volColumns = MakeImage (ctx, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, { VOL_X, VOL_Y, 1 }, OUT);
	volOcc = ctx->CreateBuffer (2 * VOL_TILE_COUNT * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	VkClearColorValue zero{}, none{ { 0.0f, 0.0f, 0.0f, 1.0f } };
	std::vector<std::pair<VkImage, VkClearColorValue>> cl;
	for (auto &v : volAccum) cl.push_back ({ v.image, zero });
	for (auto &v : volMid) cl.push_back ({ v.image, zero });
	for (auto &v : volCoarse) cl.push_back ({ v.image, zero });
	for (int l = 0; l < 2; l++) cl.push_back ({ lvlL[l].image, zero }), cl.push_back ({ lvlD[l].image, zero }), cl.push_back ({ lvlV[l].image, zero });
	for (VkImage im : { volOut.image, volCore.image, volColumns.image, volOccl.image, occl.image }) cl.push_back ({ im, zero });
	cl.push_back ({ volInteg.image, none });
	RecordClears (cmd, cl);
	vkCmdFillBuffer (cmd, volOcc.buffer, 0, VK_WHOLE_SIZE, 0);
	volAlloc = true, volFull = true;
	AllocMarch (cmd);
	for (bool &d : volSetDirty) d = true;
	if (stats.every > 0.0) oapiWriteLogV ("VK particles: smoke volume allocated");
}

void ParticleSystem::AllocMarch (VkCommandBuffer cmd)
{
	VkExtent2D re = ctx->RenderExtent ();
	marchExtent = { std::max (1u, (re.width + 1) / 2), std::max (1u, (re.height + 1) / 2) };
	for (auto &m : march) ctx->DestroyImage (m);
	const VkImageUsageFlags U = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	for (uint32_t k = 0; k < 5; k++)
		march[k] = MakeImage (ctx, VK_IMAGE_TYPE_2D, k == 2 ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT, { marchExtent.width, marchExtent.height, 1 }, U);
	VkClearColorValue none{ { 0.0f, 0.0f, 0.0f, 1.0f } };
	std::vector<std::pair<VkImage, VkClearColorValue>> cl;
	for (uint32_t k = 0; k < 5; k++) cl.push_back ({ march[k].image, k == 2 ? VkClearColorValue{} : none });
	marchFrame = 0; // (no history)
	if (cmd) RecordClears (cmd, cl);
	else ctx->ImmediateSubmit ([&cl](VkCommandBuffer c) { RecordClears (c, cl); });
	marchGen = ctx->SwapchainGeneration ();
	for (bool &d : volSetDirty) d = true;
}

void ParticleSystem::WriteVolumeSets (uint32_t slot)
{
	const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
	volSetDirty[slot] = false;
	for (VkDescriptorSet st : { sets[slot] }) {
		std::vector<VkWriteDescriptorSet> w;
		std::vector<VkDescriptorImageInfo> ii;
		ii.reserve (96);
		auto add = [&](uint32_t binding, uint32_t elem, VkDescriptorType t, VkSampler smp, VkImageView v) {
			ii.push_back ({ smp, v, G });
			VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			wr.dstSet = st, wr.dstBinding = binding, wr.dstArrayElement = elem, wr.descriptorCount = 1;
			wr.descriptorType = t, wr.pImageInfo = &ii.back ();
			w.push_back (wr);
		};
		const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		if (volAlloc) {
			for (uint32_t k = 0; k < 8; k++) {
				add (11, k, SI, VK_NULL_HANDLE, volAccum[k].view);
				add (26, k, SI, VK_NULL_HANDLE, volMid[k].view);
				add (29, k, SI, VK_NULL_HANDLE, volCoarse[k].view);
			}
			add (15, 0, SI, VK_NULL_HANDLE, volOut.view);
			add (16, 0, SI, VK_NULL_HANDLE, volColumns.view);
			add (17, 0, CI, volSampler, volOut.view);
			add (18, 0, CI, volSampler, volColumns.view);
			add (23, 0, CI, volSampler, volInteg.view);
			add (24, 0, SI, VK_NULL_HANDLE, volInteg.view);
			for (uint32_t l = 0; l < 2; l++) {
				add (31, l, SI, VK_NULL_HANDLE, lvlL[l].view);
				add (32, l, SI, VK_NULL_HANDLE, lvlD[l].view);
				add (33, l, CI, volSampler, lvlL[l].view);
				add (34, l, CI, volSampler, lvlD[l].view);
				add (51, l, SI, VK_NULL_HANDLE, lvlV[l].view);
				add (52, l, CI, volSampler, lvlV[l].view);
			}
			add (35, 0, SI, VK_NULL_HANDLE, volCore.view);
			add (45, 0, SI, VK_NULL_HANDLE, volOccl.view);
			add (46, 0, SI, VK_NULL_HANDLE, occl.view);
			add (47, 0, CI, volSampler, occl.view);
			add (36, 0, CI, volSampler, volCore.view);
			const uint32_t mk[3] = { MarchNear (slot), MarchFar (slot), 2 };
			for (uint32_t k = 0; k < 3; k++) add (38 + k, 0, SI, VK_NULL_HANDLE, march[mk[k]].view);
			for (uint32_t k = 0; k < 3; k++) add (41 + k, 0, CI, volSampler, march[mk[k]].view); // (linear: the composite's early out; taps by texelFetch)
			add (49, 0, CI, volSampler, march[MarchNear (slot ^ 1)].view), add (50, 0, CI, volSampler, march[MarchFar (slot ^ 1)].view);
		} else add (23, 0, CI, volSampler, volDummy.view);
		vkUpdateDescriptorSets (ctx->device, (uint32_t)w.size (), w.data (), 0, nullptr);
		if (volAlloc) {
			VkDescriptorBufferInfo bi{ volOcc.buffer, 0, VK_WHOLE_SIZE };
			VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			wr.dstSet = st, wr.dstBinding = 37, wr.descriptorCount = 1;
			wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr.pBufferInfo = &bi;
			vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
		}
	}
}

void ParticleSystem::FreeVolume (bool now)
{
	if (!volAlloc) return;
	volAlloc = false;
	for (bool &d : volSetDirty) d = true; // (the sprites' fog: none, at each slot's next Simulate)
	// (the images may be in use by frames in flight: destroyed a few frames
	// later (now: at shutdown, the device idle))
	std::vector<vkc::Image> ims;
	for (auto &v : volAccum) ims.push_back (v), v = vkc::Image{};
	for (auto &v : volMid) ims.push_back (v), v = vkc::Image{};
	for (auto &v : volCoarse) ims.push_back (v), v = vkc::Image{};
	for (int l = 0; l < 2; l++) ims.push_back (lvlL[l]), ims.push_back (lvlD[l]), ims.push_back (lvlV[l]), lvlL[l] = lvlD[l] = lvlV[l] = vkc::Image{};
	for (vkc::Image *im : { &volOut, &volCore, &volInteg, &volColumns, &volOccl, &occl, &march[0], &march[1], &march[2], &march[3], &march[4] }) ims.push_back (*im), *im = vkc::Image{};
	volGrave.push_back ({ frameNo, ims, volOcc });
	volOcc = vkc::Buffer{};
	marchGen = ~0u;
	if (now) ReapVolume (true);
	if (stats.every > 0.0) oapiWriteLogV ("VK particles: smoke volume released");
}

void ParticleSystem::ReapVolume (bool all)
{
	for (size_t k = 0; k < volGrave.size (); ) {
		if (all || frameNo > volGrave[k].frame + vkc::FRAMES_IN_FLIGHT + 1) {
			for (auto &im : volGrave[k].images) ctx->DestroyImage (im);
			ctx->DestroyBuffer (volGrave[k].occ);
			volGrave.erase (volGrave.begin () + k);
		} else k++;
	}
}

void ParticleSystem::ApplySettings ()
{
	// (the graphics settings' smoke rendering and trails, live; the
	// environment's ORBITER_VK_SMOKE, _SMOKE_TRAILS override them there)
	const vkc::GfxSettings &g = vkc::Gfx ();
	smokeMode = g.smokeRendering == vkc::GfxSettings::SMOKE_SPRITES ? SMOKE_SPRITES
		: g.smokeRendering == vkc::GfxSettings::SMOKE_SCREEN ? SMOKE_SCREEN : SMOKE_VOLUME;
	trails = g.smokeTrails;
}

void ParticleSystem::SetPrepass (VkImageView prepass)
{
	ApplySettings ();
	EnsureScreenSmoke (); // (before any of this frame's sets are bound)
	if (prepass == prepassView) return;
	if (prepassView) ctx->WaitIdle (); // (the sets may be in use)
	prepassView = prepass;
	VkDescriptorImageInfo ii{ clampSampler, prepass, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	for (VkDescriptorSet s : sets) {
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = s, wr.dstBinding = 4, wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &ii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void ParticleSystem::Simulate (const vkframe::Snapshot &now, const VECTOR3 &sunDir, VkDescriptorSet frameSet, VkDescriptorSet atmSet,
	const VkDescriptorSet *rt, double simt)
{
	const VECTOR3 &camPos = now.cam;
	VkCommandBuffer cmd = ctx->Cmd ();
	uint32_t fi = ctx->FrameIndex ();
	VkDescriptorSet set = sets[fi];
	auto barrier = [cmd](VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA) {
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = src, mb.srcAccessMask = srcA, mb.dstStageMask = dst, mb.dstAccessMask = dstA;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	frameNo++;
	// the rings registered for drawing (Draw): none yet; the sprites drawn
	// by this frame slot's last frame (the next sort's size)
	memset (ringInfoBuf[fi].mapped, 0, (CAPACITY / BLOCK) * sizeof(uint32_t));
	drawPending = false;
	uint32_t prevDrawn = 0;
	if (drawValid[fi]) {
		vmaInvalidateAllocation (ctx->allocator, drawRead[fi].alloc, 0, VK_WHOLE_SIZE);
		prevDrawn = ((const uint32_t*)drawRead[fi].mapped)[1];
		stats.drawn = prevDrawn, stats.drawnMax = std::max (stats.drawnMax, prevDrawn);
	}
	// the previous frame's drawing is done with the pool
	barrier (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | CS, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
	// rings allocated since: no particles
	for (auto &c : toClear)
		vkCmdFillBuffer (cmd, pool.buffer, (VkDeviceSize)c.first * PARTICLE_SIZE, (VkDeviceSize)c.second * PARTICLE_SIZE, 0);
	if (!toClear.empty()) barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
	toClear.clear ();

	for (const Emission &e : queued) aliveUntil = std::max (aliveUntil, simt + 1.5 * e.lifetime + 1.0); // (particle_emit.comp: up to 1.5 lifetimes)

	{
		// the particles counted by this frame slot's last update (the live
		// budget; telemetry)
		uint32_t *w = (uint32_t*)windBuf[fi].mapped + WIND_LAYERS * 4;
		if (nearValid[fi]) liveNow = w[2], stats.live = w[2], stats.liveVol = w[3], stats.liveMax = std::max (stats.liveMax, stats.live);
		if (nearValid[fi] && stats.every > 0.0) memcpy (stats.liveCls, w + 4, sizeof(stats.liveCls));
		w[1] = stats.every > 0.0 ? 2u : 1u, w[2] = w[3] = 0u; // (statsOn: 2 counts by class too - telemetry)
		memset (w + 4, 0, sizeof(stats.liveCls));
		// (particle_update.glsl's telemetry: summed until the next log line)
		uint32_t *tl = (uint32_t*)windBuf[fi].mapped + (WIND_LAYERS + 5 + 1 + 3 * MAX_GROUND_JETS + 4 + FLAME_POINTS) * 4;
		if (nearValid[fi] && stats.every > 0.0) {
			for (int k = 0; k < 6; k++) stats.update[k] += tl[k];
			stats.updateFrames++;
		}
		memset (tl, 0, 8 * sizeof(uint32_t));
	}
	if (stats.every > 0.0) {
		stats.frames++;
		stats.emissions += (uint32_t)queued.size (), stats.maxEmissions = std::max (stats.maxEmissions, (uint32_t)queued.size ());
	}
	// rings of streams that are gone, their particles expired, and idle
	// rings: free
	{
		uint32_t retiredBefore = slots.RetiredCount (), ringsBefore = slots.Rings ();
		size_t n = slots.Expire (simt);
		uint32_t retiredFreed = retiredBefore - slots.RetiredCount ();
		stats.frees += (uint32_t)n, stats.idleFrees += (ringsBefore - slots.Rings ()) - retiredFreed;
	}
	if (queued.empty() && simt > aliveUntil) {
		if (idleUpdates >= 2) { // (the particles have been updated as dead)
			idle = true;
			stats.idleFrames++, stats.live = stats.liveVol = 0;
			LogStats (simt);
			for (bool &v : nearValid) v = false;
			for (bool &v : drawValid) v = false;
			lastSnap = now;
			pendingDt = 0.0;
			groundJets.clear (); // (this frame's: stale by the next, kilometres off in the global frame)
			return;
		}
		idleUpdates++;
	} else idleUpdates = 0;
	idle = false;

	// move and light the existing particles
	UpdateParams up{};
	// the particles carried with the ground (FrameState.h): their positions
	// relative to the last Simulate's camera, at its time, to this one's;
	// shift: the move of a point at rest in the global frame (the camera's
	// move in it, reversed)
	const vkframe::Carry carry = vkframe::CarryBetween (lastSnap, now);
	const VECTOR3 &shift = carry.shift;
	// the smoke volume: only while a volumetric particle may be within its
	// reach (the nearest one's distance as found by this frame slot's last
	// update, FRAMES_IN_FLIGHT frames ago; the camera's moves since; new
	// particles near)
	{
		uint32_t *nearBits = (uint32_t*)windBuf[fi].mapped + WIND_LAYERS * 4;
		float nearPrev;
		memcpy (&nearPrev, nearBits, sizeof(float));
		double reach = VOL_REACH + 2000.0 + 2.0 * vkc::FRAMES_IN_FLIGHT * carry.dist; // (relative to the ground: the smoke's frame)
		nearKnown = nearValid[fi] && nearPrev < reach;
		volActive = !nearValid[fi] || nearPrev < reach;
		nearEmit = false;
		for (const Emission &e : queued)
			if (e.volumetric && std::min (length (e.p0 - camPos), length (e.p1 - camPos)) - e.size < reach) volActive = nearEmit = true;
		const uint32_t far = 0x7F800000u; // (+inf)
		*nearBits = far;
		nearValid[fi] = true;
	}
	// the volume's images: allocated when smoke is first known to be near
	// (read back, or emitted near), released when another mode is chosen or
	// none was near for two minutes; this slot's set rewritten if they
	// changed (before any of this frame's sets are bound)
	{
		bool volOn = enabled && smokeMode == SMOKE_VOLUME && volActive;
		double now = oapiGetSysTime ();
		if (volOn) {
			if (!volAlloc && (nearKnown || nearEmit)) AllocVolume (cmd);
			if (volAlloc) volIdleSince = now;
		} else if (volAlloc && (smokeMode != SMOKE_VOLUME || !enabled || now - volIdleSince > 120.0)) FreeVolume (false);
		if (!volOn || !volAlloc) volFull = true; // (stale when it is next used)
		if (volSetDirty[fi]) WriteVolumeSets (fi);
		if (fbOccDirty[fi]) WriteFireballOcclusion (fi);
		ReapVolume (false);
	}
	lastSnap = now;
	pxPerRad = ctx->RenderExtent ().height / (2.0 * std::tan (std::max (oapiCameraAperture (), 1e-3)));
	up.camShift[0] = (float)shift.x, up.camShift[1] = (float)shift.y, up.camShift[2] = (float)shift.z;
	// (the camera's velocity: its move over the step; a cut - more than 20
	// km/s from the planet nearby's - or paused: the planet's)
	if (pendingDt > 0.0) {
		VECTOR3 cv = -shift / pendingDt, bv{};
		if (OBJHANDLE hb = oapiCameraProxyGbody ()) oapiGetGlobalVel (hb, &bv); // (not up.bodyVel: filled in below - it was 0, every frame a cut)
		if (length (cv - bv) > 2e4) cv = bv;
		for (int k = 0; k < 3; k++) drawCamVel[k] = (float)(&cv.x)[k];
	}
	up.camShift[3] = (float)pendingDt;

	double camHeight = 0.0; // the camera's above the ground under it (particles.glsl camGround)
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) { // the air of the planet nearby
		VECTOR3 bp, bv;
		oapiGetGlobalPos (hp, &bp);
		oapiGetGlobalVel (hp, &bv);
		bp -= camPos;
		up.bodyPos[0] = (float)bp.x, up.bodyPos[1] = (float)bp.y, up.bodyPos[2] = (float)bp.z, up.bodyPos[3] = (float)oapiGetSize (hp);
		up.bodyVel[0] = (float)bv.x, up.bodyVel[1] = (float)bv.y, up.bodyVel[2] = (float)bv.z;
		if (const ATMCONST *ac = oapiGetPlanetAtmConstants (hp)) {
			double g = GGRAV * oapiGetMass (hp) / (oapiGetSize (hp) * oapiGetSize (hp));
			up.atm[0] = (float)ac->rho0, up.atm[1] = (float)(ac->rho0 > 0.0 && g > 0.0 ? ac->p0 / (ac->rho0 * g) : 8e3); // (isothermal: H = p0 / (rho0 g))
		}
		// the ground under the camera (collisions without ray tracing)
		VECTOR3 loc;
		oapiGlobalToLocal (hp, &camPos, &loc);
		double rad = length (loc);
		if (rad > 0.0) {
			double rg = oapiGetSize (hp) + oapiSurfaceElevation (hp, atan2 (loc.z, loc.x), asin (std::clamp (loc.y / rad, -1.0, 1.0)));
			up.ground[0] = (float)rg;
			camHeight = rad - rg; // (in double: particles.glsl camGround)
		}
		up.ground[1] = (float)(GGRAV * oapiGetMass (hp) / (oapiGetSize (hp) * oapiGetSize (hp)));
		// the wind's layers (above the camera's point; on terrestrial planets
		// the same everywhere)
		{
			float *w = (float*)windBuf[fi].mapped;
			double lng = 0.0, lat = 0.0, rad;
			oapiGlobalToEqu (hp, camPos, &lng, &lat, &rad);
			for (int k = 0; k < WIND_LAYERS; k++) {
				VECTOR3 v = oapiGetModelWind (hp, lng, lat, k * 1e3); // (whether or not there are trails)
				w[4 * k] = (float)v.x, w[4 * k + 1] = (float)v.z, w[4 * k + 2] = w[4 * k + 3] = 0.0f;
			}
		}
		double T = oapiGetPlanetPeriod (hp);
		if (T != 0.0) {
			MATRIX3 R;
			oapiGetRotationMatrix (hp, &R);
			VECTOR3 w = mul (R, _V(0, 1, 0)) * (PI2 / T);
			up.omega[0] = (float)w.x, up.omega[1] = (float)w.y, up.omega[2] = (float)w.z;
		}
	}
	up.bodyVel[3] = (float)fmod (simt, 1e4);
	// the ground jets (camera-relative; the strongest, if more)
	{
		if ((int)groundJets.size () > MAX_GROUND_JETS)
			std::partial_sort (groundJets.begin (), groundJets.begin () + MAX_GROUND_JETS, groundJets.end (),
				[](const GroundJet &a, const GroundJet &b) { return a.speed * a.reach > b.speed * b.reach; });
		int nj = std::min ((int)groundJets.size (), MAX_GROUND_JETS);
		uint32_t *cnt = (uint32_t*)windBuf[fi].mapped + (WIND_LAYERS + 5) * 4;
		float *j = (float*)(cnt + 4);
		cnt[0] = (uint32_t)nj;
		for (int k = 0; k < nj; k++) {
			const GroundJet &g = groundJets[k];
			VECTOR3 p = g.pos - camPos; // (as the emissions: Queue)
			float *e = j + 12 * k; // (particles.glsl groundJet: three vec4 each)
			e[0] = (float)p.x, e[1] = (float)p.y, e[2] = (float)p.z, e[3] = (float)g.r0;
			e[4] = (float)g.n.x, e[5] = (float)g.n.y, e[6] = (float)g.n.z, e[7] = (float)g.speed;
			e[8] = (float)g.reach, e[9] = (float)g.thick, e[10] = e[11] = 0.0f;
		}
		groundJets.clear ();
		// the flames' dust grid (particles.glsl flameBox: camera-relative)
		float *fbx = j + 12 * MAX_GROUND_JETS;
		memset (fbx, 0, (4 + FLAME_POINTS) * 4 * sizeof(float));
		if (flameBox.on) {
			const FlameBox &b = flameBox;
			VECTOR3 c = b.centre - camPos;
			const double hw[3] = { FLAME_HALF_W, FLAME_HALF_W, FLAME_HALF_H };
			const int nc[3] = { FLAME_NX, FLAME_NX, FLAME_NZ };
			const VECTOR3 *ax[3] = { &b.ax, &b.ay, &b.az };
			fbx[0] = (float)c.x, fbx[1] = (float)c.y, fbx[2] = (float)c.z, fbx[3] = 1.0f;
			for (int k = 0; k < 3; k++) {
				VECTOR3 a = *ax[k] / (2.0 * hw[k]);
				fbx[4 + 4 * k] = (float)a.x, fbx[5 + 4 * k] = (float)a.y, fbx[6 + 4 * k] = (float)a.z, fbx[7 + 4 * k] = (float)(2.0 * hw[k] / nc[k]);
			}
			double wsum = 0.0;
			for (int k = 0; k < b.n; k++) wsum += b.weight[k];
			for (int k = 0; k < std::min (b.n, FLAME_POINTS) && wsum > 0.0; k++) {
				VECTOR3 l = b.light[k] - camPos;
				float *e = fbx + 16 + 4 * k;
				e[0] = (float)l.x, e[1] = (float)l.y, e[2] = (float)l.z, e[3] = (float)(b.weight[k] / wsum);
			}
		}
		// the carry (particles.glsl frameCarry; after the telemetry)
		float *fc = fbx + 4 * (4 + FLAME_POINTS) + 8;
		for (int k = 0; k < 3; k++)
			fc[k] = (float)(&carry.T.x)[k], fc[4 + k] = (float)(&carry.v.x)[k], fc[8 + k] = (float)(&carry.D0.x)[k];
		fc[3] = (float)(int)carry.frame, fc[7] = carry.continuous ? 1.0f : 0.0f, fc[11] = 0.0f;
		fc[12] = (float)camHeight, fc[13] = fc[14] = fc[15] = 0.0f;
		static const bool carryLog = getenv ("ORBITER_VK_CARRY_LOG") != nullptr; // (debugging: the carry's rotation against omega dt)
		if (carryLog && pendingDt > 0.0)
			oapiWriteLogV ("VK particles: carry frame %d |T| %.3f m, v/dt (%.4g %.4g %.4g), omega (%.4g %.4g %.4g), |D0| %.1f m, |shift| %.1f m, dt %.4f",
				(int)carry.frame, carry.dist, carry.v.x / pendingDt, carry.v.y / pendingDt, carry.v.z / pendingDt,
				now.body.omega.x, now.body.omega.y, now.body.omega.z, length (carry.D0), length (shift), pendingDt);
	}
	for (int k = 0; k < 3; k++)
		drawBody[k] = up.bodyPos[k], drawBody[4 + k] = up.bodyVel[k], drawBody[8 + k] = up.omega[k];
	drawBody[3] = up.bodyPos[3] > 0.0f ? 1.0f : 0.0f;
	// the allocated blocks (rings are cleared when allocated, so the
	// particles left in free blocks are never seen again): the passes over
	// the particles go over these only
	const std::vector<uint32_t> &blocks = slots.Blocks ();
	memcpy (blockListBuf[fi].mapped, blocks.data (), blocks.size () * sizeof(uint32_t));
	const uint32_t nslots = (uint32_t)blocks.size () * BLOCK;
	const uint32_t groups = nslots / 64; // (BLOCK is a multiple of 64)
	up.omega[3] = (float)nslots;
	up.ground[3] = smokeMode == SMOKE_VOLUME && volAlloc ? 1.0f : 0.0f; // (no volume yet: all as sprites)
	// the sprites' list, sorted: its size from the sprites of this frame
	// slot's last frame and the particles emitted since, with a margin; all
	// allocated after a camera cut or a change of mode
	uint32_t emitted = 0;
	for (const Emission &e : queued) emitted += std::min (e.count, e.capacity);
	emitHist[fi] = emitted;
	uint32_t sortSize = nslots;
	if (drawValid[fi] && prevDrawn < sortN[fi] && carry.dist < 1000.0 && smokeMode == lastMode) {
		uint64_t b = (uint64_t)prevDrawn + std::max (prevDrawn / 4, 2048u);
		for (uint32_t k : emitHist) b += k;
		sortSize = (uint32_t)std::min<uint64_t> (b, nslots);
	}
	lastMode = smokeMode;
	uint32_t n = SORT_TILE; // (whole tiles of the shared-memory steps)
	while (n < sortSize) n <<= 1;
	sortN[fi] = n;
	stats.sortMax = std::max (stats.sortMax, n);
	// (keys beyond the list: last; the list's length counted from 0)
	vkCmdFillBuffer (cmd, sortKeys.buffer, 0, (VkDeviceSize)n * sizeof(uint32_t), 0xFFFFFFFFu);
	const uint32_t args[4] = { 4, 0, 0, 0 };
	vkCmdUpdateBuffer (cmd, drawArgs[fi].buffer, 0, sizeof(args), args);
	barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
	// noise anchored to the planet nearby (turning with it): the camera's
	// position in its frame, the noise coordinates wrapped (the noise tiles;
	// octaves at integer ratios); positions turned into its axes in the
	// shaders (noiseRot)
	{
		VECTOR3 camLoc = camPos, upLoc = _V(0, 1, 0);
		MATRIX3 R = _M(1, 0, 0, 0, 1, 0, 0, 0, 1);
		if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
			oapiGetRotationMatrix (hp, &R);
			oapiGlobalToLocal (hp, &camPos, &camLoc);
			if (length (camLoc) > 0.0) upLoc = unit (camLoc);
		}
		auto wrap = [](double q) { return (float)(q - floor (q)); };
		for (int k = 0; k < 3; k++) {
			double c = (&camLoc.x)[k];
			up.noiseOrg[k] = wrap (c * NOISE_SCALE);
			up.trailNoise[k] = wrap (c * TRAIL_NOISE_SCALE);
			// (the erosion's billows rising slowly)
			up.noiseRot[k][3] = wrap (c * MARCH_NOISE_SCALE - (&upLoc.x)[k] * fmod (simt * MARCH_NOISE_RISE, 1e4));
			for (int j = 0; j < 3; j++) up.noiseRot[k][j] = (float)R.data[3 * j + k]; // (row k of R^T)
		}
		up.noiseOrg[3] = (float)NOISE_SCALE;
		up.trailNoise[3] = (float)TRAIL_NOISE_SCALE;
		up.vol[3] = (float)MARCH_NOISE_SCALE;
	}
	// smoke shadow grid: axes across the sunlight and towards the sun
	VECTOR3 lz = unit (sunDir);
	VECTOR3 lx = unit (crossp (lz, fabs (lz.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0))), ly = crossp (lz, lx);
	for (int k = 0; k < 3; k++)
		up.lightX[k] = (float)(&lx.x)[k], up.lightY[k] = (float)(&ly.x)[k], up.lightZ[k] = (float)(&lz.x)[k];
	static const bool noSmokeShadow = getenv ("ORBITER_VK_SMOKE_NOSHADOW") != nullptr; // (debugging)
	up.lightX[3] = (float)SHADOW_HALF_WIDTH, up.lightY[3] = (float)SHADOW_HALF_DEPTH, up.lightZ[3] = noSmokeShadow ? 0.0f : (float)SMOKE_EXTINCTION;
	pendingDt = 0.0;
	bool useRT = rt && updateRTPipe;
	VkPipelineLayout ul = useRT ? updateRTLayout : updateLayout;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, useRT ? updateRTPipe : updatePipe);
	if (useRT) {
		VkDescriptorSet s[5] = { set, frameSet, rt[0], rt[1], atmSet };
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ul, 0, 5, s, 0, nullptr);
	} else {
		VkDescriptorSet s[3] = { set, frameSet, atmSet };
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ul, 0, 3, s, 0, nullptr);
	}
	auto bindSets = [&](VkPipelineLayout l, bool withRT) {
		if (withRT) {
			VkDescriptorSet s[5] = { set, frameSet, rt[0], rt[1], atmSet };
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l, 0, 5, s, 0, nullptr);
		} else {
			VkDescriptorSet s[3] = { set, frameSet, atmSet };
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l, 0, 3, s, 0, nullptr);
		}
		vkCmdPushConstants (cmd, l, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
	};
	vkCmdPushConstants (cmd, ul, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
	vkCmdDispatch (cmd, groups, 1, 1);
	ctx->GpuMark ("pt update");

	// new particles (at the end of the step: not moved again)
	if (!queued.empty()) {
		barrier (CS, RW, CS, RW);
		GpuEmission *ge = (GpuEmission*)emitBuf[fi].mapped;
		uint32_t maxCount = 0, n = 0;
		static uint32_t seed = 1;
		for (const Emission &e : queued) {
			GpuEmission &g = ge[n++];
			VECTOR3 p0 = e.p0 - camPos, p1 = e.p1 - camPos;
			float f0[4] = { (float)p0.x, (float)p0.y, (float)p0.z, (float)e.v0 };
			float f1[4] = { (float)p1.x, (float)p1.y, (float)p1.z, (float)e.spread };
			float fv[4] = { (float)e.vel.x, (float)e.vel.y, (float)e.vel.z, (float)e.lifetime };
			float fd[4] = { (float)e.dir.x, (float)e.dir.y, (float)e.dir.z, (float)e.dt };
			// (drag >= 0; a trail's smoke: a flag)
			float fs[4] = { (float)e.size, (float)e.growth, (float)e.alpha, (float)std::fabs (e.slowdown) };
			memcpy (g.p0, f0, 16), memcpy (g.p1, f1, 16), memcpy (g.vel, fv, 16), memcpy (g.dir, fd, 16), memcpy (g.shape, fs, 16);
			uint32_t count = std::min (e.count, e.capacity);
			g.slab[0] = e.first, g.slab[1] = e.capacity, g.slab[2] = e.head, g.slab[3] = count;
			g.misc[0] = seed++, g.misc[1] = (e.emissive ? 1u : 0u) | (e.reentry ? 2u : 0u) | (e.tinted ? 4u : 0u) | (e.planar ? 8u : 0u)
				| (e.ballistic ? 16u : 0u) | (e.cold ? 32u : 0u) | (e.volumetric ? 64u : 0u) | (e.speck ? 128u : 0u) | (e.spherical ? 256u : 0u)
				| (e.trail || e.slowdown < 0.0 ? 512u : 0u) | (e.ramp ? 1024u : 0u) | (e.erode ? 2048u : 0u)
				| (e.jetSmoke ? 4096u : 0u);
			uint32_t rgb = 0;
			for (int k = 0; k < 3; k++) rgb |= (uint32_t)std::lround (std::clamp (e.albedo[k], 0.0f, 1.0f) * 255.0f) << (8 * k);
			// (streak: 0 the class's, 255 none, else 2 ms units: particles.glsl ParticleStreak)
			uint32_t streak = e.streak < 0.0f ? 255u : e.streak > 0.0f ? (uint32_t)std::clamp (std::lround (e.streak / 0.002f), 1l, 254l) : 0u;
			g.misc[2] = rgb, g.misc[3] = (uint32_t)Resolve (e) | (streak << 8);
			maxCount = std::max (maxCount, count);
		}
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emitPipe);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emitLayout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants (cmd, emitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(n), &n);
		vkCmdDispatch (cmd, (maxCount + 63) / 64, n, 1);
		queued.clear ();
	}
	ctx->GpuMark ("pt emit");

	// smoke shadows: smoke per cell of the cascades, summed towards the sun
	// (the tiles with smoke this frame or the last; the accumulation
	// cleared as it is read), the tiles' marks of the last frame cleared
	static const bool volAllTiles = getenv ("ORBITER_VK_VOL_ALLTILES") != nullptr; // (debugging)
	static const bool volNoCoarse = getenv ("ORBITER_VK_VOL_NOCOARSE") != nullptr; // (debugging)
	static const bool volNoNoise = getenv ("ORBITER_VK_VOL_NONOISE") != nullptr; // (debugging: the erosion at its mean)
	static const uint32_t volDbg = getenv ("ORBITER_VK_VOL_DBG") ? (uint32_t)atoi (getenv ("ORBITER_VK_VOL_DBG")) & 63u : 0u; // (debugging: smoke_march.comp)
	up.vol[2] = (float)((volFull || volAllTiles ? 1u : 0u) | (shadowParity << 1) | (volNoCoarse ? 4u : 0u) | (volNoNoise ? 8u : 0u) | (volDbg << 4));
	barrier (CS | VK_PIPELINE_STAGE_2_TRANSFER_BIT, RW | VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, splatPipe);
	bindSets (updateLayout, false);
	vkCmdDispatch (cmd, groups, 1, 1);
	barrier (CS, RW, CS, RW);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sumPipe);
	vkCmdDispatch (cmd, SHADOW_NX / 8, SHADOW_NX / 8, SHADOW_CASCADES);
	barrier (CS, RW, CS | VK_PIPELINE_STAGE_2_TRANSFER_BIT, RW | VK_ACCESS_2_TRANSFER_WRITE_BIT);
	vkCmdFillBuffer (cmd, shadowOcc.buffer, (VkDeviceSize)(1 - shadowParity) * SHADOW_TILE_COUNT * sizeof(uint32_t), SHADOW_TILE_COUNT * sizeof(uint32_t), 0);
	barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
	shadowParity ^= 1u;
	ctx->GpuMark ("pt shadow");

	// the dust around the brightest vessel's flames: its extinction, then
	// each cell's transmittance towards them (the lights' shadows of the
	// dust: lights.glsl FlameOcc - the surfaces, the sprites and the smoke
	// volume drawn after this read it)
	if (flameBox.on) {
		VkClearColorValue zero{};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		barrier (CS, RW, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		vkCmdClearColorImage (cmd, flameAccum.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
		barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, flameSplatPipe);
		bindSets (updateLayout, false);
		vkCmdDispatch (cmd, groups, 1, 1);
		// (the transmittance, read by the last frame's surfaces and sprites -
		// one image for the frames in flight: not rewritten under them)
		barrier (CS | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, RW, CS, RW);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, flameTransPipe);
		vkCmdDispatch (cmd, FLAME_NX / 4, FLAME_NX / 4, FLAME_NZ / 4);
		barrier (CS, RW, CS | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		ctx->GpuMark ("pt flame dust");
	}

	// sunlight on the particles
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, useRT ? lightRTPipe : lightPipe);
	bindSets (ul, useRT);
	vkCmdDispatch (cmd, groups, 1, 1);
	barrier (CS, RW, CS, RW);
	ctx->GpuMark ("pt light");

	// smoke volume: volumetric particles into the froxels (and the coarse
	// grid), resolved (the coarse grid, then the tiles of columns with smoke
	// this frame or the last, clearing the accumulators as they are read);
	// the tiles' marks of the last frame cleared for the next
	if (smokeMode == SMOKE_VOLUME && volActive && volAlloc) {
		barrier (CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, RW | VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
		up.vol[0] = 0.0f, up.vol[1] = (float)volParity; // (vol[2]: set with the shadows)
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, volSplatPipe);
		bindSets (updateLayout, false);
		vkCmdDispatch (cmd, groups, 1, 1);
		barrier (CS, RW, CS, RW);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, volResolvePipe);
		up.vol[0] = 1.0f; // (the half-resolution grid, then the quarter)
		vkCmdPushConstants (cmd, updateLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
		vkCmdDispatch (cmd, (VOLM_X + 7) / 8, (VOLM_Y + 7) / 8, VOLM_Z);
		up.vol[0] = 2.0f;
		vkCmdPushConstants (cmd, updateLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
		vkCmdDispatch (cmd, (VOLC_X + 7) / 8, (VOLC_Y + 7) / 8, VOLC_Z);
		barrier (CS, RW, CS, RW);
		up.vol[0] = 0.0f;
		vkCmdPushConstants (cmd, updateLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
		vkCmdDispatch (cmd, VOL_TILES_X, VOL_TILES_Y, 1);
		barrier (CS, RW, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
		vkCmdFillBuffer (cmd, volOcc.buffer, (VkDeviceSize)(1 - volParity) * VOL_TILE_COUNT * sizeof(uint32_t), VOL_TILE_COUNT * sizeof(uint32_t), 0);
		barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
		volParity ^= 1u, volFull = false;
	}
	ctx->GpuMark ("pt volume");
	if (stats.every > 0.0) {
		stats.simFrames++;
		bool v = smokeMode == SMOKE_VOLUME && volActive;
		stats.volFrames += v ? 1 : 0, stats.volToggles += v != stats.lastVol ? 1 : 0;
		stats.lastVol = v;
	}
	volNoise[0] = up.noiseOrg[0], volNoise[1] = up.noiseOrg[1], volNoise[2] = up.noiseOrg[2], volNoise[3] = up.bodyVel[3];
	memcpy (lastUp, &up, sizeof(up)), lastAtm = atmSet; // (the march: RenderScreenSmoke)

	// draw order: the sprites of all rings far to near (their list compacted
	// over the allocated blocks, a bitonic sort over a power of two)
	const uint32_t sortBase = 0;
	up.ground[2] = (float)n;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sortKeysPipe);
	bindSets (updateLayout, false);
	vkCmdDispatch (cmd, groups, 1, 1);
	// (bitonic: the steps within a tile of SORT_TILE keys in shared memory,
	// the larger ones over the keys)
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sortLayout, 0, 1, &set, 0, nullptr);
	auto sortStep = [&](VkPipeline pipe, uint32_t j, uint32_t k, uint32_t groups) {
		barrier (CS, RW, CS, RW);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		uint32_t pcs[4] = { j, k, n, sortBase };
		vkCmdPushConstants (cmd, sortLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs), pcs);
		vkCmdDispatch (cmd, groups, 1, 1);
	};
	sortStep (sortLocalPipe, 0, 0, n / SORT_TILE);
	for (uint32_t k = 2 * SORT_TILE; k <= n; k <<= 1) {
		for (uint32_t j = k >> 1; j >= SORT_TILE; j >>= 1) sortStep (sortPipe, j, k, n / 256);
		sortStep (sortLocalPipe, 1, k, n / SORT_TILE);
	}
	barrier (CS, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT |
		VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT); // (nearVol read back)
	// (the list's length read back: the next sort's size)
	VkBufferCopy bc{ 0, 0, 4 * sizeof(uint32_t) };
	vkCmdCopyBuffer (cmd, drawArgs[fi].buffer, drawRead[fi].buffer, 1, &bc);
	barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	drawValid[fi] = true;
	ctx->GpuMark ("pt sort");
	LogStats (simt);
}

void ParticleSystem::Draw (uint32_t first, uint32_t capacity, VkTexture *tex, bool emissive, VkDescriptorSet frameSet)
{
	if (!enabled || !prepassView || !tex || idle) return;
	const ParticlePool::Ring *r = slots.RingAt (first);
	if (!r || r->capacity != capacity) return;
	if (!r->retired) ringLook[first / BLOCK] = RingLook{ tex, emissive }; // (as drawn: when retired)
	// (registered: DrawSprites draws all rings' particles together)
	uint32_t *ri = (uint32_t*)ringInfoBuf[ctx->FrameIndex ()].mapped;
	ri[first / BLOCK] = TexSlot (tex) | (emissive ? 0x100u : 0u) | (tex == smokeTex ? 0x200u : 0u) | 0x80000000u;
	drawPending = true;
}

uint32_t ParticleSystem::TexSlot (VkTexture *tex)
{
	if (tex == smokeTex) return 0;
	for (uint32_t k = 1; k < TEX_SLOTS; k++)
		if (slotTex[k] == tex) { slotFrame[k] = frameNo; return k; }
	// a free slot, or the one unused for longest (not by a frame in flight)
	uint32_t best = 0;
	for (uint32_t k = 1; k < TEX_SLOTS; k++) {
		if (!slotTex[k]) { best = k; break; }
		if (frameNo - slotFrame[k] > vkc::FRAMES_IN_FLIGHT + 1 && (!best || slotFrame[k] < slotFrame[best])) best = k;
	}
	if (!best) return 0; // (all in use: drawn with the smoke texture)
	if (slotTex[best]) texmgr->Release (slotTex[best]);
	texmgr->AddRef (tex);
	slotTex[best] = tex, slotFrame[best] = frameNo;
	return best;
}

void ParticleSystem::DrawSprites (VkDescriptorSet frameSet)
{
	if (!enabled || !prepassView || idle || !drawPending) return;
	uint32_t fi = ctx->FrameIndex ();
	// the texture table of this frame slot (not bound before in this frame)
	VkDescriptorImageInfo ii[TEX_SLOTS];
	VkWriteDescriptorSet w[TEX_SLOTS];
	uint32_t nw = 0;
	for (uint32_t k = 0; k < TEX_SLOTS; k++)
		if (slotTex[k] && setTex[fi][k] != slotTex[k]) {
			ii[nw] = { texSampler, slotTex[k]->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			w[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			w[nw].dstSet = texSets[fi], w[nw].dstBinding = 0, w[nw].dstArrayElement = k, w[nw].descriptorCount = 1;
			w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[nw].pImageInfo = &ii[nw];
			setTex[fi][k] = slotTex[k];
			nw++;
		}
	if (nw) vkUpdateDescriptorSets (ctx->device, nw, w, 0, nullptr);
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawBlend);
	VkDescriptorSet s[3] = { frameSet, sets[fi], texSets[fi] };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 3, s, 0, nullptr);
	uint32_t flags = (smokeMode == SMOKE_SCREEN || (smokeMode == SMOKE_VOLUME && volAlloc) ? 8u : 0u) | (smokeMode == SMOKE_VOLUME && volActive && volAlloc ? 32u : 0u)
		| (fireballOcc && fbOccView ? 64u : 0u) | (plumeLayer && plLightView ? 128u : 0u);
	struct { uint32_t count, flags; float alpha, pad; float body[12]; float camVel[4]; } pc{ sortN[fi], flags, 1.0f, 0.0f, {}, {} };
	memcpy (pc.body, drawBody, sizeof(drawBody));
	memcpy (pc.camVel, drawCamVel, sizeof(drawCamVel));
	vkCmdPushConstants (cmd, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndirect (cmd, drawArgs[fi].buffer, 0, 1, 4 * sizeof(uint32_t));
	drawPending = false;
}

bool ParticleSystem::VolumeFog (VkImageView &view, VkSampler &sampler) const
{
	if (!enabled || smokeMode != SMOKE_VOLUME || !volActive || !volAlloc || idle || !volInteg.view) return false;
	view = volInteg.view, sampler = volSampler;
	return true;
}

void ParticleSystem::SetFireballOcclusion (VkImageView view)
{
	// (each slot's set written at its next Simulate, before it is bound:
	// no idle wait - the fireballs' images come and go within a frame)
	if (view == fbOccView) return;
	fbOccView = view;
	for (bool &d : fbOccDirty) d = true;
}

void ParticleSystem::SetPlumeLayer (VkImageView light, VkImageView dist)
{
	// (none: the prepass, a view of the same kind - not sampled then)
	if (plSet && light == plLightView && dist == plDistView) return;
	if (!prepassView && !light) return;
	if (plSet) ctx->WaitIdle (); // (the sets may be in use)
	plLightView = light, plDistView = dist, plSet = true;
	VkDescriptorImageInfo ii[2] = { { volSampler, light ? light : prepassView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ volSampler, light ? dist : prepassView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	for (VkDescriptorSet st : sets) {
		VkWriteDescriptorSet w[2]{};
		for (int k = 0; k < 2; k++) {
			w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w[k].dstSet = st, w[k].dstBinding = 53 + k, w[k].descriptorCount = 1;
			w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[k].pImageInfo = &ii[k];
		}
		vkUpdateDescriptorSets (ctx->device, 2, w, 0, nullptr);
	}
}

void ParticleSystem::WriteFireballOcclusion (uint32_t slot)
{
	fbOccDirty[slot] = false;
	if (!fbOccView) return;
	VkDescriptorImageInfo ii{ volSampler, fbOccView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = sets[slot], w.dstBinding = 48, w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = &ii;
	vkUpdateDescriptorSets (ctx->device, 1, &w, 0, nullptr);
}

void ParticleSystem::DrawVolume (VkDescriptorSet frameSet)
{
	// (the half-resolution march of RenderScreenSmoke, upsampled over the scene)
	if (!enabled || smokeMode != SMOKE_VOLUME || !prepassView || idle || !marchReady) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, volMarchPipe);
	VkDescriptorSet s[2] = { frameSet, sets[ctx->FrameIndex ()] };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, volMarchLayout, 0, 2, s, 0, nullptr);
	vkCmdDraw (cmd, 3, 1, 0, 0);
	ctx->GpuMark ("pt march");
}

void ParticleSystem::RenderMarch (VkDescriptorSet frameSet, const MATRIX3 &camRot, const float *proj)
{
	marchReady = false;
	if (!enabled || smokeMode != SMOKE_VOLUME || !prepassView || idle || !volActive || !volAlloc || marchGen != ctx->SwapchainGeneration ()) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2 (cmd, &dep);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchPipe);
	VkDescriptorSet s[3] = { sets[ctx->FrameIndex ()], frameSet, lastAtm };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, updateLayout, 0, 3, s, 0, nullptr);
	// (its history: the last frame's march, if there was one, reprojected -
	// the smoke moves with the ground (the air), not with the global frame
	// (a kilometre and a half a step near Earth): a point in this view
	// carried back to the last's, x = R p (camera-relative now), x_l = Q^T
	// (x - T) (FrameState carry, last march -> now): p' = Rl^T Q^T R p -
	// Rl^T Q^T T.
	// In the parameters the march does not use: lightX..Z rows of that
	// rotation, w the shift; trailNoise the last projection's scales, its
	// share kept (0: none), the march's count (its rays' and steps' offsets))
	UpdateParams up;
	memcpy (&up, lastUp, sizeof(up));
	const vkframe::Carry carry = vkframe::CarryBetween (marchSnap, lastSnap); // (lastSnap: this frame's, Simulate)
	bool hist = marchFrame != 0 && marchFrame + 1 == frameNo && carry.continuous;
	VECTOR3 t = -tmul (marchRot, tmul (carry.Q, carry.T));
	MATRIX3 qr; // Q^T R
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double m = 0.0;
			for (int k = 0; k < 3; k++) m += carry.Q.data[3 * k + i] * camRot.data[3 * k + j];
			qr.data[3 * i + j] = m;
		}
	float *rows[3] = { up.lightX, up.lightY, up.lightZ };
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double m = 0.0;
			for (int k = 0; k < 3; k++) m += marchRot.data[3 * k + i] * qr.data[3 * k + j];
			rows[i][j] = (float)m;
		}
		rows[i][3] = (float)(&t.x)[i];
	}
	up.trailNoise[0] = marchProj[0], up.trailNoise[1] = marchProj[1];
	up.trailNoise[2] = hist ? 1.0f : 0.0f, up.trailNoise[3] = (float)(marchCount++ & 1023u);
	marchRot = camRot, marchSnap = lastSnap, marchProj[0] = proj[0], marchProj[1] = proj[5], marchFrame = frameNo;
	vkCmdPushConstants (cmd, updateLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpdateParams), &up);
	vkCmdDispatch (cmd, (marchExtent.width + 7) / 8, (marchExtent.height + 7) / 8, 1);
	mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	vkCmdPipelineBarrier2 (cmd, &dep);
	ctx->GpuMark ("pt march half");
	marchReady = true;
}

void ParticleSystem::EnsureScreenSmoke ()
{
	if (volAlloc && marchGen != ctx->SwapchainGeneration ()) {
		ctx->WaitIdle (); // (the swapchain was made again: idle anyway)
		AllocMarch (VK_NULL_HANDLE);
	}
	if (smokeMode != SMOKE_SCREEN) return;
	VkExtent2D re = ctx->RenderExtent ();
	VkExtent2D e = { std::max (1u, re.width / 2), std::max (1u, re.height / 2) };
	if (thickGen != ctx->SwapchainGeneration ()) {
		ctx->WaitIdle ();
		for (auto &t : thick) ctx->DestroyImage (t);
		for (auto &t : thick) {
			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
			ici.extent = { e.width, e.height, 1 };
			ici.mipLevels = 1, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_OPTIMAL;
			ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			VmaAllocationCreateInfo aci{};
			aci.usage = VMA_MEMORY_USAGE_AUTO;
			VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &t.image, &t.alloc, nullptr));
			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = t.image, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = ici.format;
			vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &t.view));
		}
		VkImage a = thick[0].image, b = thick[1].image;
		ctx->ImmediateSubmit ([a, b](VkCommandBuffer cmd) {
			vkc::ImageBarrier (cmd, a, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			vkc::ImageBarrier (cmd, b, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
		});
		// blur sets: 0 reads a (shader-read) and writes b; 1 reads b and writes a (general)
		VkDescriptorImageInfo ii[4] = { { volSampler, thick[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { VK_NULL_HANDLE, thick[1].view, VK_IMAGE_LAYOUT_GENERAL },
			{ volSampler, thick[1].view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, thick[0].view, VK_IMAGE_LAYOUT_GENERAL } };
		VkWriteDescriptorSet wr[4]{};
		for (int k = 0; k < 4; k++) {
			wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			wr[k].dstSet = blurSets[k / 2], wr[k].dstBinding = k & 1, wr[k].descriptorCount = 1;
			wr[k].descriptorType = (k & 1) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr[k].pImageInfo = &ii[k];
		}
		vkUpdateDescriptorSets (ctx->device, 4, wr, 0, nullptr);
		VkDescriptorImageInfo ti{ volSampler, thick[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		for (VkDescriptorSet st : sets) {
			VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w.dstSet = st, w.dstBinding = 19, w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = &ti;
			vkUpdateDescriptorSets (ctx->device, 1, &w, 0, nullptr);
		}
		thickGen = ctx->SwapchainGeneration ();
	}
}

void ParticleSystem::RenderScreenSmoke (VkDescriptorSet frameSet, const MATRIX3 &camRot, const float *proj)
{
	RenderMarch (frameSet, camRot, proj);
	if (!enabled || smokeMode != SMOKE_SCREEN || !prepassView || idle || thickGen != ctx->SwapchainGeneration ()) return;
	VkExtent2D re = ctx->RenderExtent ();
	VkExtent2D e = { std::max (1u, re.width / 2), std::max (1u, re.height / 2) };
	// thickness of the volumetric particles
	ctx->BindTarget ({ thick[0].image, thick[0].view, VK_FORMAT_R16G16B16A16_SFLOAT, e });
	VkCommandBuffer cmd = ctx->Cmd ();
	VkClearAttachment ca{ VK_IMAGE_ASPECT_COLOR_BIT, 0, {} };
	VkClearRect cr{ { { 0, 0 }, e }, 0, 1 };
	vkCmdClearAttachments (cmd, 1, &ca, 1, &cr);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, thickPipe);
	uint32_t fi = ctx->FrameIndex ();
	VkDescriptorSet s[2] = { frameSet, sets[fi] };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, thickLayout, 0, 2, s, 0, nullptr);
	// (the sprites' list: its volumetric particles)
	struct { uint32_t count, flags; float alpha, pad; float body[12]; float camVel[4]; } pc{ sortN[fi], 16u, 1.0f, 0.0f, {}, {} };
	memcpy (pc.body, drawBody, sizeof(drawBody));
	memcpy (pc.camVel, drawCamVel, sizeof(drawCamVel));
	vkCmdPushConstants (cmd, thickLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndirect (cmd, drawArgs[fi].buffer, 0, 1, 4 * sizeof(uint32_t));
	ctx->EndTarget ();
	// blur: horizontal (a -> b), vertical (b -> a)
	auto barrier = [cmd](VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA) {
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = src, mb.srcAccessMask = srcA, mb.dstStageMask = dst, mb.dstAccessMask = dstA;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier (CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, CS, VK_ACCESS_2_SHADER_WRITE_BIT);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipe);
	int32_t bp[4] = { 1, 0, (int32_t)e.width, (int32_t)e.height };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurLayout, 0, 1, &blurSets[0], 0, nullptr);
	vkCmdPushConstants (cmd, blurLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bp), bp);
	vkCmdDispatch (cmd, (e.width + 7) / 8, (e.height + 7) / 8, 1);
	vkc::ImageBarrier (cmd, thick[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		CS, VK_ACCESS_2_SHADER_READ_BIT, CS, VK_ACCESS_2_SHADER_WRITE_BIT);
	barrier (CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_READ_BIT);
	bp[0] = 0, bp[1] = 1;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurLayout, 0, 1, &blurSets[1], 0, nullptr);
	vkCmdPushConstants (cmd, blurLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bp), bp);
	vkCmdDispatch (cmd, (e.width + 7) / 8, (e.height + 7) / 8, 1);
	vkc::ImageBarrier (cmd, thick[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		CS, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_SHADER_READ_BIT);
}

void ParticleSystem::DrawScreenSmoke (VkDescriptorSet frameSet)
{
	if (!enabled || idle || smokeMode != SMOKE_SCREEN || thickGen != ctx->SwapchainGeneration ()) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, screenCompPipe);
	VkDescriptorSet s[2] = { frameSet, sets[ctx->FrameIndex ()] };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, volMarchLayout, 0, 2, s, 0, nullptr);
	vkCmdPushConstants (cmd, volMarchLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(volNoise), volNoise);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}
