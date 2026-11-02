// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Zodiacal.h"
#include "GalaxySky.h"
#include "GalacticFrame.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "zodiacal.comp.h"

namespace {
	constexpr double AU_PER_PC = 206264.806247;
	// the zodiacal light 90 deg from the Sun in the ecliptic, at 1 AU [S10 at
	// 500 nm] (Levasseur-Regourd & Dumont 1980; Leinert et al. 1998)
	constexpr double REF_S10 = 202.0;
	// redder than sunlight: per band, (wavelength / 550 nm)^k (Leinert et al.
	// 1998: B - V about 0.1 redder at 90 deg from the Sun)
	constexpr double REDDENING = 0.4;
	constexpr double MOVE = 0.002;  // relative change of the observer's place that computes the light again
	constexpr double MOVE_MIN = 1e-4; // [AU]
	// the zodiacal cloud's fractional luminosity (its dust's share of the
	// Sun's light; Nesvorny et al. 2010: about 1e-7): debris rings' density
	// in the model's units by theirs
	constexpr double ZOD_FRACTION = 1e-7;
	constexpr double RING_HR = 0.05; // a ring's thickness (Gaussian sigma) per its radius

	// the model, as zodiacal.glsl
	const double ALPHA = 1.34, BETA = 4.14, GAMMA = 0.942, MU = 0.189;
	const double INCL = 2.03 * 3.14159265358979323846 / 180.0, NODE = 77.7 * 3.14159265358979323846 / 180.0;
	const double OFFSET[3] = { 0.0119, 0.00548, -0.00215 };

	double Density (const double x[3])
	{
		double q[3] = { x[0] - OFFSET[0], x[1] - OFFSET[1], x[2] - OFFSET[2] };
		double R = std::max (std::sqrt (q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), 1e-3);
		double z = q[0] * std::sin (NODE) * std::sin (INCL) - q[1] * std::cos (NODE) * std::sin (INCL) + q[2] * std::cos (INCL);
		double zeta = std::fabs (z) / R;
		double g = zeta < MU ? zeta * zeta / (2.0 * MU) : zeta - 0.5 * MU;
		return std::pow (R, -ALPHA) * std::exp (-BETA * std::pow (g, GAMMA));
	}

	double Phase (double c)
	{
		const double g[3] = { 0.7, -0.2, -0.81 }, w[3] = { 0.665, 0.330, 0.005 };
		double p = 0.0;
		for (int k = 0; k < 3; k++) {
			double d = 1.0 + g[k] * g[k] - 2.0 * g[k] * c;
			p += w[k] * (1.0 - g[k] * g[k]) / (d * std::sqrt (d));
		}
		return p / (4.0 * 3.14159265358979323846);
	}

	double ZodLight (const double o[3], const double d[3], int n)
	{
		const double SMAX = 8.0;
		double tc = -(o[0] * d[0] + o[1] * d[1] + o[2] * d[2]);
		double p[3] = { o[0] + d[0] * tc, o[1] + d[1] * tc, o[2] + d[2] * tc };
		double b = std::max (std::sqrt (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), 0.004);
		double s1 = std::sqrt (o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) + SMAX;
		double a0 = std::atan ((0.0 - tc) / b), a1 = std::atan ((s1 - tc) / b), da = (a1 - a0) / n, I = 0.0;
		for (int i = 0; i < n; i++) {
			double s = tc + b * std::tan (a0 + (i + 0.5) * da);
			double ds = da * (b * b + (s - tc) * (s - tc)) / b;
			double x[3] = { o[0] + d[0] * s, o[1] + d[1] * s, o[2] + d[2] * s };
			double r2 = std::max (x[0] * x[0] + x[1] * x[1] + x[2] * x[2], 1e-12);
			I += Density (x) * Phase (-(x[0] * d[0] + x[1] * d[1] + x[2] * d[2]) / std::sqrt (r2)) / r2 * ds;
		}
		return I;
	}

