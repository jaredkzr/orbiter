// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Nebulae.h"
#include "GalaxySky.h"
#include "StarCatalogue.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "nebula_front.comp.h"

namespace {
	constexpr int NDIR = 16 * 16;          // front table entries per source (as nebula.glsl)
	constexpr int TILE = 32;               // texels per tile side (as nebula.glsl)
	constexpr double LSUN = 3.828e33;      // [erg/s]
	constexpr double PC3_CM3 = 2.9380e55;  // [cm^3 per pc^3]
	constexpr double ALPHA_B = 2.59e-13;   // case B recombination coefficient at 10^4 K [cm^3/s]
	constexpr double HALPHA = 3.54e-25;    // H-alpha emissivity per n_e n_p at 10^4 K [erg cm^3/s] (case B)
	constexpr double MIN_LOGQ = 45.0;      // [photons/s]: weaker sources ionise no visible region
	constexpr double MAX_REACH = 60.0;     // [pc]: of a star's light on the dust
	constexpr double LOCAL_MIN_EXCESS = 0.2; // [mag]: of a star's extinction over the map's, for dust about it
	constexpr double MIN_ANGLE = 0.004;    // [rad]: lit spheres looking smaller are left out of a bake (not hot stars)
	constexpr int NLOGT = 256;
	constexpr double LOGT0 = 3.2, LOGT1 = 4.75;
	constexpr double PI_ = 3.14159265358979323846;

	struct GpuSource { float pos[4], light[4], line[4], excite[4], misc[4], ion[4]; };

	// ionising photons per solar luminosity, log10, by temperature: model
	// atmospheres of O stars (Martins et al. 2005) and B stars (Sternberg
	// et al. 2003), approximately
	double LogQperL (double T)
	{
		static const double tab[][2] = { { 18000, 39.8 }, { 20000, 40.5 }, { 22000, 41.0 }, { 25000, 41.7 }, { 28000, 42.5 },
			{ 30000, 43.0 }, { 32000, 43.35 }, { 35000, 43.65 }, { 40000, 43.8 }, { 45000, 43.85 }, { 50000, 43.9 } };
		const int n = sizeof(tab) / sizeof(tab[0]);
		if (T <= tab[0][0]) return tab[0][1] - 0.35 * (tab[0][0] - T) / 1000.0;
		for (int i = 1; i < n; i++)
			if (T < tab[i][0]) return tab[i-1][1] + (tab[i][1] - tab[i-1][1]) * (T - tab[i-1][0]) / (tab[i][0] - tab[i-1][0]);
		return tab[n-1][1];
	}

