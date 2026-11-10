// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Atmosphere.h"
#include "Clouds.h"
#include "Scene.h"
#include "VulkanClient.h"
#include "WaterFx.h"
#include "SurfaceQuery.h"
#include "ShoreField.h"
#include "OceanFft.h"
#include "VkTexture.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <random>

#include "sky.vert.h"
#include "sky.frag.h"
#include "atm_transmittance.comp.h"
#include "atm_multiscat.comp.h"

namespace {

	// Must match the uniform block in atmosphere.glsl
	constexpr int SALT_MAX = 24; // (atmosphere.glsl)
	struct AtmUniforms {
		float viewToPlanet[16];
		float camPos[4];
		float sunDir[4];
		float betaR[4];
		float betaM[4];
		float radii[4];
		float camC[4];
		float misc[4];
		float ground[4];
		float cloud[4];
		float water[4];
		float ozone[4];
		float detail[4];
		float aurora[4];
		float mieLobe[4];     // aerosols' forward lobe: asymmetry per colour (rgb), share (w; 0: none)
		float cloudMap[4];    // volumetric clouds (their shadows): coverage map centre, half size (w; 0: none)
		float cloudNoise[4];  // shape noise offset, scale
		float cloudShell[4];  // shell base, top radius, extinction, coverage scale
		float detailPrm[4];   // terrain detail: ridge, craters, crater depth, fresh craters
		float surfPrm[4];     // x: colour saturation of the surface textures
		float relBeta[4];     // relativistic view: the camera's velocity (view, units of c)
		float sunTint[4];     // the light's colour (luminance 1): the Sun white, a red dwarf orange
		float wavePrm[4];     // x: slope amplitude per wave (0: no waves), y: g, z: wavelength scale
		float waves[WAVE_COUNT][4]; // planet-frame wave vector k*dir (xyz), phase (w); longest first
		float waterFx[4];     // x: water disturbances packed (0: none)
		float waterFxSrc[WaterFx::MAX_GPU * WaterFx::VEC4][4]; // per source (WaterFx.h)
		float seaPrm[4];      // x: mean-square slope below the shortest wave, y: total mean-square slope (0: no sea state), z: U10 [m/s], w: whitecap coverage (G3; 0: none)
		float foamPrm[4];     // whitecaps: x: crest threshold (slope units), y: sigma of the crest index, z: far-field mean foam, w: mean of the foam lace
		float shorePrm[4];    // shore foam: x: band width [m] (0: none), y: bore spacing [m], z: bore phase [cycles], w: strength
		float pebShape[4];    // terrain detail pebbles (B1): size, round, elong, height
		float pebShape2[4];   //   bevel, tilt, crag, tone
		float pebPrm[4];      //   albedo factor, grit density, micro floor, Earth gate (0/1)
		float playaPrm[4];    // playa (B2): x on/off (1: cracks, the weight is per vertex); yzw reserved
		float sitePrm[4];     // site materials (F3): on (0/1), seed, edge, kpMax
		float siteThr[4];     //   thresholds: coarse, fine, flat
		float siteCover[4];   //   shares: coarse, fine, flat
		float siteMat[9][4];  //   per material (coarse, fine, flat): [3i] tint rgb, amount; [3i+1] abs rgb, grit factor; [3i+2] pebbles, cracks, smoothing, rocks
		float overcastSun[4]; // C3: s0 * OVERCAST.sky (rgb; 0: none), w 0
		float overcastPrm[4]; //   falloff k, deck altitude [m], tan(theta0), 0
		float coastPrm[4];    // shore (F1): x wet sand width [m] (0: none), y shallows tint (1: procedural seas), z mask field range [texels] (0: plain masks), w 0
		float waveEnv[WAVE_COUNT][4]; // the waves' crest envelopes (W2): planet-frame wave vector kp (xyz), phase (w)
		float oceanT1[4];     // the sea field (W3): t1 (xyz), long patch [m] (w)
		float oceanT2[4];     //   t2 (xyz), ripple patch [m] (w)
		float oceanD[4];      //   the camera from the anchor (xyz, planet frame), on (w: 1)
		float oceanVar[4];    //   mean-square slope of the layers (x, y)
		float oceanMipVar[2][4]; //   the long layer's levels 0..5
		float nearHaze[4];    // C4 (T4): haze near the ground: extinction at the surface [1/m] (rgb), scale height [m] (w; 0: none)
		float saltFlat[SALT_MAX][4]; // playa salt flats (pass 2, P1): planet-frame centre direction (xyz), angular radius [rad]; playaPrm.y of them
	};
	static_assert (sizeof (AtmUniforms) % 16 == 0, "std140");

	const VkFormat LUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	const VkExtent2D TRANSMITTANCE_SIZE = { 256, 64 }, MULTISCAT_SIZE = { 32, 32 }; // as in atmlut.glsl

	// water's surface tension over its density [m^3/s^2]: the capillary term of
	// the waves' dispersion, omega^2 = g k + SIGMA_RHO k^3
	constexpr double SIGMA_RHO = 7.28e-5;
	// share of the unresolved slope variance that goes into the glitter's
	// roughness (tunable 0.5-1: lower keeps the near glitter sparkling)
	constexpr double SEA_UNRES_GAIN = 1.0;

	// ---- whitecaps: the calibration's mirror of tile_main.glsl (keep in sync) ----
	// constants as CAP_* in tile_main.glsl (keep in sync)
	constexpr int CAP_WAVES = 6;          // waves in the crest index
	constexpr double CAP_EDGE = 3.0;      // edge noise amplitude [sigma] (the noise has a std of about 0.1)
	constexpr double CAP_WIDTH = 0.35;    // soft edge of a patch [sigma]
	constexpr double CAP_SPREAD = 0.3;    // threshold lowered per period of age [sigma]
	constexpr double CAP_TF = 0.4;        // foam e-folding time / T0
	constexpr double CAP_LIFE = 2.0;      // foam lives two periods
	constexpr double CAP_COVER_F = 0.3;   // a sample counts as whitecap where foam > this