	void CubeBarrier (VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
	{
		VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = srcStage, b.srcAccessMask = srcAccess, b.dstStageMask = dstStage, b.dstAccessMask = dstAccess;
		b.oldLayout = from, b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image, b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.imageMemoryBarrierCount = 1, dep.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2 (cmd, &dep);
	}

	// a direction from an observer at (1, 0, 0) AU: dl from the Sun in
	// ecliptic longitude, at ecliptic latitude beta [deg]
	void Dir (double dl, double beta, double d[3])
	{
		const double r = 3.14159265358979323846 / 180.0;
		d[0] = -std::cos (beta * r) * std::cos (dl * r);
		d[1] = std::cos (beta * r) * std::sin (dl * r);
		d[2] = std::sin (beta * r);
	}
}

void Zodiacal::Init (vkc::Context *c, const GalaxySky &sky)
{
	ctx = c;
	enabled = sky.Settings ().zodiacal;
	// Galactic -> ecliptic (right-handed: Orbiter's x, z, y)
	double G[3][3];
	galactic::EclipticToGalactic (G);
	for (int j = 0; j < 3; j++) {
		eclFromGal[0][j] = G[j][0];
		eclFromGal[1][j] = G[j][2];
		eclFromGal[2][j] = G[j][1];
	}
	// the scale, at 1 AU, 90 deg from the Sun in the ecliptic
	const double o[3] = { 1.0, 0.0, 0.0 };
	double d[3];
	Dir (90.0, 0.0, d);
	scale = REF_S10 / ZodLight (o, d, 4096);
	{
		// (the model against the measured light at 1 AU [S10]: Leinert et
		// al. 1998, table 16)
		struct { double dl, beta, s10; } ref[] = { { 30, 0, 1940 }, { 60, 0, 394 }, { 120, 0, 147 }, { 150, 0, 140 },
			{ 180, 0, 180 }, { 90, 30, 103 }, { 90, 60, 68 }, { 0, 90, 63 } };
		char buf[512];
		int len = snprintf (buf, sizeof(buf), "VulkanClient: zodiacal light at 1 AU (model / measured, S10):");
		for (auto &r : ref) {
			Dir (r.dl, r.beta, d);
			double m = scale * ZodLight (o, d, 4096);
			len += snprintf (buf + len, sizeof(buf) - len, " (%.0f, %.0f) %.0f/%.0f", r.dl, r.beta, m, r.s10);
			deviation = std::max (deviation, std::abs (m / r.s10 - 1.0));
		}
		oapiWriteLog (buf);
	}
	// (the cloud's absorbed share per density unit: the density integrated
	// over all directions and out to 5 AU, / 4 pi, as the rings' pi sigma h)
	{
		double K = 0.0;
		const int NR = 400, NT = 90, NP = 36;
		for (int i = 0; i < NR; i++) {
			double R = 0.01 + (i + 0.5) * (5.0 - 0.01) / NR, dR = (5.0 - 0.01) / NR;
			for (int j = 0; j < NT; j++) {
				double th = (j + 0.5) * 3.14159265358979323846 / NT;
				for (int k = 0; k < NP; k++) {
					double ph = (k + 0.5) * 2.0 * 3.14159265358979323846 / NP;
					double x[3] = { R * std::sin (th) * std::cos (ph), R * std::sin (th) * std::sin (ph), R * std::cos (th) };
					K += Density (x) * std::sin (th) * dR * (3.14159265358979323846 / NT) * (2.0 * 3.14159265358979323846 / NP);
				}
			}
		}
		kZod = K / (4.0 * 3.14159265358979323846);
	}
	const double (*bands)[2] = sky.Bands ();
	for (int b = 0; b < GalaxySky::NBAND; b++)
		colour[b] = (float)std::pow (0.5 * (bands[b][0] + bands[b][1]) / 550.0, REDDENING);

	// the cube
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_R16G16B16A16_SFLOAT, ici.extent = { SIZE, SIZE, 1 };
	ici.mipLevels = 1, ici.arrayLayers = 6, ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &image, &alloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = image, vci.format = ici.format, vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
	vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &view));
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &arrayView));

	// the pipeline
	VkDescriptorSetLayoutBinding lb{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 1, lci.pBindings = &lb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &layout));
	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 1, pci.poolSizeCount = 1, pci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &layout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));
	VkDescriptorImageInfo ii{ VK_NULL_HANDLE, arrayView, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = set, w.dstBinding = 0, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w.pImageInfo = &ii;
	vkUpdateDescriptorSets (ctx->device, 1, &w, 0, nullptr);
	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 128 };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1, plci.pSetLayouts = &layout, plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &pl));
	VkShaderModule cs = ctx->CreateShaderModule (spv_zodiacal_comp, spv_zodiacal_comp_size);
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
	cpi.layout = pl;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);

	// (none until the first update: black)
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		CubeBarrier (cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkClearColorValue black{{ 0.0f, 0.0f, 0.0f, 0.0f }};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
		vkCmdClearColorImage (cmd, image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
		CubeBarrier (cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
}

void Zodiacal::Shutdown ()
{
	if (!ctx) return;
	vkDestroyPipeline (ctx->device, pipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, pl, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, layout, nullptr);
	vkDestroyImageView (ctx->device, view, nullptr);
	vkDestroyImageView (ctx->device, arrayView, nullptr);
	if (image) vmaDestroyImage (ctx->allocator, image, alloc);
	image = VK_NULL_HANDLE;
	ctx = nullptr;
}

void Zodiacal::Update (const double pos[3], bool now)
{
	if (!enabled) return;
	// the observer relative to the Sun, ecliptic [AU]
	double au[3];
	for (int i = 0; i < 3; i++)
		au[i] = (eclFromGal[i][0] * pos[0] + eclFromGal[i][1] * pos[1] + eclFromGal[i][2] * pos[2]) * AU_PER_PC;
	double dx = au[0] - last[0], dy = au[1] - last[1], dz = au[2] - last[2];
	double r = std::sqrt (au[0] * au[0] + au[1] * au[1] + au[2] * au[2]);
	bool moved = std::sqrt (dx * dx + dy * dy + dz * dz) >= std::max (MOVE * r, MOVE_MIN);
	// (a ring: moved relative to its star, or another ring)
	if ((ring.fraction > 0.0) != (lastRing.fraction > 0.0) || ring.radius != lastRing.radius) moved = true;
	else if (ring.fraction > 0.0) {
		double rx = ring.obs[0] - lastRing.obs[0], ry = ring.obs[1] - lastRing.obs[1], rz = ring.obs[2] - lastRing.obs[2];
		double rr = std::sqrt (ring.obs[0] * ring.obs[0] + ring.obs[1] * ring.obs[1] + ring.obs[2] * ring.obs[2]);
		if (std::sqrt (rx * rx + ry * ry + rz * rz) >= std::max (MOVE * rr, 0.01)) moved = true;
	}
	if (!moved) return;
	for (int i = 0; i < 3; i++) last[i] = au[i];
	lastRing = ring;
	if (now) ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) { Compute (cmd); });
	else {
		ctx->EndTarget ();
		Compute (ctx->Cmd ());
	}
}