	double Smoothstep (double e0, double e1, double x)
	{
		double t = std::clamp ((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// The ionised gas's light per band per n_e^2 [L_sun,band / pc^3 / cm^-6]
	// at excitation x (0..1): the hydrogen lines (case B), the forbidden
	// lines (more highly excited gas: more doubly ionised oxygen, less singly
	// ionised nitrogen and sulphur; typical ratios of Galactic HII regions),
	// and the nebular continuum
	void LineEmission (const GalaxySky &sky, double x, float out[GalaxySky::NBAND])
	{
		double oiii = 0.35 * (0.075 + 1.425 * x); // [OIII] 5007 / H-alpha (H-beta: 0.35)
		struct Line { double nm, ratio; } lines[] = {
			{ 656.3, 1.0 }, { 658.4, 0.30 - 0.18 * x }, { 654.8, 0.10 - 0.06 * x }, { 672.0, 0.15 - 0.10 * x },
			{ 587.6, 0.01 + 0.025 * x }, { 486.1, 0.35 }, { 500.7, oiii }, { 495.9, oiii / 3.0 },
			{ 434.0, 0.164 }, { 410.2, 0.091 } };
		const double CONTINUUM = 0.0025; // per nm, of H-alpha
		const double (*bands)[2] = sky.Bands ();
		for (int b = 0; b < GalaxySky::NBAND; b++) {
			double p = CONTINUUM * (bands[b][1] - bands[b][0]);
			for (const Line &l : lines)
				if (l.nm >= bands[b][0] && l.nm < bands[b][1]) p += l.ratio;
			out[b] = (float)(p * HALPHA * PC3_CM3 / (LSUN * sky.SolarBandFraction (b)));
		}
	}

	// Radius [pc] of a part of the natal cloud of a star of ionising photon
	// budget Q / (4 pi alpha_B) [cm^-6 pc^3]: its density n0 (1 + r/r0)^-2
	// takes the share of the photons (the integral of n^2 r^2 dr is
	// n0^2 r0^3 / 3; as NebNatalRadius)
	double NatalRadius (double budget, double n0, double share)
	{
		return std::cbrt (3.0 * share * budget / (n0 * n0));
	}

	// Vulkan cube faces as axis + u, v vectors (FaceDir in sky_bake.comp)
	const double FACE[6][3][3] = {
		{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, -1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },  { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, -1, 0 } }, { { 0, 0, -1 }, { -1, 0, 0 }, { 0, -1, 0 } } };

	double Dot (const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
}

void Nebulae::Init (vkc::Context *c, const GalaxySky &sky, const galactic::StarCatalogue *cat)
{
	ctx = c;
	cubeSize = GalaxySky::SIZE;
	auto t0 = std::chrono::steady_clock::now ();
	const GalaxySky::NebParams &np = sky.Neb ();

	// tables by temperature: band light, bolometric light, of M_V = 0
	std::vector<float> bandT (NLOGT * GalaxySky::NBAND);
	std::vector<double> bolT (NLOGT);
	for (int t = 0; t < NLOGT; t++) {
		double T = std::pow (10.0, LOGT0 + (LOGT1 - LOGT0) * t / (NLOGT - 1));
		sky.BandLight (T, &bandT[t * GalaxySky::NBAND]);
		bolT[t] = sky.BolometricRatio (T);
	}
	auto tIndex = [](double T) {
		return std::clamp ((int)std::lround ((std::log10 (T) - LOGT0) / (LOGT1 - LOGT0) * (NLOGT - 1)), 0, NLOGT - 1);
	};

	// the line spectra of the least and the most excited gas (linear in
	// the excitation between)
	float lineLo[GalaxySky::NBAND], lineHi[GalaxySky::NBAND];
	LineEmission (sky, 0.0, lineLo);
	LineEmission (sky, 1.0, lineHi);

	// the sources
	std::vector<GpuSource> src;
	std::vector<float> ions;      // xyz, budget; natal cloud's radii, -, -
	std::vector<uint32_t> ionSrc; // source of each
	std::vector<float> locals;     // stars of known spectral type: xyz, -; -
	std::vector<uint32_t> localSrc; // their sources
	std::vector<double> localA;     // their extinction from the Sun (from their colour)
	if (cat) {
		for (size_t i = 0; i < cat->Count (); i++) {
			// (the cheap tests first: most of the catalogue's stars are neither)
			double mv = cat->AbsMag (i);
			bool spectral = cat->Flags (i) & galactic::StarCatalogue::SPECTRAL;
			if (mv > np.lightMV && !spectral) continue;
			double T = cat->Teff (i);
			bool hot = spectral && T >= 18000.0;
			if (mv > np.lightMV && !hot) continue;
			double p[3];
			cat->Position (i, p);
			double LV = std::pow (10.0, -0.4 * (mv - 4.83));
			int ti = tIndex (T);
			GpuSource s{};
			for (int k = 0; k < 3; k++) s.pos[k] = (float)p[k];
			s.pos[3] = (float)std::min (10.0 * std::pow (10.0, 0.2 * (np.reachMag - mv)), MAX_REACH);
			for (int b = 0; b < GalaxySky::NBAND; b++) s.light[b] = (float)(LV * bandT[ti * GalaxySky::NBAND + b]);
			s.misc[0] = -1.0f;
			if (hot) {
				double logQ = std::log10 (LV * bolT[ti]) + LogQperL (T);
				if (logQ >= MIN_LOGQ) {
					s.misc[0] = (float)(ions.size() / 8 * NDIR);
					// (the spectrum from the least excited gas to the most a star
					// of the temperature excites: the shader takes the excitation
					// from the gas's ionisation parameter)
					double x = Smoothstep (33000.0, 45000.0, T);
					for (int b = 0; b < GalaxySky::NBAND; b++)
						s.line[b] = lineLo[b], s.excite[b] = x * (lineHi[b] - lineLo[b]);
					s.ion[0] = (float)logQ;
					ionSrc.push_back ((uint32_t)src.size());
					for (int k = 0; k < 3; k++) ions.push_back ((float)p[k]);
					double budget = std::pow (10.0, logQ) / (4.0 * PI_ * ALPHA_B * PC3_CM3);
					ions.push_back ((float)budget);
					// (young stars in their natal clouds; evolved ones have
					// dispersed theirs)
					if (!(cat->Flags (i) & galactic::StarCatalogue::EVOLVED)) {
						s.misc[2] = (float)NatalRadius (budget, np.coreDensity, np.coreShare);
						s.misc[3] = (float)NatalRadius (budget, np.envDensity, np.envShare);
					}
					ions.insert (ions.end(), { s.misc[2], s.misc[3], 0.0f, 0.0f });
				}
			}
			if (mv > np.lightMV && s.misc[0] < 0.0f) continue;
			if (cat->Flags (i) & galactic::StarCatalogue::SPECTRAL) {
				localSrc.push_back ((uint32_t)src.size());
				localA.push_back (cat->ExtinctionFromSun (i));
				locals.insert (locals.end(), { (float)p[0], (float)p[1], (float)p[2], 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
			}
			src.push_back (s);
		}
	}
	uint32_t nIon = (uint32_t)(ions.size() / 8);

	// on the GPU, once: the ionisation fronts, and the dust map's
	// extinction from the Sun to the stars of known spectral type
	std::vector<float> front ((size_t)std::max (nIon, 1u) * NDIR, 0.0f);
	std::vector<float> mapExt (std::max (locals.size() / 8, (size_t)1), 0.0f);
	uint32_t nLocal = (uint32_t)(locals.size() / 8), nLit = 0;
	fronts = ctx->CreateBuffer (front.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, true);
	if (nIon || nLocal) {
		if (ions.empty ()) ions.resize (8, 0.0f);
		if (locals.empty ()) locals.resize (8, 0.0f);
		vkc::Buffer in[2] = { ctx->CreateDeviceBuffer (ions.data(), ions.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
			ctx->CreateDeviceBuffer (locals.data(), locals.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) };
		vkc::Buffer extBuf = ctx->CreateBuffer (mapExt.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
		VkBuffer out[2] = { fronts.buffer, extBuf.buffer };
		VkDescriptorSetLayoutBinding b[4] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)GalaxySky::DustLevels (), VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 4, lci.pBindings = b;
		VkDescriptorSetLayout layout;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &layout));
		VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (uint32_t)GalaxySky::DustLevels () } };
		VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pci.maxSets = 2, pci.poolSizeCount = 2, pci.pPoolSizes = ps;
		VkDescriptorPool pool;
		VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
		VkDescriptorSetLayout layouts[2] = { layout, layout };
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = pool, dai.descriptorSetCount = 2, dai.pSetLayouts = layouts;
		VkDescriptorSet sets[2];
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
		VkDescriptorImageInfo di[3];
		for (int l = 0; l < GalaxySky::DustLevels (); l++)
			di[l] = { sky.DustSampler (), sky.DustView (l), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		for (int k = 0; k < 2; k++) {
			VkDescriptorBufferInfo bi[3] = { { sky.ModelBuffer (), 0, VK_WHOLE_SIZE }, { in[k].buffer, 0, VK_WHOLE_SIZE },
				{ out[k], 0, VK_WHOLE_SIZE } };
			VkWriteDescriptorSet wr[4]{};
			for (int j = 0; j < 4; j++) {
				wr[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, wr[j].dstSet = sets[k], wr[j].dstBinding = j;
				wr[j].descriptorCount = 1, wr[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			}
			wr[0].pBufferInfo = &bi[0];
			wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[1].descriptorCount = GalaxySky::DustLevels (), wr[1].pImageInfo = di;
			wr[2].pBufferInfo = &bi[1];
			wr[3].pBufferInfo = &bi[2];
			vkUpdateDescriptorSets (ctx->device, 4, wr, 0, nullptr);
		}
		VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &layout;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VkPipelineLayout pl;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &pl));
		VkShaderModule cs = ctx->CreateShaderModule (spv_nebula_front_comp, spv_nebula_front_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = pl;
		VkPipeline pipe;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);

		vkc::Buffer rb = ctx->CreateReadbackBuffer ((front.size() + mapExt.size()) * sizeof(float));
		ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
			uint32_t job[2][2] = { { nIon, 0 }, { nLocal, 1 } }; // (count, mode)
			for (int k = 0; k < 2; k++) {
				if (!job[k][0]) continue;
				vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[k], 0, nullptr);
				vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, job[k]);
				vkCmdDispatch (cmd, (job[k][0] * (k ? 1 : NDIR) + 63) / 64, 1, 1);
			}
			VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
			mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			mb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
			VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
			vkCmdPipelineBarrier2 (cmd, &dep);
			VkBufferCopy r[2] = { { 0, 0, front.size() * sizeof(float) }, { 0, front.size() * sizeof(float), mapExt.size() * sizeof(float) } };
			vkCmdCopyBuffer (cmd, fronts.buffer, rb.buffer, 1, &r[0]);
			vkCmdCopyBuffer (cmd, extBuf.buffer, rb.buffer, 1, &r[1]);
		});
		vmaInvalidateAllocation (ctx->allocator, rb.alloc, 0, VK_WHOLE_SIZE);
		memcpy (front.data(), rb.mapped, front.size() * sizeof(float));
		memcpy (mapExt.data(), (const float*)rb.mapped + front.size(), mapExt.size() * sizeof(float));
		ctx->DestroyBuffer (rb);
		vkDestroyPipeline (ctx->device, pipe, nullptr);
		vkDestroyPipelineLayout (ctx->device, pl, nullptr);
		vkDestroyDescriptorPool (ctx->device, pool, nullptr);
		vkDestroyDescriptorSetLayout (ctx->device, layout, nullptr);
		for (auto &b : in) ctx->DestroyBuffer (b);
		ctx->DestroyBuffer (extBuf);