	// bit-exact ports of tile_main.glsl's Hash and ValueNoise (keep in sync):
	// the same uint32 constants, xor-shifts and floor-based periodic wrap
	float NHash (int cx, int cy, int cz)
	{
		uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u ^ (uint32_t)cz * 83492791u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return (float)(h & 0xffffu) / 65535.0f;
	}
	float NMix (float a, float b, float t) { return a * (1.0f - t) + b * t; }
	float NValue (float px, float py, float pz, float cell, int period)
	{
		float qx = px / cell, qy = py / cell, qz = pz / cell;
		float fx = floorf (qx), fy = floorf (qy), fz = floorf (qz);
		int ix = (int)fx, iy = (int)fy, iz = (int)fz;
		float x = qx - fx, y = qy - fy, z = qz - fz;
		x = x * x * (3.0f - 2.0f * x); y = y * y * (3.0f - 2.0f * y); z = z * z * (3.0f - 2.0f * z);
		float n[8];
		for (int k = 0; k < 8; k++) {
			int cx = ix + (k & 1), cy = iy + ((k >> 1) & 1), cz = iz + (k >> 2);
			cx -= period * (int)floorf ((float)cx / (float)period);
			cy -= period * (int)floorf ((float)cy / (float)period);
			cz -= period * (int)floorf ((float)cz / (float)period);
			n[k] = NHash (cx, cy, cz);
		}
		return NMix (NMix (NMix (n[0], n[1], x), NMix (n[2], n[3], x), y),
		             NMix (NMix (n[4], n[5], x), NMix (n[6], n[7], x), y), z);
	}
	float NSmooth (float a, float b, float x)
	{
		float t = std::min (std::max ((x - a) / (b - a), 0.0f), 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}
	// ragged edges of the foam patches (full resolution: footprint 0)
	float CapEdgeNoise (float x, float y, float z)
	{
		return 0.45f * NValue (x + 9.7f, y + 1.9f, z + 4.1f, 8.0f, 125) + 0.35f * NValue (x, y, z, 2.0f, 500)
			+ 0.2f * NValue (x + 17.1f, y + 3.7f, z + 9.3f, 0.5f, 2000) - 0.5f;
	}
	// filaments of the foam (full resolution)
	float FoamLace (float x, float y, float z)
	{
		float qx = x + (NValue (x, y, z, 2.0f, 500) - 0.5f);
		float qy = y + (NValue (x + 11.7f, y + 5.3f, z + 2.9f, 2.0f, 500) - 0.5f);
		float qz = z + (NValue (x + 3.1f, y + 7.9f, z + 13.3f, 2.0f, 500) - 0.5f);
		float l1 = NSmooth (0.55f, 0.9f, 1.0f - fabsf (2.0f * NValue (qx, qy, qz, 1.0f, 1000) - 1.0f));
		float l2 = NSmooth (0.55f, 0.9f, 1.0f - fabsf (2.0f * NValue (qx + 5.3f, qy + 9.1f, qz + 2.2f, 0.4f, 2500) - 1.0f));
		float fil = 1.0f - (1.0f - l1) * (1.0f - 0.7f * l2);
		return 0.25f + 0.75f * fil;
	}
	float FoamA (float x, float y, float z)
	{
		return NMix (0.7f, 1.0f, NValue (x + 13.3f, y + 6.1f, z + 2.7f, 2.5f, 400));
	}
	float FoamLA (float x, float y, float z)
	{
		return NMix (0.75f, 1.0f, NValue (x + 5.9f, y + 1.3f, z + 7.7f, 1.0f, 1000));
	}
	double SmoothStep (double a, double b, double x)
	{
		double t = std::min (std::max ((x - a) / (b - a), 0.0), 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// Config/VK/Sea.cfg: the wind speed [m/s] a body's sea takes instead of the
	// model's ([name] sections, "Wind = v"); -1: no entry. Read once.
	double SeaCfgWind (const char *body)
	{
		static std::map<std::string, double> entries;
		static bool loaded = false;
		if (!loaded) {
			loaded = true;
			std::ifstream ifs ("Config/VK/Sea.cfg");
			std::string line, section;
			std::map<std::string, bool> warned;
			auto trim = [] (std::string &t) {
				size_t a = t.find_first_not_of (" \t\r\n"), b = t.find_last_not_of (" \t\r\n");
				t = a == std::string::npos ? "" : t.substr (a, b - a + 1);
			};
			while (std::getline (ifs, line)) {
				size_t c = line.find (';');
				if (c != std::string::npos) line.erase (c);
				trim (line);
				if (line.empty ()) continue;
				if (line[0] == '[') {
					size_t e = line.find (']');
					section = line.substr (1, e == std::string::npos ? std::string::npos : e - 1);
					trim (section);
					continue;
				}
				size_t eq = line.find ('=');
				if (eq == std::string::npos) continue;
				std::string key = line.substr (0, eq), val = line.substr (eq + 1);
				trim (key); trim (val);
				for (char &ch : key) ch = (char)tolower ((unsigned char)ch);
				if (key == "wind") {
					if (!section.empty ()) entries[section] = std::min (std::max (atof (val.c_str ()), 0.0), 25.0);
				} else if (!warned[key]) {
					warned[key] = true;
					oapiWriteLogV ("VulkanClient: Config/VK/Sea.cfg: unknown key %s", key.c_str ());
				}
			}
		}
		auto it = entries.find (body);
		return it == entries.end () ? -1.0 : it->second;
	}

	// Config/VK/Playa.cfg: the salt flats among a body's playas ([name] sections,
	// "Salt = lat lng radius", deg, deg, km). Read once.
	struct SaltFlat { double lat, lng, radius; };
	const std::vector<SaltFlat> &PlayaCfgSalt (const char *body)
	{
		static std::map<std::string, std::vector<SaltFlat>> entries;
		static bool loaded = false;
		if (!loaded) {
			loaded = true;
			std::ifstream ifs ("Config/VK/Playa.cfg");
			std::string line, section;
			auto trim = [] (std::string &t) {
				size_t a = t.find_first_not_of (" \t\r\n"), b = t.find_last_not_of (" \t\r\n");
				t = a == std::string::npos ? "" : t.substr (a, b - a + 1);
			};
			while (std::getline (ifs, line)) {
				size_t c = line.find (';');
				if (c != std::string::npos) line.erase (c);
				trim (line);
				if (line.empty ()) continue;
				if (line[0] == '[') {
					size_t e = line.find (']');
					section = line.substr (1, e == std::string::npos ? std::string::npos : e - 1);
					trim (section);
					continue;
				}
				size_t eq = line.find ('=');
				if (eq == std::string::npos) continue;
				std::string key = line.substr (0, eq), val = line.substr (eq + 1);
				trim (key);
				for (char &ch : key) ch = (char)tolower ((unsigned char)ch);
				SaltFlat f;
				if (key == "salt" && !section.empty () && sscanf (val.c_str (), "%lf %lf %lf", &f.lat, &f.lng, &f.radius) == 3)
					entries[section].push_back (f);
				else oapiWriteLogV ("VulkanClient: Config/VK/Playa.cfg: line not understood: %s", line.c_str ());
			}
		}
		static const std::vector<SaltFlat> none;
		auto it = entries.find (body);
		return it == entries.end () ? none : it->second;
	}

	constexpr uint32_t MAX_ATMOSPHERES = 64; // plus one "no atmosphere" set

	// Rayleigh scattering of Earth's air is close to proportional to the
	// square of its sky colour (AtmColor0), which is used to derive the
	// coefficients of other atmospheres: beta = K * colour^2 * density factor
	constexpr double RAYLEIGH_K = 3.9e-5;  // [1/m]
	constexpr double MIE_BETA = 1.0e-5;    // [1/m] for Earth-like density
	constexpr double EARTH_RHO0 = 1.225;   // [kg/m^3]

	// Overcast atmospheres: a haze of an optical depth tau through the
	// whole atmosphere (Venus: its cloud deck, Titan: its photochemical
	// haze), scattering forward with the single-scattering albedo scat
	// (absorbing blue: yellow-orange skies), and skylight: the ground lit
	// mostly by the sky (irradiance relative to the sun's at the top).
	// The sky model (single scattering of sunlight, and higher orders from
	// it, Hillaire 2020) does not diffuse light through optically thick
	// layers: the optical depths are kept moderate (also the gas's: density
	// factor rayleigh), so that the sky stays lit, and the skylight term
	// stands for the light diffused down.
	// The haze also passes on light from about the sun's direction (the
	// beam plus the forward-scattered aureole). share is that light's
	// share of the ground's light with the sun at the zenith; on level
	// ground it falls as mu exp(-falloff (1/mu - 1)). It comes from an
	// apparent source of angular radius radius [rad], and blends into the
	// beam up to the cloud deck at deck [m]. The share is taken out of the
	// skylight (Scene.cpp), so the ground's light is unchanged.
	// (pass 2, T4) Venus' share raised from 0.12 to 0.25: at 0.12 the ship showed no direction of the light.
	// nearHaze: a haze layer near the ground (C4): extinction at the surface [1/m] (grey) and scale height
	// [m]. The sky model keeps the optical depths moderate (above), so that far ground showed through the
	// thick lower air brighter than the sky above it (Venus' horizon read inverted); the layer adds the
	// near-horizontal paths' extinction and light (a horizontal optical depth of about 3 over 10 km on
	// Venus: the far ground fades into the haze's light, which near the horizon is the sky's) and little
	// changes the zenith (vertical depth beta H, 0.45)
	struct Overcast { const char *name; double tau, scat[3], g, rayleigh; float sky[3]; double share, falloff, radius, deck, hazeBeta, hazeH; };
	const Overcast OVERCAST[] = {
		{ "Venus", 2.0, { 0.98, 0.85, 0.55 }, 0.75f, 1.0, { 1.80f, 1.25f, 0.55f }, 0.25, 0.6, 0.30, 60e3, 3.0e-4, 1500.0 },
		{ "Titan", 1.2, { 0.96, 0.80, 0.52 }, 0.70f, 2.0, { 1.80f, 1.25f, 0.65f }, 0.30, 0.4, 0.12, 30e3, 1.0e-4, 1500.0 },
	};
	const Overcast *FindOvercast (const char *name)
	{
		for (const Overcast &o : OVERCAST) if (!strcmp (o.name, name)) return &o;
		return nullptr;
	}

} // namespace

bool OvercastSkylight (const char *planet, float rgb[3])
{
	const Overcast *o = FindOvercast (planet);
	if (!o) return false;
	for (int i = 0; i < 3; i++) rgb[i] = o->sky[i];
	return true;
}

bool OvercastSunlight (const char *planet, OvercastSun &s)
{
	// ORBITER_VK_OVERCAST_SUN: scales the shares (0: off, the old lighting)
	static const double scale = [] {
		const char *e = getenv ("ORBITER_VK_OVERCAST_SUN");
		return (e && *e) ? std::clamp (atof (e), 0.0, 3.0) : 1.0;
	}();
	const Overcast *o = FindOvercast (planet);
	if (!o || scale <= 0.0) return false;
	s = { o->share * scale, o->falloff, o->radius, o->deck };
	return true;
}

// ======================================================================
// AtmosphereRenderer

bool AtmosphereRenderer::Init (vkc::Context *c, TextureManager *tm)
{
	ctx = c;
	texmgr = tm;
	VkDescriptorSetLayoutBinding b[11]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	for (int i = 1; i < 11; i++) { // ground, clouds, transmittance and multiple-scattering tables, volumetric clouds' coverage and shape, the sea field (W3)
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // (volumetric clouds)
	}
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 11;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));

	const uint32_t nSets = MAX_ATMOSPHERES * 2 * vkc::FRAMES_IN_FLIGHT + 1;
	VkDescriptorPoolSize ps[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nSets },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 * nSets }
	};
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pci.maxSets = nSets;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	noneUbo = ctx->CreateBuffer (sizeof(AtmUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
	memset (noneUbo.mapped, 0, sizeof(AtmUniforms));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &setLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &noneSet));
	VkDescriptorBufferInfo dbi{ noneUbo.buffer, 0, sizeof(AtmUniforms) };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = noneSet;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	wr.pBufferInfo = &dbi;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	WriteGround (noneSet, texmgr->White(), texmgr->White());
	WriteGround (noneSet, texmgr->White(), texmgr->White(), 3);
	WriteGround (noneSet, texmgr->White(), texmgr->White(), 5); // (tables unused without an atmosphere)
	{
		VkSamplerCreateInfo oci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; // (the sea field: periodic, mip mapped)
		oci.magFilter = oci.minFilter = VK_FILTER_LINEAR;
		oci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		oci.addressModeU = oci.addressModeV = oci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		oci.maxLod = 16.0f;
		VK_CHECK(vkCreateSampler (ctx->device, &oci, nullptr, &oceanSampler));
	}
	WriteOcean (noneSet, VK_NULL_HANDLE, VK_NULL_HANDLE);

	// precomputed tables: sampler and compute pipelines
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &lutSampler));
	VkDescriptorSetLayoutBinding lb[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
	VkDescriptorSetLayoutCreateInfo llci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	llci.bindingCount = 2;
	llci.pBindings = lb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &llci, nullptr, &lutSetLayout));
	VkPushConstantRange lpcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LutParams) };
	VkPipelineLayoutCreateInfo lplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	lplci.setLayoutCount = 1;
	lplci.pSetLayouts = &lutSetLayout;
	lplci.pushConstantRangeCount = 1;
	lplci.pPushConstantRanges = &lpcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &lplci, nullptr, &lutLayout));
	auto compute = [this](const unsigned char *code, size_t size) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = lutLayout;
		VkPipeline pipe = VK_NULL_HANDLE;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		return pipe;
	};
	transmittancePipe = compute (spv_atm_transmittance_comp, spv_atm_transmittance_comp_size);
	multiscatPipe = compute (spv_atm_multiscat_comp, spv_atm_multiscat_comp_size);

	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &skyLayout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_sky_vert, spv_sky_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_sky_frag, spv_sky_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = ctx->sceneSamples;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE; // the sky is added to the star background, the ground covers it
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &ctx->sceneFormat;
	rci.depthAttachmentFormat = ctx->depthFormat;
	VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpi.pNext = &rci;
	gpi.stageCount = 2;
	gpi.pStages = stages;
	gpi.pVertexInputState = &vi;
	gpi.pInputAssemblyState = &ia;
	gpi.pViewportState = &vps;
	gpi.pRasterizationState = &rs;
	gpi.pMultisampleState = &ms;
	gpi.pDepthStencilState = &ds;
	gpi.pColorBlendState = &cb;
	gpi.pDynamicState = &dy;
	gpi.layout = skyLayout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &skyPipeline));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return true;
}