void Zodiacal::Compute (VkCommandBuffer cmd)
{
	const double *au = last;
	struct { float obs[4], rows[3][4], colour[4], ringObs[4], ringAxis[4], ringPrm[4]; } p{};
	for (int i = 0; i < 3; i++) {
		p.obs[i] = (float)au[i];
		for (int j = 0; j < 3; j++) p.rows[i][j] = (float)eclFromGal[i][j];
	}
	// a ring: its density in the model's units by its fractional luminosity
	// against the cloud's (its share of the light absorbed: pi sigma h per
	// density unit), lit by its star
	const Ring &rg = lastRing;
	if (rg.fraction > 0.0 && rg.radius > 0.0) {
		double sigma = rg.width / 2.3548, h = RING_HR * rg.radius;
		double n0 = rg.fraction / ZOD_FRACTION * kZod / (3.14159265358979323846 * sigma * RING_HR);
		for (int i = 0; i < 3; i++) p.ringObs[i] = (float)rg.obs[i], p.ringAxis[i] = (float)rg.axis[i];
		p.ringObs[3] = (float)(n0 * rg.lum);
		p.ringAxis[3] = (float)rg.radius;
		p.ringPrm[0] = (float)sigma, p.ringPrm[1] = (float)h;
	}
	p.obs[3] = (float)scale;
	p.rows[0][3] = (float)SIZE;
	for (int b = 0; b < 4; b++) p.colour[b] = colour[b];
	CubeBarrier (cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, SIZE / 8, SIZE / 8, 6);
	CubeBarrier (cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}