		// the dust close about a star the map misses (its own extinction from
		// its colour, beyond the map's): a local cloud it lights
		// (beyond ~2 sigma of the colours' and the intrinsic colours'
		// uncertainty)
		for (uint32_t k = 0; k < nLocal; k++) {
			double excess = localA[k] - mapExt[k];
			if (excess <= LOCAL_MIN_EXCESS) continue;
			nLit++;
			GpuSource &s = src[localSrc[k]];
			s.ion[1] = (float)(2.0 * excess); // (the shell's column from the star: half that)
			s.ion[2] = (float)np.localRadius;
		}
		// (the lit sphere holds the ionised region)
		bool log = getenv ("ORBITER_VK_SKY_LOG") != nullptr;
		for (uint32_t k = 0; k < nIon; k++) {
			float rMax = *std::max_element (front.begin() + (size_t)k * NDIR, front.begin() + (size_t)(k + 1) * NDIR);
			GpuSource &s = src[ionSrc[k]];
			s.pos[3] = std::max (s.pos[3], 1.05f * rMax);
			s.misc[1] = rMax;
			if (log && ions[k * 8 + 3] > 3e4) {
				std::vector<float> f (front.begin() + (size_t)k * NDIR, front.begin() + (size_t)(k + 1) * NDIR);
				std::sort (f.begin(), f.end());
				double l = std::atan2 (s.pos[1], s.pos[0]) * 180.0 / PI_, d = std::sqrt (s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1] + s.pos[2] * s.pos[2]);
				oapiWriteLogV ("VulkanClient: nebulae: l %.1f b %.1f d %.0f pc: log Q %.2f, front %.2f / %.2f / %.2f pc", l < 0 ? l + 360.0 : l,
					std::asin (s.pos[2] / d) * 180.0 / PI_, d, std::log10 (ions[k * 8 + 3] * 4.0 * PI_ * ALPHA_B * PC3_CM3), f[0], f[NDIR / 2], f[NDIR - 1]);
			}
		}
	}

	// for the binning
	size_t n = src.size();
	pos.resize (n * 3), radius.resize (n), ionising.resize (n);
	for (size_t i = 0; i < n; i++) {
		memcpy (&pos[i * 3], src[i].pos, 3 * sizeof(float));
		radius[i] = src[i].pos[3];
		ionising[i] = src[i].misc[0] >= 0.0f;
	}
	if (src.empty ()) src.resize (1);
	sources = ctx->CreateDeviceBuffer (src.data(), src.size() * sizeof(GpuSource), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
	oapiWriteLogV ("VulkanClient: nebulae: %zu stars light the dust, %u of them ionise the gas, %u light dust about them (%.0f ms)",
		n, nIon, nLit, ms);
}