void AtmosphereRenderer::SetCloudVolume (VkImageView coverage, VkSampler covSampler, VkImageView shape, VkSampler shapeSampler)
{
	cloudVol[0] = { covSampler, coverage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	cloudVol[1] = { shapeSampler, shape, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	WriteCloudVolume (noneSet);
}

void AtmosphereRenderer::WriteCloudVolume (VkDescriptorSet set) const
{
	VkWriteDescriptorSet wr[2]{};
	for (int k = 0; k < 2; k++) {
		wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[k].dstSet = set;
		wr[k].dstBinding = 7 + k;
		wr[k].descriptorCount = 1;
		wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[k].pImageInfo = &cloudVol[k];
	}
	vkUpdateDescriptorSets (ctx->device, 2, wr, 0, nullptr);
}

void AtmosphereRenderer::WriteOcean (VkDescriptorSet set, VkImageView longV, VkImageView rippleV) const
{
	VkImageView w = texmgr->White ()->img.view;
	VkDescriptorImageInfo ii[2] = {
		{ oceanSampler, longV ? longV : w, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ oceanSampler, rippleV ? rippleV : w, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet wr[2]{};
	for (int k = 0; k < 2; k++) {
		wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[k].dstSet = set;
		wr[k].dstBinding = 9 + k;
		wr[k].descriptorCount = 1;
		wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[k].pImageInfo = &ii[k];
	}
	vkUpdateDescriptorSets (ctx->device, 2, wr, 0, nullptr);
}

void AtmosphereRenderer::WriteGround (VkDescriptorSet set, VkTexture *west, VkTexture *east, int binding) const
{
	// the textures' own descriptor sets carry the sampler and view
	VkCopyDescriptorSet cp[2]{};
	VkTexture *t[2] = { west ? west : texmgr->White(), east ? east : texmgr->White() };
	for (int i = 0; i < 2; i++) {
		cp[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
		cp[i].srcSet = t[i]->set;
		cp[i].srcBinding = 0;
		cp[i].dstSet = set;
		cp[i].dstBinding = binding + i;
		cp[i].descriptorCount = 1;
	}
	vkUpdateDescriptorSets (ctx->device, 0, nullptr, 2, cp);
}

void AtmosphereRenderer::ComputeLuts (const LutParams &p, const vkc::Image &trans, const vkc::Image &ms)
{
	// one-off (at the creation of a planet's visual): a temporary descriptor
	// pool, executed and waited for
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VkDescriptorPool tmpPool;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &tmpPool));
	VkDescriptorSetLayout l[2] = { lutSetLayout, lutSetLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = tmpPool;
	dai.descriptorSetCount = 2;
	dai.pSetLayouts = l;
	VkDescriptorSet s[2];
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, s));
	VkDescriptorImageInfo tOut{ VK_NULL_HANDLE, trans.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo tIn{ lutSampler, trans.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo mOut{ VK_NULL_HANDLE, ms.view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet wr[4]{};
	for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.descriptorCount = 1;
	wr[0].dstSet = s[0], wr[0].dstBinding = 0, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[0].pImageInfo = &tOut;
	wr[1].dstSet = s[0], wr[1].dstBinding = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[1].pImageInfo = &tIn;
	wr[2].dstSet = s[1], wr[2].dstBinding = 0, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[2].pImageInfo = &mOut;
	wr[3].dstSet = s[1], wr[3].dstBinding = 1, wr[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[3].pImageInfo = &tIn;
	vkUpdateDescriptorSets (ctx->device, 4, wr, 0, nullptr);
	VkPipeline tp = transmittancePipe, mp = multiscatPipe;
	VkPipelineLayout pl = lutLayout;
	VkImage ti = trans.image, mi = ms.image;
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, ti, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
		vkc::ImageBarrier (cmd, mi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
		vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tp);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s[0], 0, nullptr);
		vkCmdDispatch (cmd, (TRANSMITTANCE_SIZE.width + 7) / 8, (TRANSMITTANCE_SIZE.height + 7) / 8, 1);
		vkc::ImageBarrier (cmd, ti, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mp);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s[1], 0, nullptr);
		vkCmdDispatch (cmd, MULTISCAT_SIZE.width, MULTISCAT_SIZE.height, 1);
		vkc::ImageBarrier (cmd, mi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	vkDestroyDescriptorPool (ctx->device, tmpPool, nullptr);
}

void AtmosphereRenderer::Shutdown ()
{
	if (!ctx) return;
	vkDestroyPipeline (ctx->device, transmittancePipe, nullptr);
	vkDestroyPipeline (ctx->device, multiscatPipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, lutLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, lutSetLayout, nullptr);
	vkDestroySampler (ctx->device, lutSampler, nullptr);
	vkDestroyPipeline (ctx->device, skyPipeline, nullptr);
	vkDestroyPipelineLayout (ctx->device, skyLayout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	ctx->DestroyBuffer (noneUbo);
	vkDestroySampler (ctx->device, oceanSampler, nullptr);
	ctx = nullptr;
}

// ======================================================================
// Atmosphere

Atmosphere::Atmosphere (OBJHANDLE h, Scene *s)
	: hPlanet(h), scene(s)
{
	ctx = scene->Client()->Ctx();
	radius = oapiGetSize (hPlanet);
	atmRadius = radius;

	const ATMCONST *ac = oapiPlanetHasAtmosphere (hPlanet) ? oapiGetPlanetAtmConstants (hPlanet) : nullptr;
	// (tenuous atmospheres, e.g. Io's and Triton's, a ten thousandth of
	// Earth's density: a black sky, as in vacuum)
	if (ac && ac->rho0 > 1e-3 * EARTH_RHO0 && ac->p0 > 0.0) {
		double g = GGRAV * oapiGetMass (hPlanet) / (radius * radius);
		double H = ac->p0 / (ac->rho0 * g); // scale height [m]
		double dens = std::clamp (ac->rho0 / EARTH_RHO0, 0.1, 10.0);
		const double col[3] = { ac->color0.x, ac->color0.y, ac->color0.z };
		// aerosol (Mie) scattering takes the colour of the configured haze
		double haze[3] = { 1.0, 1.0, 1.0 };
		if (const VECTOR3 *hc = (const VECTOR3*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_HAZECOLOUR)) {
			double m = std::max ({ hc->x, hc->y, hc->z });
			if (m > 0.0) haze[0] = hc->x / m, haze[1] = hc->y / m, haze[2] = hc->z / m;
		}
		for (int i = 0; i < 3; i++) {
			betaR[i] = (float)(RAYLEIGH_K * col[i] * col[i] * dens);
			betaM[i] = (float)(MIE_BETA * dens * haze[i]);
		}
		betaR[3] = (float)H;
		betaM[3] = (float)(H / 6.0);
		atmRadius = radius + std::min (ac->altlimit > 0.0 ? ac->altlimit : 1e30, 12.0 * H);
		present = true;
		// Earth's ozone layer absorbs in the orange (the blue of the sky at twilight)
		char name[64];
		oapiGetObjectName (hPlanet, name, 64);
		if (!strcmp (name, "Earth")) {
			ozone[0] = 0.650e-6f, ozone[1] = 1.881e-6f, ozone[2] = 0.085e-6f;
			airglow = 1.0f; // (the green oxygen line at about 95 km)
			// aurora about the geomagnetic poles (north pole near 80.7 N, 72.7 W)
			double plat = 80.7 * RAD, plng = -72.7 * RAD;
			aurora[0] = (float)(cos (plat) * cos (plng)), aurora[1] = (float)sin (plat), aurora[2] = (float)(cos (plat) * sin (plng));
			aurora[3] = 1.0f;
			if (const char *e = getenv ("ORBITER_VK_AURORA")) aurora[3] = (float)atof (e);
		}
		if (!strcmp (name, "Mars")) {
			// suspended dust: a vertical optical depth of about 0.5 over a
			// scale height like the gas's, scattering mostly forward, and
			// absorbing blue light (the butterscotch sky, blue sunsets)
			const double tau = 0.4, scat[3] = { 0.96, 0.78, 0.48 }; // single-scattering albedo
			for (int i = 0; i < 3; i++) {
				betaM[i] = (float)(tau / H * scat[i]);
				ozone[i] = (float)(tau / H * (1.0 - scat[i]));
			}
			betaM[3] = (float)H;
			ozone[3] = 1.0f; // (absorption with the dust's profile)
			// the dust's diffraction peak about the sun is narrower for blue
			// light (about lambda / particle size): blue about the sun, the
			// sky farther from it butterscotch (Curiosity's blue sunsets)
			mieG = 0.55f;
			mieLobe[0] = 0.84f, mieLobe[1] = 0.88f, mieLobe[2] = 0.92f, mieLobe[3] = 0.5f;
		}
		if (const Overcast *o = FindOvercast (name)) {
			for (int i = 0; i < 3; i++) overcastSky[i] = o->sky[i];
			static const bool hazeOff = [] { const char *e = getenv ("ORBITER_VK_NEAR_HAZE"); return e && *e && !atoi (e); }();
			if (!hazeOff) { // (scattering per colour with the haze's albedo, absorbing blue as the deck's; the shader takes the extinction as grey)
				for (int i = 0; i < 3; i++) nearHaze[i] = (float)(o->hazeBeta * o->scat[i]);
				nearHaze[3] = (float)o->hazeH;
			}
			OvercastSun os;
			if (OvercastSunlight (name, os)) {
				for (int i = 0; i < 3; i++) overcastSun[i] = (float)(os.share * o->sky[i]);
				overcastPrm[0] = (float)os.falloff, overcastPrm[1] = (float)os.deck, overcastPrm[2] = (float)tan (os.radius);
			}
			for (int i = 0; i < 3; i++) {
				betaR[i] = (float)(RAYLEIGH_K * col[i] * col[i] * o->rayleigh);
				betaM[i] = (float)(o->tau / H * o->scat[i]);
				ozone[i] = (float)(o->tau / H * (1.0 - o->scat[i]));
			}
			betaM[3] = (float)H;
			ozone[3] = 1.0f;
			mieG = o->g;
		}
		if (!oapiPlanetHasSurface (hPlanet)) {
			// gas giants: no ground at the 1-bar level (the planet's radius);
			// the gas reaches down to a dark floor far below, where no
			// sunlight arrives (the scattering coefficients are given at
			// the floor: as dense there as the gas's profile makes it)
			const double D = GG_FLOOR_DEPTH;
			double f = exp (D / H);
			betaM[3] = (float)H; // (the haze mixed through the gas)
			for (int i = 0; i < 3; i++) betaR[i] *= (float)f, betaM[i] *= (float)f;
			radius -= D;
			groundAlbedo = 0.0f;
			gasGiant = true;
		}
	}

	{
		// waves: planets whose cfg asks for them, and procedural seas
		bool cfgRipple = false;
		if (const bool *r = (const bool*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_SURFACERIPPLE)) cfgRipple = *r;
		PROCSURFACEPARAM wp;
		bool proc = oapiProcSurfaceParams (hPlanet, &wp);
		waves = cfgRipple || (proc && (wp.surfClass == 1 || (wp.surfClass == 2 && wp.openWater > 0)));
		procSea = proc && (wp.surfClass == 1 || (wp.surfClass == 2 && wp.openWater > 0));
		procOcean = proc && wp.surfClass == 1;
		wavesWhy = cfgRipple ? "cfg" : proc && wp.surfClass == 1 ? "ocean" : "ice open water";
		if (const char *e = getenv ("ORBITER_VK_WAVES")) if (*e && !atoi (e)) waves = false; // (empty: unset)
		oceanOn = waves;
		if (const char *e = getenv ("ORBITER_VK_WAVE_FFT")) if (*e && !atoi (e)) oceanOn = false;
		if (const char *e = getenv ("ORBITER_VK_FOAM")) if (*e && !atoi (e)) foam = false; // (empty: unset)
		waveG = GGRAV * oapiGetMass (hPlanet) / (radius * radius);
		oapiGetObjectName (hPlanet, wavesName, 64);
		if (const char *e = getenv ("ORBITER_VK_SEA_WIND")) if (*e && atof (e) >= 0.0) seaEnvWind = atof (e);
		seaCfgWind = SeaCfgWind (wavesName);
		sea = SeaState (0.0, 0.0, 0.0); // (the far-field state until the first anchoring)
		if (waves) {
			double l0 = 60.0 * sea.lambdaScale;
			double k0 = 2.0 * PI / l0;
			double bd = fmod (sea.bearing * 180.0 / PI + 360.0, 360.0);
			oapiWriteLogV ("VulkanClient: waves on %s (%s): g %.2f m/s^2, wind %.1f m/s (%s) toward %03.0f deg, wavelengths %.1f-%.2f m, longest period %.1f s, slope %.4f per wave, mean-square slope %.4f (%.4f below the shortest wave)",
				wavesName, wavesWhy, waveG, sea.wind, sea.src, bd, l0, l0 * pow (0.7, WAVE_COUNT - 1),
				2.0 * PI / sqrt (waveG * k0 + SIGMA_RHO * k0 * k0 * k0), sea.slope, sea.varTotal, sea.varUnres);
			if (foam) {
				wfoam = FoamState (sea);
				LogFoam ("");
			} else
				oapiWriteLogV ("VulkanClient: whitecaps on %s: off (ORBITER_VK_FOAM=0)", wavesName);
		}
	}
	{
		// procedural surface detail close to the camera: craters and rocks
		// on airless bodies, eroded craters and rocks on Mars, undulation and
		// mottling on Earth; none on bodies with deep atmospheres (gas giants)
		char name[64];
		oapiGetObjectName (hPlanet, name, 64);
		// (Venus and Titan: rocks and grain as on Mars, under their atmospheres)
		int type = !strcmp (name, "Earth") ? 3 : !strcmp (name, "Mars") || !strcmp (name, "Venus") || !strcmp (name, "Titan") ? 2 : !present ? 1 : 0;
		// (a procedural world with an atmosphere: rocks and grain as on Mars)
		PROCSURFACEPARAM pp;
		if (type == 0 && oapiProcSurfaceParams (hPlanet, &pp) && pp.surfClass != 5) type = 2;
		if (const char *e = getenv ("ORBITER_VK_SURFACE_DETAIL")) if (!atoi (e)) type = 0;
		detail[0] = (float)type, detail[1] = 1.0f, detail[2] = 3000.0f;

		// colour saturation of the surface textures: the Moon's and
		// Mercury's carry tints (green, purple) that the grey ground doesn't
		// have; planet files may set it (SurfaceSaturation)
		// (Venus: the radar maps' false colour; its basalt is dark grey, the
		// orange is the light's)
		surfSaturation = !strcmp (name, "Moon") ? 0.3f : !strcmp (name, "Mercury") ? 0.5f : !strcmp (name, "Venus") ? 0.15f : 1.0f;
		if (FILEHANDLE f = oapiOpenFile ((std::string (name) + ".cfg").c_str(), FILE_IN, CONFIG)) {
			double v;
			if (oapiReadItem_float (f, (char*)"SurfaceSaturation", v)) surfSaturation = (float)std::clamp (v, 0.0, 2.0);
			oapiCloseFile (f, FILE_IN);
		}
	}

	AtmosphereRenderer *ar = scene->Atm();
	if (present) {
		auto image = [this](VkExtent2D e) {
			vkc::Image im;
			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = LUT_FORMAT;
			ici.extent = { e.width, e.height, 1 };
			ici.mipLevels = 1;
			ici.arrayLayers = 1;
			ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_OPTIMAL;
			ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			VmaAllocationCreateInfo aci{};
			aci.usage = VMA_MEMORY_USAGE_AUTO;
			VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = im.image;
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = LUT_FORMAT;
			vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
			return im;
		};
		transmittanceLut = image (TRANSMITTANCE_SIZE);
		multiscatLut = image (MULTISCAT_SIZE);
		AtmosphereRenderer::LutParams lp{};
		memcpy (lp.betaR, betaR, sizeof(betaR));
		memcpy (lp.betaM, betaM, sizeof(betaM));
		memcpy (lp.ozone, ozone, sizeof(ozone));
		lp.radii[0] = (float)radius, lp.radii[1] = (float)atmRadius, lp.radii[2] = groundAlbedo, lp.radii[3] = mieG;
		ar->ComputeLuts (lp, transmittanceLut, multiscatLut);
	}
	for (uint32_t i = 0; i < NSETS; i++) {
		ubo[i] = ctx->CreateBuffer (sizeof(AtmUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		memset (ubo[i].mapped, 0, sizeof(AtmUniforms));
		VkDescriptorSetLayout l = ar->SetLayout();
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = ar->Pool();
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &l;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &sets[i]));
		VkDescriptorBufferInfo dbi{ ubo[i].buffer, 0, sizeof(AtmUniforms) };
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = sets[i];
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wr.pBufferInfo = &dbi;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
		ar->WriteGround (sets[i], nullptr, nullptr);
		ar->WriteGround (sets[i], nullptr, nullptr, 3);
		ar->WriteGround (sets[i], nullptr, nullptr, 5);
		ar->WriteCloudVolume (sets[i]);
		if (oceanOn && i < vkc::FRAMES_IN_FLIGHT) {
			// the sea field's images (per frame in flight: written by UploadOcean as the frame begins)
			auto image = [this](uint32_t n, uint32_t mips) {
				vkc::Image im;
				VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
				ici.imageType = VK_IMAGE_TYPE_2D;
				ici.format = VK_FORMAT_R16G16_SFLOAT;
				ici.extent = { n, n, 1 };
				ici.mipLevels = mips;
				ici.arrayLayers = 1;
				ici.samples = VK_SAMPLE_COUNT_1_BIT;
				ici.tiling = VK_IMAGE_TILING_OPTIMAL;
				ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
				VmaAllocationCreateInfo aci{};
				aci.usage = VMA_MEMORY_USAGE_AUTO;
				VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
				VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				vci.image = im.image;
				vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				vci.format = ici.format;
				vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
				VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
				return im;
			};
			oceanImg[i][0] = image (OceanFft::N, OceanFft::MIPS + 1);
			oceanImg[i][1] = image (OceanFft::N, 1);
			oceanStage[i] = ctx->CreateBuffer (OceanStageBytes (), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
			// (readable from the start: bound before the first upload, and on planets the camera is not at)
			VkImage a = oceanImg[i][0].image, b = oceanImg[i][1].image;
			ctx->ImmediateSubmit ([a, b](VkCommandBuffer c) {
				for (VkImage im : { a, b })
					vkc::ImageBarrier (c, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			});
		}
		if (oceanOn) {
			uint32_t f = i % vkc::FRAMES_IN_FLIGHT;
			ar->WriteOcean (sets[i], oceanImg[f][0].view ? oceanImg[f][0].view : VK_NULL_HANDLE, oceanImg[f][1].view);
		} else
			ar->WriteOcean (sets[i], VK_NULL_HANDLE, VK_NULL_HANDLE);
		if (present) {
			VkDescriptorImageInfo ii[2] = {
				{ ar->LutSampler(), transmittanceLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
				{ ar->LutSampler(), multiscatLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
			VkWriteDescriptorSet lw[2]{};
			for (int k = 0; k < 2; k++) {
				lw[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				lw[k].dstSet = sets[i];
				lw[k].dstBinding = 5 + k;
				lw[k].descriptorCount = 1;
				lw[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				lw[k].pImageInfo = &ii[k];
			}
			vkUpdateDescriptorSets (ctx->device, 2, lw, 0, nullptr);
		}
	}
}

void Atmosphere::SetClouds (VkTexture *west, VkTexture *east, double alt)
{
	if (!west || !east) return;
	for (uint32_t i = 0; i < NSETS; i++)
		scene->Atm()->WriteGround (sets[i], west, east, 3);
	cloudParams[0] = (float)(radius + alt);
	cloudParams[2] = 0.3f;
	if (const float *d = (const float*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDSHADOWCOL))
		cloudParams[2] = std::clamp (*d, 0.0f, 1.0f);
	cloudParams[3] = 1.0f;
}

void Atmosphere::SetGround (VkTexture *west, VkTexture *east, const float colour[3])
{
	for (uint32_t i = 0; i < NSETS; i++)
		scene->Atm()->WriteGround (sets[i], west, east);
	for (int i = 0; i < 3; i++) groundColour[i] = colour[i];
	groundColour[3] = (west && east) ? 1.0f : 0.0f;
}

Atmosphere::~Atmosphere ()
{
	vkc::Context *c = ctx;
	vkc::Image tl = transmittanceLut, ml = multiscatLut;
	ctx->DeferDestroy ([c, tl, ml]() mutable { c->DestroyImage (tl); c->DestroyImage (ml); });
	VkDescriptorPool pool = scene->Atm()->Pool();
	for (uint32_t i = 0; i < NSETS; i++) {
		vkc::Buffer b = ubo[i];
		VkDescriptorSet s = sets[i];
		ctx->DeferDestroy ([c, b, s, pool]() mutable {
			c->DestroyBuffer (b);
			vkFreeDescriptorSets (c->device, pool, 1, &s);
		});
	}
	for (uint32_t f = 0; f < vkc::FRAMES_IN_FLIGHT; f++) {
		vkc::Image i0 = oceanImg[f][0], i1 = oceanImg[f][1];
		vkc::Buffer sb = oceanStage[f];
		if (!i0.image) continue;
		ctx->DeferDestroy ([c, i0, i1, sb]() mutable { c->DestroyImage (i0); c->DestroyImage (i1); c->DestroyBuffer (sb); });
	}
}

uint32_t Atmosphere::SetIndex () const
{
	return ctx->FrameIndex() + (scene->InProbe () ? vkc::FRAMES_IN_FLIGHT : 0);
}

VkDescriptorSet Atmosphere::Set () const
{
	return sets[SetIndex ()];
}

// Sea state from the wind 10 m above the water (a fully developed sea):
// speed and direction from the core's wind model (x east, y up, z north), or
// the overrides (env > Sea.cfg > model; they keep the model's direction).
WaveSea Atmosphere::SeaState (double lng, double lat, double elev) const
{
	WaveSea s;
	VECTOR3 w = oapiGetModelWind (hPlanet, lng, lat, std::max (elev, 0.0) + 10.0);
	double U = hypot (w.x, w.z), bearing = PI / 2.0;
	s.src = "none";
	if (U > 1e-3) bearing = atan2 (w.x, w.z), s.src = "model";
	if (seaCfgWind >= 0.0) U = seaCfgWind, s.src = "cfg";
	if (seaEnvWind >= 0.0) U = seaEnvWind, s.src = "env";
	U = std::min (std::max (U, 0.0), 25.0);
	// Pierson-Moskowitz: the peak wavenumber k_p = 0.877^2 g / U19.5^2 with
	// U19.5 = 1.06 U10, so the peak wavelength is 9.2 U^2 / g; the longest
	// wave drawn is a little longer (3 m: calm water's wavelets; 400 m: within
	// the 3 km fade and the pixels' window)
	double lp = 9.2 * U * U / waveG;
	double l0 = std::min (std::max (1.25 * lp, 3.0), 400.0);
	// wind waves form near 1 m/s (glassy below); Cox & Munk 1954, clean sea:
	// the mean-square slope (both axes) 0.003 + 5.12e-3 U, plus a floor
	double t = std::min (std::max ((U - 0.7) / (2.5 - 0.7), 0.0), 1.0);
	double on = t * t * (3.0 - 2.0 * t);
	double var = 1e-4 + (0.003 + 5.12e-3 * U) * on;
	// log bands of 0.7 from l0 down to 1 cm: in the saturation range each
	// holds the same slope variance, so the drawn waves share it equally
	double N = log (l0 / 0.01) / log (1.0 / 0.7);
	double slope = sqrt (2.0 * var / N);
	double varUnres = var * (N - WAVE_COUNT) / N * SEA_UNRES_GAIN;
	s.slope = (float)slope;
	s.lambdaScale = (float)(l0 / 60.0);
	s.wind = (float)U;
	s.bearing = (float)bearing;
	s.varUnres = (float)varUnres;
	s.varTotal = (float)(varUnres + WAVE_COUNT * slope * slope / 2.0);
	return s;
}

// Whitecaps and the surf band from the sea state. The shader (tile_main.glsl,
// Whitecaps) breaks the crests of the longest CAP_WAVES waves, where the crest
// index S = slope sum sin(phi_i) (the waves' downward acceleration over g,
// linear wave theory: k_i a_i = slope for every wave) exceeds a threshold.
// The coverage is set by the wind: Stramska & Petelski 2003 (developed seas),
// W = 5e-5 (U - 4.47)^3 (at most 0.3). Here the threshold is solved for it on
// samples of the same formula (same noise ports, same constants), so the
// look follows the waves and the coverage follows the wind. Cached on
// (wind, slope, lambdaScale, g). Surf: Pierson-Moskowitz Hs and Tp, breaking
// at a depth of about 1.3 Hs.
WaveFoam Atmosphere::FoamState (const WaveSea &s, bool *ran)
{
	if (ran) *ran = false;
	if (foamKey[0] == s.wind && foamKey[1] == s.slope && foamKey[2] == s.lambdaScale && foamKey[3] == (float)waveG)
		return wfoam;
	foamKey[0] = s.wind, foamKey[1] = s.slope, foamKey[2] = s.lambdaScale, foamKey[3] = (float)waveG;
	if (ran) *ran = true;
	constexpr int NS = 8192;
	WaveFoam f;
	const double g = waveG, U = s.wind, twoPi = 2.0 * PI;
	// the noise samples (once per body)
	if (capE.empty()) {
		std::mt19937 rng (12345);
		std::uniform_real_distribution<float> uni (0.0f, 1.0f);
		capE.resize (NS), capLace.resize (NS), capLA.resize (NS), capA.resize (NS);
		double sum = 0.0;
		for (int j = 0; j < NS; j++) {
			float x = uni (rng) * 512.0f, z = uni (rng) * 512.0f;
			capE[j] = CapEdgeNoise (x, 0.0f, z);
			capLace[j] = FoamLace (x, 0.0f, z);
			capLA[j] = FoamLA (x, 0.0f, z);
			capA[j] = FoamA (x, 0.0f, z);
			sum += capLace[j];
		}
		laceMean = (float)(sum / NS);
	}
	// shore: Pierson-Moskowitz developed sea
	{
		double Hs = 0.22 * U * U / g;
		double Tp = std::max (7.60 * U / g, 2.0);
		f.Hs = (float)Hs;
		f.shoreT = (float)Tp;
		f.shoreW = (float)std::min (std::max (6.0 + 25.0 * Hs, 6.0), 120.0);
		f.shoreL = (float)std::max (Tp * sqrt (1.3 * g * Hs), 3.0);
		f.shoreOn = (float)SmoothStep (0.5, 3.0, U);
	}
	double W = U > 4.47 ? std::min (5.0e-5 * pow (U - 4.47, 3.0), 0.3) : 0.0;
	if (W < 1e-6) W = 0.0; // (a hair above 4.47 m/s: none)
	f.cover = (float)W;
	const double slope = s.slope, sig = slope * sqrt (CAP_WAVES / 2.0);
	f.sigma = (float)sig;
	if (W <= 0.0 || slope <= 0.0) return f;

	// the sea's waves for the statistics (the GPU's geometry, relative to the main direction)
	std::mt19937 rng (12345);
	std::uniform_real_distribution<double> uni (0.0, 1.0);
	double k[CAP_WAVES], om[CAP_WAVES], dx[CAP_WAVES], dy[CAP_WAVES], c[CAP_WAVES];
	const double l0 = 60.0 * s.lambdaScale;
	for (int i = 0; i < CAP_WAVES; i++) {
		double lambda = l0 * pow (0.7, i);
		k[i] = twoPi / lambda;
		om[i] = sqrt (g * k[i] + SIGMA_RHO * k[i] * k[i] * k[i]);
		double ang = (WAVE_SPREAD0 + WAVE_SPREAD1 * i / (WAVE_COUNT - 1.0)) * sin (i * 2.39996 + 0.3 * sin (i * 7.1));
		dx[i] = cos (ang), dy[i] = sin (ang);
		c[i] = uni (rng) * twoPi;
	}
	const double T0 = twoPi / om[0];
	struct Smp { double S, win, tc, Sd[2], o[2], tau[2]; };
	std::vector<Smp> sm (NS);
	const double E = exp (-CAP_LIFE / CAP_TF);
	for (int j = 0; j < NS; j++) {
		// position and time from the RNG (the noise is paired arbitrarily: only statistics matter)
		double X = uni (rng) * 8.0 * l0, Y = uni (rng) * 8.0 * l0, t = uni (rng) * 20.0 * T0;
		double phi[CAP_WAVES], m[CAP_WAVES];
		Smp &p = sm[j];
		p.S = 0.0;
		for (int i = 0; i < CAP_WAVES; i++) {
			phi[i] = k[i] * (X * dx[i] + Y * dy[i]) - om[i] * t + c[i];
			double dmdu;
			WaveEnvelope (i, WAVE_ENV_K * k[i] * (Y * dx[i] - X * dy[i]) + 2.3 * i + 0.5, m[i], dmdu); // (the crest envelope)
			p.S += slope * m[i] * sin (phi[i]);
		}
		double dphi = fmod (phi[0] - PI / 2.0 + PI, twoPi);
		if (dphi < 0.0) dphi += twoPi;
		dphi -= PI;
		p.win = SmoothStep (-0.15, 0.0, dphi) * (1.0 - SmoothStep (0.45, 0.9, dphi));
		double a = fmod (PI / 2.0 - phi[0], twoPi);
		if (a < 0.0) a += twoPi;
		p.tc = a / om[0];
		for (int n = 0; n < 2; n++) {
			double tau = p.tc + n * T0;
			double Sd = 0.0;
			for (int i = 0; i < CAP_WAVES; i++) Sd += slope * m[i] * sin (phi[i] + om[i] * tau);
			p.Sd[n] = Sd;
			p.o[n] = std::max ((exp (-tau / (CAP_TF * T0)) - E) / (1.0 - E), 0.0);
			p.tau[n] = tau / T0;
		}
	}
	auto foamAt = [&](int j, double th) {
		const Smp &p = sm[j];
		double e = (double)capE[j] * CAP_EDGE * sig;
		double act = SmoothStep (th, th + CAP_WIDTH * sig, p.S + e) * p.win * capLA[j] * capA[j];
		double keep = 1.0 - act;
		for (int n = 0; n < 2; n++) {
			double thn = th - CAP_SPREAD * sig * p.tau[n];
			keep *= 1.0 - SmoothStep (thn, thn + CAP_WIDTH * sig, p.Sd[n] + e) * p.o[n] * capLace[j];
		}
		return 1.0 - keep;
	};
	auto cov = [&](double th) {
		int cnt = 0;
		for (int j = 0; j < NS; j++) if (foamAt (j, th) > CAP_COVER_F) cnt++;
		return (double)cnt / NS;
	};
	double lo = -CAP_WAVES * slope - sig, hi = CAP_WAVES * slope + sig;
	for (int it = 0; it < 30; it++) {
		double mid = 0.5 * (lo + hi);
		if (cov (mid) > W) lo = mid; else hi = mid;
	}
	double th = 0.5 * (lo + hi), sum = 0.0;
	for (int j = 0; j < NS; j++) sum += foamAt (j, th);
	f.thresh = (float)th;
	f.measured = (float)cov (th);
	f.mean = (float)(sum / NS);
	return f;
}

void Atmosphere::LogFoam (const char *prefix) const
{
	const WaveFoam &f = wfoam;
	char cov[160];
	if (f.cover > 0.0f)
		snprintf (cov, sizeof (cov), "coverage %.5f (measured %.5f), threshold %.4f (%.2f sigma), mean foam %.5f, lace mean %.3f",
			f.cover, f.measured, f.thresh, f.sigma > 0.0f ? f.thresh / f.sigma : 0.0f, f.mean, laceMean);
	else
		snprintf (cov, sizeof (cov), "coverage 0 (wind below 4.47 m/s), lace mean %.3f", laceMean);
	oapiWriteLogV ("VulkanClient: %swhitecaps on %s: wind %.1f m/s, %s; shore band %.1f m (Hs %.2f m), bores every %.1f m / %.1f s",
		prefix, wavesName, sea.wind, cov, f.shoreW, f.Hs, f.shoreL, f.shoreT);
}

void Atmosphere::UpdateWaves (const VECTOR3 &cam, double camR, float prm[4], float data[][4], float env[][4])
{
	static const bool wlog = getenv ("ORBITER_VK_WAVE_LOG") && atoi (getenv ("ORBITER_VK_WAVE_LOG"));
	const double twoPi = 2.0 * PI;
	const double t = oapiGetSimTime ();
	if (!waveAnchored || length (cam - waveAnchor) > 100e3) {
		double moved = waveAnchored ? length (cam - waveAnchor) : 0.0;
		waveAnchored = true;
		waveAnchor = cam;
		VECTOR3 up = unit (cam);
		waveT1 = unit (crossp (up, fabs (up.y) > 0.9 ? _V(1, 0, 0) : _V(0, 1, 0)));
		waveT2 = crossp (up, waveT1);
		// the sea state follows the wind here. It changes only at (re)anchoring,
		// so the pattern does not swim; a time-varying wind would have to
		// re-anchor or blend two spectra.
		double lng = atan2 (cam.z, cam.x), lat = asin (std::min (std::max (cam.y / length (cam), -1.0), 1.0));
		// the wind 10 m above the water (code review M1: not above the ground under the camera, which
		// gave every sea the wind of the hills flown over): seas at their level; an ice world's open
		// water follows the terrain, so its level is where the camera is over it (elsewhere the sea
		// state stays as it was)
		double elev = 0.0;
		bool overWater = true;
		if (!strcmp (wavesWhy, "ice open water")) {
			vksurf::Point pt = vksurf::Query (scene, hPlanet, lng, lat);
			overWater = pt.water > 0.5;
			elev = pt.elev;
		}
		if (overWater || !seaSet) sea = SeaState (lng, lat, elev), seaSet = true;
		bool foamRan = false;
		if (foam) wfoam = FoamState (sea, &foamRan);
		VECTOR3 east = _V(-sin (lng), 0.0, cos (lng));
		VECTOR3 north = _V(-sin (lat) * cos (lng), cos (lat), -sin (lat) * sin (lng));
		seaWindDir = unit (east * sin ((double)sea.bearing) + north * cos ((double)sea.bearing));
		sea.azimuth = (float)atan2 (dotp (seaWindDir, waveT2), dotp (seaWindDir, waveT1));
		// the sea near the camera (W3): its spectrum for this sea state, the drawn share of its slopes
		if (oceanOn) {
			uint32_t seed = 2166136261u;
			for (const char *c = wavesName; *c; c++) seed = (seed ^ (uint8_t)*c) * 16777619u;
			bool ok = ocean.Setup (sea.wind, sea.azimuth, waveG, 60.0 * sea.lambdaScale, std::max (sea.varTotal - sea.varUnres, 0.0f), seed);
			oceanT1 = waveT1, oceanT2 = waveT2;
			if (wlog) oapiWriteLogV ("VulkanClient: waves on %s: sea field %s, patches %.1f / %.2f m, mean-square slope %.4f / %.4f",
				wavesName, ok ? "on" : "off (calm)", ocean.L[0], ocean.L[1], ocean.var[0], ocean.var[1]);
		}
		if (wlog) {
			double l0 = 60.0 * sea.lambdaScale;
			oapiWriteLogV ("VulkanClient: waves on %s: re-anchored (moved %.1f km), sea: elevation %.0f m, wind %.1f m/s (%s) toward %03.0f deg, azimuth %.1f deg from t1, wavelengths %.1f-%.2f m, slope %.4f",
				wavesName, moved * 1e-3, elev, sea.wind, sea.src, fmod (sea.bearing * 180.0 / PI + 360.0, 360.0),
				sea.azimuth * 180.0 / PI, l0, l0 * pow (0.7, WAVE_COUNT - 1), sea.slope);
			if (foam && foamRan) LogFoam ("re-anchored: ");
		}
	}
	VECTOR3 d = cam - waveAnchor;
	VECTOR3 dir0 = _V(0, 0, 0);
	double k0 = 0.0, om0 = 0.0;
	for (int i = 0; i < WAVE_COUNT; i++) {
		double lambda = 60.0 * sea.lambdaScale * pow (0.7, i);
		double ang = sea.azimuth + (WAVE_SPREAD0 + WAVE_SPREAD1 * i / (WAVE_COUNT - 1.0)) * sin (i * 2.39996 + 0.3 * sin (i * 7.1));
		VECTOR3 dir = waveT1 * cos (ang) + waveT2 * sin (ang);
		double k = twoPi / lambda, omega = sqrt (waveG * k + SIGMA_RHO * k * k * k);
		double phi = fmod (k * dotp (d, dir) - omega * t + 1.7 * i, twoPi);
		if (phi < 0.0) phi += twoPi;
		data[i][0] = (float)(k * dir.x), data[i][1] = (float)(k * dir.y), data[i][2] = (float)(k * dir.z);
		data[i][3] = (float)phi;
		// the crest envelope: across the wave, anchored as the wave is
		VECTOR3 kp = (waveT2 * cos (ang) - waveT1 * sin (ang)) * (WAVE_ENV_K * k);
		double psi = fmod (dotp (d, kp) + 2.3 * i + 0.5, twoPi);
		if (psi < 0.0) psi += twoPi;
		env[i][0] = (float)kp.x, env[i][1] = (float)kp.y, env[i][2] = (float)kp.z, env[i][3] = (float)psi;
		if (i == 0) dir0 = dir, k0 = k, om0 = omega;
	}
	prm[0] = sea.slope, prm[1] = (float)waveG, prm[2] = sea.lambdaScale, prm[3] = 0.0f;

	if (wlog && camR - radius < 10e3 && t - waveLogT >= 2.0) {
		waveLogT = t;
		VECTOR3 Pref = waveAnchor + waveT1 * 37.0 + waveT2 * 11.0;
		float rx = (float)(Pref.x - cam.x), ry = (float)(Pref.y - cam.y), rz = (float)(Pref.z - cam.z);
		float got = rx * data[0][0] + ry * data[0][1] + rz * data[0][2] + data[0][3];
		double want = k0 * dotp (Pref - waveAnchor, dir0) - om0 * t;
		double err = remainder ((double)got - want, twoPi);
		oapiWriteLogV ("VulkanClient: waves on %s: alt %.0f m, anchor %.2f km away, wave0 phase error %.1e rad, wave0.wind %.3f",
			wavesName, camR - radius, length (d) * 1e-3, err, dotp (dir0, seaWindDir));
	}
}

size_t Atmosphere::OceanStageBytes ()
{
	size_t texels = 0;
	for (int m = 0; m <= OceanFft::MIPS; m++) texels += (size_t)(OceanFft::N >> m) * (OceanFft::N >> m);
	texels += (size_t)OceanFft::N * OceanFft::N;
	return texels * 4; // (RG16F)
}

void Atmosphere::UploadOcean (VkCommandBuffer cmd, const VECTOR3 &camGlobal)
{
	// (the camera's planet: waves on, their spectrum set at an anchor near the camera)
	const bool *rip = (const bool*)scene->Client()->GetConfigParam (CFGPRM_SURFACERIPPLE);
	static int dbg = 0;
	auto why = [&] (const char *w) { if (getenv ("ORBITER_VK_WAVE_LOG") && dbg++ < 5) oapiWriteLogV ("VulkanClient: sea field upload skipped: %s", w); };
	if (!oceanOn || !waves || (rip && !*rip)) { why ("off"); return; }
	if (!ocean.Valid () || !waveAnchored) { why ("no spectrum or anchor"); return; }
	VECTOR3 ppos;
	scene->ObservedPos (hPlanet, ppos);
	if (length (camGlobal - ppos) - radius > 50e3) { why ("camera far"); return; }
	uint32_t f = ctx->FrameIndex ();
	if (!oceanImg[f][0].image) { why ("no images"); return; }
	if (oceanScratch.size () != OceanFft::Floats ()) oceanScratch.resize (OceanFft::Floats ());
	ocean.Evaluate (oapiGetSimTime (), oceanScratch.data ());
	// float -> half (the slopes are small: no infinities, denormals flushed)
	auto half = [] (float v) -> uint16_t {
		uint32_t x; memcpy (&x, &v, 4);
		uint32_t sgn = (x >> 16) & 0x8000u;
		int e = (int)((x >> 23) & 0xff) - 127 + 15;
		uint32_t m = x & 0x7fffffu;
		if (e <= 0) return (uint16_t)sgn;
		if (e >= 31) return (uint16_t)(sgn | 0x7bffu);
		uint32_t h = sgn | ((uint32_t)e << 10) | (m >> 13);
		if (m & 0x1000u) h++; // (round)
		return (uint16_t)h;
	};
	uint16_t *dst = (uint16_t*)oceanStage[f].mapped;
	const float *src = oceanScratch.data ();
	const size_t NN = (size_t)OceanFft::N * OceanFft::N;
	// staging: the long layer's levels 0..MIPS, then the ripples
	size_t o = 0;
	auto put = [&] (const float *p, size_t n) { for (size_t i = 0; i < 2 * n; i++) dst[o + i] = half (p[i]); o += 2 * n; };
	put (src, NN);
	const float *mp = src + 2 * OceanFft::LAYERS * NN;
	for (int m = 1; m <= OceanFft::MIPS; m++) {
		size_t n = (size_t)(OceanFft::N >> m) * (OceanFft::N >> m);
		put (mp, n);
		mp += 2 * n;
	}
	size_t rippleOfs = o;
	put (src + 2 * NN, NN);
	// the copies (the images are this frame's: the GPU is done with them)
	for (int k = 0; k < 2; k++)
		vkc::ImageBarrier (cmd, oceanImg[f][k].image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkBufferImageCopy rg[OceanFft::MIPS + 1]{};
	VkDeviceSize ofs = 0;
	for (int m = 0; m <= OceanFft::MIPS; m++) {
		uint32_t n = OceanFft::N >> m;
		rg[m].bufferOffset = ofs;
		rg[m].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, 1 };
		rg[m].imageExtent = { n, n, 1 };
		ofs += (VkDeviceSize)n * n * 4;
	}
	vkCmdCopyBufferToImage (cmd, oceanStage[f].buffer, oceanImg[f][0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, OceanFft::MIPS + 1, rg);
	VkBufferImageCopy r1{};
	r1.bufferOffset = rippleOfs * 2;
	r1.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	r1.imageExtent = { (uint32_t)OceanFft::N, (uint32_t)OceanFft::N, 1 };
	vkCmdCopyBufferToImage (cmd, oceanStage[f].buffer, oceanImg[f][1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r1);
	for (int k = 0; k < 2; k++)
		vkc::ImageBarrier (cmd, oceanImg[f][k].image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	oceanFrame = ctx->FrameValue ();
}

float Atmosphere::SaltShare (double lat, double lng) const
{
	// (as terraindetail.glsl PlayaSalt)
	double d[3] = { cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng) }, s = 0.0;
	for (const auto &f : saltFlats) {
		double dx = d[0] - f[0], dy = d[1] - f[1], dz = d[2] - f[2];
		s = std::max (s, 1.0 - SmoothStep (0.8 * f[3], f[3], sqrt (dx * dx + dy * dy + dz * dz)));
	}
	return (float)s;
}

void Atmosphere::Update ()
{
	AtmUniforms u{};
	VECTOR3 ppos;
	MATRIX3 prot;
	scene->ObservedPos (hPlanet, ppos); // (light-time: where its light left it)
	oapiGetRotationMatrix (hPlanet, &prot);

	// view space (camera frame) -> planet frame
	MATRIX3 v2p = mul (transp (prot), scene->CamRot());
	Mat4::FromRotTrans (v2p, _V(0, 0, 0)).ToFloat (u.viewToPlanet);
	VECTOR3 cam = tmul (prot, scene->CameraPos() - ppos);
	double camR = length (cam);
	u.camPos[0] = (float)cam.x, u.camPos[1] = (float)cam.y, u.camPos[2] = (float)cam.z;
	u.camPos[3] = (float)camR;
	u.camC[0] = (float)((camR - radius) * (camR + radius));
	u.camC[1] = (float)((camR - atmRadius) * (camR + atmRadius));

	// (the scene's light: the Sun, or the star of another system)
	VECTOR3 sun = _V(1, 0, 0);
	if (scene->Light ().flux > 0.0) sun = unit (tmul (prot, scene->Light ().pos - ppos));
	u.sunDir[0] = (float)sun.x, u.sunDir[1] = (float)sun.y, u.sunDir[2] = (float)sun.z;
	memcpy (u.betaR, betaR, sizeof(betaR));
	memcpy (u.betaM, betaM, sizeof(betaM));
	u.radii[0] = (float)radius;
	u.radii[1] = (float)atmRadius;
	u.radii[2] = mieG;
	static const float skyScale = getenv ("ORBITER_VK_SKY_INTENSITY") ? (float)atof (getenv ("ORBITER_VK_SKY_INTENSITY")) : 0.0f;
	u.radii[3] = skyScale > 0.0f ? skyScale : sunIntensity;
	u.misc[0] = present ? 1.0f : 0.0f;
	u.misc[3] = airglow;
	memcpy (u.detail, detail, sizeof(detail));
	memcpy (u.aurora, aurora, sizeof(aurora));
	memcpy (u.ozone, ozone, sizeof(ozone));
	memcpy (u.overcastSun, overcastSun, sizeof(overcastSun));
	memcpy (u.overcastPrm, overcastPrm, sizeof(overcastPrm));
	memcpy (u.nearHaze, nearHaze, sizeof(nearHaze));
	memcpy (u.ground, groundColour, sizeof(groundColour));
	if (cloudParams[3] > 0.0f)
		if (const double *rot = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDROTATION))
			cloudParams[1] = (float)*rot;
	memcpy (u.cloud, cloudParams, sizeof(cloudParams));
	// origin of world-anchored surface patterns (waves, terrain detail): the
	// camera position snapped to 1 km, passed relative to the camera (small
	// values, no loss of precision)
	VECTOR3 origin = _V(std::floor (cam.x / 1000.0), std::floor (cam.y / 1000.0), std::floor (cam.z / 1000.0)) * 1000.0;
	VECTOR3 rel = cam - origin;
	u.water[0] = (float)rel.x, u.water[1] = (float)rel.y, u.water[2] = (float)rel.z;
	u.water[3] = (float)fmod (oapiGetSimTime (), 3600.0); // (the aurora's clock: also without waves, code review n2)
	// Settings > Scenery > Ripples (the core's EnableSpecularRipples, on by default): the wind's waves,
	// the sea state, the whitecaps and the surf; read every frame, so the toggle takes effect at once
	// (the jets' disturbances, WaterFx, stay)
	const bool *rip = (const bool*)scene->Client()->GetConfigParam (CFGPRM_SURFACERIPPLE);
	const bool wv = waves && (!rip || *rip);
	// the shore (F1): the wet sand the swash reaches, about half the surf band (wider with the sea);
	// the shallows' tint where the sea's colour is made (procedural worlds); the masks' distance field
	u.coastPrm[0] = wv && present ? (float)std::max (0.5 * wfoam.shoreW, 4.0) : 0.0f;
	u.coastPrm[1] = wv && procSea ? 1.0f : 0.0f;
	u.coastPrm[2] = vkshore::Enabled () ? vkshore::SD_MAX : 0.0f;
	static const bool seaIceOff = [] { const char *e = getenv ("ORBITER_VK_SEA_ICE"); return e && *e && !atoi (e); }();
	u.coastPrm[3] = procOcean && !seaIceOff ? 1.0f : 0.0f; // (sea ice, I1; ORBITER_VK_SEA_ICE=0: off)
	// (the waves show within 3 km of the surface: not computed for planets far
	// off, which also frees the anchor for the next approach)
	if (wv && camR - radius < 50e3) UpdateWaves (cam, camR, u.wavePrm, u.waves, u.waveEnv);
	else waveAnchored = false;
	// the sea field (W3): its uniforms when UploadOcean wrote this frame's images
	if (wv && oceanFrame == ctx->FrameValue ()) { // (UploadOcean wrote this frame's images)
		VECTOR3 d = cam - waveAnchor;
		u.oceanT1[0] = (float)oceanT1.x, u.oceanT1[1] = (float)oceanT1.y, u.oceanT1[2] = (float)oceanT1.z, u.oceanT1[3] = (float)ocean.L[0];
		u.oceanT2[0] = (float)oceanT2.x, u.oceanT2[1] = (float)oceanT2.y, u.oceanT2[2] = (float)oceanT2.z, u.oceanT2[3] = (float)ocean.L[1];
		u.oceanD[0] = (float)d.x, u.oceanD[1] = (float)d.y, u.oceanD[2] = (float)d.z, u.oceanD[3] = 1.0f;
		u.oceanVar[0] = (float)ocean.mipVar[0], u.oceanVar[1] = (float)ocean.var[1];
		for (int m = 0; m < 6; m++) u.oceanMipVar[m / 4][m % 4] = (float)ocean.mipVar[std::min (m, OceanFft::MIPS)];
	}
	if (wv) u.seaPrm[0] = sea.varUnres, u.seaPrm[1] = sea.varTotal, u.seaPrm[2] = sea.wind;
	if (wv && foam) {
		u.seaPrm[3] = wfoam.cover;
		u.foamPrm[0] = wfoam.thresh, u.foamPrm[1] = wfoam.sigma, u.foamPrm[2] = wfoam.mean, u.foamPrm[3] = laceMean;
		double ph = fmod (oapiGetSimTime () / (double)wfoam.shoreT, 1.0);
		if (ph < 0.0) ph += 1.0;
		u.shorePrm[0] = wfoam.shoreW, u.shorePrm[1] = wfoam.shoreL, u.shorePrm[2] = (float)ph, u.shorePrm[3] = wfoam.shoreOn;
	}
	if (WaterFx *wf = scene->Water ()) if (camR - radius < 50e3) wf->Pack (hPlanet, cam, u.waterFx, u.waterFxSrc);
	memcpy (u.mieLobe, mieLobe, sizeof(mieLobe));
	if (scene->CloudsActive () && hPlanet == oapiCameraProxyGbody ()) {
		const VolumetricClouds *vc = scene->VClouds ();
		memcpy (u.cloudMap, vc->shadowMap, sizeof(u.cloudMap));
		memcpy (u.cloudNoise, vc->shadowNoise, sizeof(u.cloudNoise));
		memcpy (u.cloudShell, vc->shadowShell, sizeof(u.cloudShell));
	}
	{
		SURFDETAILPARAM dp;
		if (oapiSurfaceDetailParamsEx (hPlanet, &dp, sizeof (dp))) {
			u.detailPrm[0] = (float)dp.ridge, u.detailPrm[1] = (float)dp.craters;
			u.detailPrm[2] = (float)dp.craterDepth, u.detailPrm[3] = (float)dp.craterFresh;
			u.pebShape[0] = (float)dp.pebbleSize, u.pebShape[1] = (float)dp.pebbleRound;
			u.pebShape[2] = (float)dp.pebbleElong, u.pebShape[3] = (float)dp.pebbleHeight;
			u.pebShape2[0] = (float)dp.pebbleBevel, u.pebShape2[1] = (float)dp.pebbleTilt;
			u.pebShape2[2] = (float)dp.pebbleCrag, u.pebShape2[3] = (float)dp.pebbleTone;
			u.pebPrm[0] = (float)dp.pebbleAlbedo, u.pebPrm[1] = (float)dp.grit;
			u.pebPrm[2] = (float)dp.micro, u.pebPrm[3] = dp.pebbleGate ? 1.0f : 0.0f;
			static const bool playaOff = [] { const char *e = getenv ("ORBITER_VK_PLAYA"); return e && e[0] == '0'; }();
			u.playaPrm[0] = (playaOff || dp.playa <= 0.0) ? 0.0f : 1.0f; // (the weight itself comes per vertex)
			u.playaPrm[1] = u.playaPrm[2] = u.playaPrm[3] = 0.0f;
			memset (u.saltFlat, 0, sizeof (u.saltFlat));
			if (u.playaPrm[0] > 0.0f) {
				char nm[64];
				oapiGetObjectName (hPlanet, nm, 64);
				const std::vector<SaltFlat> &sf = PlayaCfgSalt (nm);
				int n = std::min ((int)sf.size (), SALT_MAX);
				if (saltFlats.size () != (size_t)n) {
					saltFlats.resize (n);
					for (int i = 0; i < n; i++) {
						double la = sf[i].lat * RAD, lo = sf[i].lng * RAD;
						saltFlats[i] = { cos (la) * cos (lo), sin (la), cos (la) * sin (lo), sf[i].radius * 1e3 / oapiGetSize (hPlanet) };
					}
				}
				for (int i = 0; i < n; i++)
					for (int k = 0; k < 4; k++) u.saltFlat[i][k] = (float)saltFlats[i][k];
				u.playaPrm[1] = (float)n;
			}
			// site materials (F3); ORBITER_VK_SITE=0: not in the shading (the meshes keep the core's mask)
			static const bool siteOff = [] { const char *e = getenv ("ORBITER_VK_SITE"); return e && e[0] == '0'; }();
			memset (u.sitePrm, 0, sizeof (u.sitePrm)), memset (u.siteThr, 0, sizeof (u.siteThr)), memset (u.siteCover, 0, sizeof (u.siteCover)), memset (u.siteMat, 0, sizeof (u.siteMat));
			if (dp.site && !siteOff) {
				float kmax = 1.0f;
				for (int i = 0; i < 3; i++) kmax = std::max (kmax, (float)dp.sitePebbles[i]);
				u.sitePrm[0] = 1.0f, u.sitePrm[1] = (float)dp.siteSeed, u.sitePrm[2] = (float)dp.siteEdge, u.sitePrm[3] = kmax;
				for (int i = 0; i < 3; i++) {
					u.siteThr[i] = (float)dp.siteThresh[i], u.siteCover[i] = (float)dp.siteCover[i];
					for (int c = 0; c < 3; c++) u.siteMat[3 * i][c] = (float)dp.siteTint[i][c], u.siteMat[3 * i + 1][c] = (float)dp.siteAbs[i][c];
					u.siteMat[3 * i][3] = (float)dp.siteAbs[i][3];
					u.siteMat[3 * i + 1][3] = (float)dp.siteGrit[i];
					u.siteMat[3 * i + 2][0] = (float)dp.sitePebbles[i], u.siteMat[3 * i + 2][1] = (float)dp.siteCrack[i];
					u.siteMat[3 * i + 2][2] = (float)dp.siteSmooth[i], u.siteMat[3 * i + 2][3] = (float)dp.siteRocks[i];
				}
			}
		} else {
			u.detailPrm[0] = u.detailPrm[1] = u.detailPrm[2] = u.detailPrm[3] = 0.0f;
			memset (u.pebShape, 0, sizeof (u.pebShape)), memset (u.pebShape2, 0, sizeof (u.pebShape2)), memset (u.pebPrm, 0, sizeof (u.pebPrm)), memset (u.playaPrm, 0, sizeof (u.playaPrm));
			memset (u.sitePrm, 0, sizeof (u.sitePrm)), memset (u.siteThr, 0, sizeof (u.siteThr)), memset (u.siteCover, 0, sizeof (u.siteCover)), memset (u.siteMat, 0, sizeof (u.siteMat));
		}
	}
	u.surfPrm[0] = surfSaturation;
	for (int i = 0; i < 3; i++) u.surfPrm[1 + i] = overcastSky[i] / sunIntensity; // (deep in it: the skylight's radiance)
	{
		VECTOR3 bv = scene->RelBetaView (); // (relativistic view: the camera's velocity)
		u.relBeta[0] = (float)bv.x, u.relBeta[1] = (float)bv.y, u.relBeta[2] = (float)bv.z;
	}
	{
		// (the sky's light in the star's colour, as the surfaces': Scene's
		// sunColor without its brightness)
		float c[3];
		Scene::StarDiscRGB (scene->Light ().teff, c);
		double lum = std::max (1e-6, 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]);
		for (int k = 0; k < 3; k++) u.sunTint[k] = (float)(c[k] / lum);
		u.sunTint[3] = 1.0f;
	}
	u.misc[1] = (float)(1.0 / scene->ProjX());
	u.misc[2] = (float)(1.0 / scene->ProjY());
	memcpy (ubo[SetIndex ()].mapped, &u, sizeof(u));

	// the sky pass is needed when the atmosphere covers a noticeable part of the view
	visible = present && scene->PixelSize (atmRadius, length (scene->CameraPos() - ppos)) > 2.0;
}

void Atmosphere::RenderSky ()
{
	if (!visible) return;
	AtmosphereRenderer *ar = scene->Atm();
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ar->SkyPipeline());
	VkDescriptorSet s = Set ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ar->SkyLayout(), 0, 1, &s, 0, nullptr);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}