void Nebulae::Shutdown ()
{
	if (!ctx) return;
	ctx->DestroyBuffer (sources);
	ctx->DestroyBuffer (fronts);
	for (auto &t : tiles) ctx->DestroyBuffer (t);
	ctx = nullptr;
}

bool Nebulae::Bin (const galactic::Observer &obs, int slot)
{
	const int T = (int)cubeSize / TILE, nTile = 6 * T * T;
	std::vector<std::vector<uint32_t>> lists (nTile);
	auto addRect = [&](int f, double u0, double u1, double v0, double v1, uint32_t i) {
		auto tile = [&](double u) { return std::clamp ((int)std::floor ((u * 0.5 + 0.5) * T), 0, T - 1); };
		int x0 = tile (u0), x1 = tile (u1), y0 = tile (v0), y1 = tile (v1);
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) lists[(f * T + y) * T + x].push_back (i);
	};
	// (tile centres, for the large spheres)
	std::vector<double> centre (nTile * 3);
	for (int f = 0; f < 6; f++)
		for (int y = 0; y < T; y++)
			for (int x = 0; x < T; x++) {
				double u = (x + 0.5) / T * 2.0 - 1.0, v = (y + 0.5) / T * 2.0 - 1.0, d[3], l = 0.0;
				for (int k = 0; k < 3; k++) l += (d[k] = FACE[f][0][k] + u * FACE[f][1][k] + v * FACE[f][2][k]) * d[k];
				for (int k = 0; k < 3; k++) centre[((f * T + y) * T + x) * 3 + k] = d[k] / std::sqrt (l);
			}
	const double TILE_RADIUS = 2.0 * std::atan (std::sqrt (2.0) / T); // (a tile's angular half-diagonal, at most)

	for (uint32_t i = 0; i < (uint32_t)radius.size(); i++) {
		double c[3];
		for (int k = 0; k < 3; k++) c[k] = pos[i * 3 + k] - obs.pos[k];
		double dist = std::sqrt (Dot (c, c)), R = radius[i];
		if (dist <= R * 1.02) { // (inside: every tile)
			for (int t = 0; t < nTile; t++) lists[t].push_back (i);
			continue;
		}
		double alpha = std::asin (R / dist) * 1.03;
		if (alpha < MIN_ANGLE && !ionising[i]) continue;
		double a[3] = { c[0] / dist, c[1] / dist, c[2] / dist };
		if (alpha > 0.35) {
			double lim = std::cos (std::min (alpha + TILE_RADIUS, PI_));
			for (int t = 0; t < nTile; t++)
				if (Dot (&centre[t * 3], a) >= lim) lists[t].push_back (i);
			continue;
		}
		// the cone's outline (16 points) and centre, on each face
		double e1[3], e2[3];
		double h[3] = { std::fabs (a[0]) < 0.9 ? 1.0 : 0.0, std::fabs (a[0]) < 0.9 ? 0.0 : 1.0, 0.0 };
		e1[0] = a[1] * h[2] - a[2] * h[1], e1[1] = a[2] * h[0] - a[0] * h[2], e1[2] = a[0] * h[1] - a[1] * h[0];
		double l1 = std::sqrt (Dot (e1, e1));
		for (double &v : e1) v /= l1;
		e2[0] = a[1] * e1[2] - a[2] * e1[1], e2[1] = a[2] * e1[0] - a[0] * e1[2], e2[2] = a[0] * e1[1] - a[1] * e1[0];
		double pts[17][3];
		double ca = std::cos (alpha), sa = std::sin (alpha);
		for (int k = 0; k < 16; k++) {
			double phi = k * PI_ / 8.0, cp = std::cos (phi), sp = std::sin (phi);
			for (int m = 0; m < 3; m++) pts[k][m] = ca * a[m] + sa * (cp * e1[m] + sp * e2[m]);
		}
		memcpy (pts[16], a, sizeof(a));
		for (int f = 0; f < 6; f++) {
			double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
			bool any = false;
			for (auto &p : pts) {
				double z = Dot (p, FACE[f][0]);
				if (z <= 1e-6) continue;
				double u = Dot (p, FACE[f][1]) / z, v = Dot (p, FACE[f][2]) / z;
				u0 = std::min (u0, u), u1 = std::max (u1, u), v0 = std::min (v0, v), v1 = std::max (v1, v);
				any = true;
			}
			if (!any || u1 < -1.0 || u0 > 1.0 || v1 < -1.0 || v0 > 1.0) continue;
			addRect (f, u0, u1, v0, v1, i);
		}
	}

	// flatten: the first entry of each tile (+ the end), then the entries
	size_t total = 0;
	for (auto &l : lists) total += l.size();
	bool replaced = false;
	if (!tiles[slot].buffer || total > tileCapacity[slot]) {
		if (tiles[slot].buffer) {
			vkDeviceWaitIdle (ctx->device);
			ctx->DestroyBuffer (tiles[slot]);
		}
		tileCapacity[slot] = total + total / 2 + 65536;
		tiles[slot] = ctx->CreateBuffer ((nTile + 1 + tileCapacity[slot]) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true);
		replaced = true;
	}
	uint32_t *out = (uint32_t*)tiles[slot].mapped;
	uint32_t at = nTile + 1;
	for (int t = 0; t < nTile; t++) {
		out[t] = at;
		memcpy (out + at, lists[t].data(), lists[t].size() * sizeof(uint32_t));
		at += (uint32_t)lists[t].size();
	}
	out[nTile] = at;
	vmaFlushAllocation (ctx->allocator, tiles[slot].alloc, 0, VK_WHOLE_SIZE);
	if (getenv ("ORBITER_VK_SKY_LOG"))
		oapiWriteLogV ("VulkanClient: nebulae: %zu tile entries from (%.1f, %.1f, %.1f) pc", total, obs.pos[0], obs.pos[1], obs.pos[2]);
	return replaced;
}
