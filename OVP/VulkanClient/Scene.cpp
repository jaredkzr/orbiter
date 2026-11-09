// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Scene.h"
#include "GfxSettings.h"
#include "AO.h"
#include "SSR.h"
#include "RayTracing.h"
#include "RTLighting.h"
#include "TAA.h"
#include "Fsr.h"
#include "Clouds.h"
#include "GroundTracks.h"
#include "EnvProbe.h"
#include "VObjects.h"
#include "PlanetSurface.h"
#include <chrono>
#include <random>
#include <set>
#include "VObjects.h"
#include "VkMesh.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "StarSphere.h"
#include "StarField.h"
#include "StarCatalogue.h"
#include "GalaxySky.h"
#include "BlackHoleLens.h"
#include "../../Src/Orbiter/Kerr.h"
#include "Particles.h"
#include "ParticleStream.h"
#include "Plasma.h"
#include "PlanetSurface.h"
#include "Atmosphere.h"
#include "Shadows.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "mesh.vert.h"
#include "mesh.frag.h"
#include "ring.vert.h"
#include "ring.frag.h"
#include "star.frag.h"
#include "ringshadow.frag.h"
#include "prepass.vert.h"
#include "prepass_inst.vert.h"
#include "mesh_inst.vert.h"
#include "impostor_inst.vert.h"
#include "prepass_impostor_inst.vert.h"
#include "building.vert.h"
#include "building.frag.h"
#include "prepass_building.vert.h"
#include "lamp.vert.h"
#include "prepass.frag.h"
#include "particle.vert.h"
#include "particle.frag.h"

namespace {

	// Must match the Frame uniform block in frame.glsl
	struct FrameUniforms {
		float proj[16];
		float sunDir[4];
		float sunColor[4];
		float ambient[4];
		float shadowMat[3][16];   // view space -> shadow map, per cascade
		float shadowParams[3][4]; // enabled, normal offset, depth bias, texel size
		float jitter[4];       // projection jitter (NDC), render size
		float aoParams[4];     // 1/width, 1/height of the occlusion image, fraction applied to direct light, w: 1 = no occlusion
		float viewToGlobal[16];// view space -> global frame (rotation), for the environment probe
		float probe[4];        // x: environment probe valid, y: highest mip level, z: screen-space reflections valid, w: their debug view
		float rt[4];           // x: ray traced lighting valid, y: shadowed local lights
		float planetDir[4];    // direction to the planet nearby (view), w: sine of its angular radius
		float planetLight[4];  // its irradiance at normal incidence, w: present
		float lightCount[4];
		float city[4][4];      // the cities' road map (CityField): centre (view), w: present; axes x, y (view); 1/size of its levels, w: the camera's altitude above the ground [m]
		float relBeta[4];      // relativistic view: the camera's velocity (view, units of c), w: on
		float relSelf[4];      // the camera's own vessel: centre (view), radius
		float relSunDir[4];    // the sunlight on it in its frame: direction (view, aberrated)
		float relSunColor[4];  //   colour (Doppler shifted)
		float lens[4];         // a black hole's lens: centre (view), mass [m] (0: none)
		struct {
			float pos[4];   // view space position, range
			float dir[4];   // view space direction, type (1 point, 2 spot)
			float col[4];   // colour times intensity
			float att[4];   // attenuation coefficients, cos(umbra/2)
			float spot[4];  // cos(penumbra/2)
		} lights[MAX_LIGHTS];
	};

	// shadows are cast within this range of the camera target, in units of
	// the camera distance (plus the target's size)
	// a black hole's lens is traced while the hole lies within this many
	// Einstein rings of the view (always, once its ring is this wide [rad])
	constexpr double LENS_VIEW_MARGIN = 10.0, LENS_VIEW_ALWAYS = 0.1;
	constexpr double COSMIC_BG_DOPPLER = 1.5; // the extragalactic background drawn where seen at a larger Doppler factor (at rest: ~1/30 of the darkest sky)
	constexpr double SHADOW_RANGE = 1.5;
	constexpr double SHADOW_RADIUS_MIN = 20.0, SHADOW_RADIUS_MAX = 5000.0; // [m]
	// cascades around the camera [m]; each is used if the previous one is
	// less than half its size
	constexpr double SHADOW_CAM_RADIUS[2] = { 600.0, 4000.0 };

	// Distance up to which vessel visuals are kept, in units of the vessel size
	constexpr double VESSEL_VIS_RANGE = 5e3;
	constexpr double VESSEL_VIS_MIN = 1e5; // [m]

	void MulMat4f (const float *a, const float *b, float *r) // column-major r = a*b
	{
		for (int c = 0; c < 4; c++)
			for (int rr = 0; rr < 4; rr++) {
				float s = 0.0f;
				for (int k = 0; k < 4; k++) s += a[k*4 + rr] * b[c*4 + k];
				r[c*4 + rr] = s;
			}
	}

} // namespace

Scene::Scene (VulkanClient *client) : gc(client)
{
}

Scene::~Scene ()
{
	Shutdown ();
}

VkCommandBuffer Scene::Cmd () const
{
	return ctx->Cmd();
}

bool Scene::Init ()
{
	ctx = gc->Ctx();
	// the graphics settings that apply when a session starts
	ctx->Reconfigure (vkc::Gfx ().renderScale, vkc::Gfx ().SceneSamples ());
	vkc::Gfx ().MarkSessionStart ();
	if (ctx->deviceProps.limits.maxPushConstantsSize < sizeof(MeshPushConstants))
		oapiWriteLogError ("VulkanClient: device supports only %u bytes of push constants (%zu required)",
			ctx->deviceProps.limits.maxPushConstantsSize, sizeof(MeshPushConstants));

	for (auto &sm : shadows) {
		sm = std::make_unique<ShadowMap>();
		sm->Init (ctx, gc->TexMgr()->Layout());
	}

	// per-frame uniform buffer, the shadow maps, ambient occlusion, the
	// environment probe and screen-space reflections (set 0)
	tracks = std::make_unique<GroundTracks>();
	tracks->Init (ctx);
	VkDescriptorSetLayoutBinding b[9]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // (ray traced reflections)
	b[1].binding = 1;
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b[1].descriptorCount = SHADOW_CASCADES;
	b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b[2] = b[1];
	b[2].binding = 3; // ambient occlusion
	b[2].descriptorCount = 1;
	b[3] = b[2];
	b[3].binding = 4; // environment probe
	b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	b[4] = b[2];
	b[4].binding = 5; // screen-space reflections
	b[5] = b[2];
	b[5].binding = 6; // ray traced lighting: sun, ambient occlusion, planet light
	b[6] = b[2];
	b[6].binding = 7; // ray traced lighting: local lights
	b[7].binding = 8; // ground tracks (tile shading)
	b[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	b[7].descriptorCount = 1;
	b[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b[8] = b[2];
	b[8].binding = 9; // the cities' road map: fine and coarse level, and the far built-up map (tile shading)
	b[8].descriptorCount = 3;
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 9;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &frameLayout));

	VkDescriptorPoolSize ps[3] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (SHADOW_CASCADES + 8) * vkc::FRAMES_IN_FLIGHT }, // (+1: the cities' far map)
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * vkc::FRAMES_IN_FLIGHT }
	};
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2 * vkc::FRAMES_IN_FLIGHT;
	pci.poolSizeCount = 3;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &framePool));
	probe = std::make_unique<EnvProbe>();
	probe->Init (ctx);
	if (const char *e = getenv ("ORBITER_VK_PROBE")) probeEnabled = atoi (e) != 0;
	if (ctx->sceneSamples != VK_SAMPLE_COUNT_1_BIT) probeEnabled = false; // (pipelines are multisampled)

	// frame sets of the main view and of the environment probe
	for (uint32_t j = 0; j < 2 * vkc::FRAMES_IN_FLIGHT; j++) {
		uint32_t i = j % vkc::FRAMES_IN_FLIGHT;
		bool forProbe = j >= vkc::FRAMES_IN_FLIGHT;
		vkc::Buffer &ubo = forProbe ? probeFrameUbo[i] : frameUbo[i];
		VkDescriptorSet &set = forProbe ? probeFrameSet[i] : frameSet[i];
		ubo = ctx->CreateBuffer (sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		memset (ubo.mapped, 0, sizeof(FrameUniforms));
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = framePool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &frameLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));
		VkDescriptorBufferInfo dbi{ ubo.buffer, 0, sizeof(FrameUniforms) };
		VkDescriptorImageInfo dii[SHADOW_CASCADES];
		for (int k = 0; k < SHADOW_CASCADES; k++)
			dii[k] = { shadows[k]->Sampler(), shadows[k]->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo pii{ probe->Sampler(), probe->CubeView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorBufferInfo tbi{ tracks->Buffer (i), 0, GroundTracks::BUFFER_SIZE };
		VkWriteDescriptorSet wr[4]{};
		for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.dstSet = set;
		wr[3].dstBinding = 8;
		wr[3].descriptorCount = 1;
		wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		wr[3].pBufferInfo = &tbi;
		wr[0].dstBinding = 0;
		wr[0].descriptorCount = 1;
		wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wr[0].pBufferInfo = &dbi;
		wr[1].dstBinding = 1;
		wr[1].descriptorCount = SHADOW_CASCADES;
		wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[1].pImageInfo = dii;
		wr[2].dstBinding = 4;
		wr[2].descriptorCount = 1;
		wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[2].pImageInfo = &pii;
		vkUpdateDescriptorSets (ctx->device, 4, wr, 0, nullptr);
	}

	// the cities' road map: none yet (a black texel), sampled with mip levels
	{
		const uint8_t none[4] = { 0, 0, 0, 0 };
		cityNone = gc->TexMgr()->CreateRGBALinear (1, 1, none);
		VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.maxLod = VK_LOD_CLAMP_NONE;
		sci.anisotropyEnable = ctx->deviceFeatures.samplerAnisotropy;
		sci.maxAnisotropy = 8.0f;
		VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &citySampler));
		for (auto &b : cityBound) b[0] = b[1] = b[2] = nullptr;
	}

	ao = std::make_unique<AmbientOcclusion>();
	ao->Init (ctx);
	ssr = std::make_unique<ScreenReflections>();
	ssr->Init (ctx);
	ssr->EnsureSize (ao->PrepassView (), ao->MaterialView (), ao->VelocityView ());
	ssr->BeginFrame ();
	WriteSSRDescriptors ();
	taaEnabled = vkc::Gfx ().TemporalAA ();
	taa = std::make_unique<TemporalAA>();
	taa->Init (ctx);
	// temporal upscaler: FSR 3 if built in and set (GfxSettings); it needs a
	// single-sampled scene
	fsr = std::make_unique<FsrUpscaler>();
	useFsr = taaEnabled && FsrUpscaler::Available () && ctx->sceneSamples == VK_SAMPLE_COUNT_1_BIT && vkc::Gfx ().UseFsr ();
	if (useFsr) fsr->Init (ctx);
	oapiWriteLogV ("VulkanClient: temporal anti-aliasing: %s", !taaEnabled ? "off" : useFsr ? "FSR 3" : "TAA");
	WriteAODescriptors ();

	atm = std::make_unique<AtmosphereRenderer>();
	atm->Init (ctx, gc->TexMgr());
	vclouds = std::make_unique<VolumetricClouds>();
	vclouds->Init (ctx, gc->TexMgr(), atm->SetLayout());
	atm->SetCloudVolume (vclouds->CoverageView (), vclouds->ClampSampler (), vclouds->ShapeNoiseView (), vclouds->RepeatSampler ());
	// ray traced reflections (hardware ray queries)
	rt = std::make_unique<RTScene>();
	if (rt->Init (ctx, gc->TexMgr()) && ssr->enabled && vkc::Gfx ().rtReflections) {
		ssr->InitRT (frameLayout, rt->SetLayout(), rt->TextureLayout(), atm->SetLayout());
		gc->TexMgr()->onDestroy = [this](VkTexture *t) { if (rt) rt->ForgetTexture (t); };
	}
	// ray traced shadows, ambient occlusion and planet light
	rtl = std::make_unique<RTLighting>();
	if (rt->Enabled ()) rtl->Init (ctx, frameLayout, rt->SetLayout(), rt->TextureLayout());
	rtl->EnsureSize (ao->PrepassView (), ao->VelocityView (), ao->MaterialView ());
	WriteRTLightDescriptors ();
	CreatePipelines ();
	BuildSphere ();
	tiles = std::make_unique<TileRenderer>();
	tiles->Init (ctx, gc->TexMgr(), frameLayout, atm->SetLayout());
	exhaustTex = gc->TexMgr()->Load ("Exhaust.dds");
	plumes = std::make_unique<PlumeRenderer>();
	plumes->Init (ctx, frameLayout, gc->TexMgr()->Layout(), atm->SetLayout(), vclouds->ShapeNoiseView (), vclouds->DetailNoiseView ());
	ssr->SetPlumes (plumes->Buffer (), plumes->BufferSize (), plumes->ShapeNoise (), plumes->DetailNoise ());
	plasma = std::make_unique<PlasmaRenderer>();
	plasma->Init (ctx, frameLayout);
	particles = std::make_unique<ParticleSystem>();
	particles->Init (ctx, gc->TexMgr(), frameLayout, atm->SetLayout(), gc->TexMgr()->Layout(), vclouds->ShapeNoiseView (), vclouds->DetailNoiseView ());
	if (rt->Enabled ()) particles->InitRT (frameLayout, rt->SetLayout(), rt->TextureLayout(), atm->SetLayout());
	plumes->rayCast = [this](uint32_t owner, const VECTOR3 &vo, const VECTOR3 &vd, double maxDist, double &t, VECTOR3 &vn, int *kind) {
		VECTOR3 o = camPos + mul (camRot, vo), d = mul (camRot, vd);
		bool found = false;
		t = maxDist;
		for (auto &it : visuals) {
			if (it.second->Id () == owner) continue;
			double ti;
			VECTOR3 ni;
			if (it.second->RayHit (o, d, t, ti, ni)) {
				t = ti, vn = tmul (camRot, ni), found = true;
				if (kind) *kind = it.second->LastHitKind ();
			}
		}
		return found;
	};
	{
		// glow sprite: bright core with a soft halo, zero at the edge (half
		// floats: a bright star's glow is this scaled far up, where 8 bits'
		// steps showed as rings)
		const int N = 256;
		auto half = [](float f) -> uint16_t { // (0 to 1: normal or zero)
			if (f < 6.2e-5f) return 0;
			uint32_t u;
			memcpy (&u, &f, 4);
			return (uint16_t)((((u >> 23) & 0xff) - 112) << 10 | ((u >> 13) & 0x3ff));
		};
		std::vector<uint16_t> px (N * N * 4);
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++) {
				double dx = (x + 0.5) / N * 2.0 - 1.0, dy = (y + 0.5) / N * 2.0 - 1.0;
				double r = std::sqrt (dx*dx + dy*dy);
				double a = r < 1.0 ? (0.75 * std::exp (-r*r*40.0) + 0.25 * std::pow (1.0 - r, 3.0)) : 0.0;
				uint16_t *p = &px[(y*N + x) * 4];
				p[0] = p[1] = p[2] = half (1.0f);
				p[3] = half ((float)std::clamp (a, 0.0, 1.0));
			}
		glowTex = gc->TexMgr()->CreateRGBAHalf (N, N, px.data());
	}

	stars = std::make_unique<StarSphere> (gc);
	stars->Init (ctx);

	// the diffuse light of the Milky Way, seen from the camera's place in
	// the Galaxy. Its radiance relative to a sunlit white surface at 1 AU
	// (4.0e4 cd/m^2): 1 S10 (V = 27.78 mag/arcsec^2) is 8.4e-7 cd/m^2,
	// raised by the dark-adapted eye's gain (2^20.5, ORBITER_VK_SKY_GAIN: log2)
	{
		double g = 20.5;
		if (const char *e = getenv ("ORBITER_VK_SKY_GAIN")) g = atof (e);
		skyDarkGain = 2.1e-11 * std::exp2 (g);
	}
	// (the 3D star catalogue draws the stars brighter than the limit; the
	// Milky Way leaves them out. ORBITER_VK_SKY=0: no sky, for tests)
	skySettings.Load (gc);
	const char *noSky = getenv ("ORBITER_VK_SKY");
	std::unique_ptr<galactic::StarCatalogue> cat;
	if (!noSky || atoi (noSky)) cat = StarField::Load ();
	if (cat) galaxy = std::make_unique<GalaxySky> ();
	if (galaxy && galaxy->Init (ctx, SkyObserver (), *cat, skySettings)) {
		galactic::Observer o = SkyObserver ();
		starField = std::make_unique<StarField> ();
		if (!starField->Init (ctx, *galaxy, *cat, o, std::sqrt (o.pos[0] * o.pos[0] + o.pos[1] * o.pos[1] + o.pos[2] * o.pos[2]) < 0.01)) {
			oapiWriteLogError ("VulkanClient: the stars could not be set up");
			starField.reset ();
		}
		cat.reset ();
		// ORBITER_VK_SKY_PROBE=file.png: the baked sky's brightness in some
		// directions (log) and the whole sky as an image
		if (const char *e = getenv ("ORBITER_VK_SKY_PROBE")) {
			struct { const char *name; double l, b; } dirs[] = {
				{ "north Galactic pole", 0, 90 }, { "south Galactic pole", 0, -90 },
				{ "Galactic centre", 0, 0 }, { "Baade's window", 1.0, -3.9 }, { "Sgr star cloud", 7, -1.5 },
				{ "Scutum cloud", 27, -2 }, { "Cygnus", 77, 1 }, { "Cygnus rift", 70, 0 }, { "anticentre", 180, 0 },
				{ "Carina", 287, -1 }, { "Coalsack", 301, -1 }, { "Orion (b = -20)", 209, -19 }, { "l = 90, b = 30", 90, 30 } };
			std::vector<std::array<double, 2>> lb;
			for (auto &d : dirs) lb.push_back ({ d.l, d.b });
			std::vector<std::array<float, GalaxySky::NBAND>> v;
			if (galaxy->Probe (lb, v, e))
				for (size_t i = 0; i < v.size(); i++)
					oapiWriteLogV ("VulkanClient: sky at l = %5.1f, b = %5.1f (%s): S10 %.1f %.1f %.1f %.1f", dirs[i].l, dirs[i].b, dirs[i].name, v[i][0], v[i][1], v[i][2], v[i][3]);
		}
		// ORBITER_VK_SKY_CHECK=file (test): the baked sky against reference
		// values (lines: l b band S10 tolerance name; tolerance a fraction)
		// and the zodiacal light against the measured (within 30%)
		if (const char *e = getenv ("ORBITER_VK_SKY_CHECK")) {
			// (to the log and to the standard output: the test's)
			auto report = [](const char *fmt, auto... a) {
				char buf[512];
				snprintf (buf, sizeof(buf), fmt, a...);
				oapiWriteLogV ("VulkanClient: %s", buf);
				printf ("%s\n", buf);
				fflush (stdout);
			};
			struct Ref { double l, b, s10, tol; int band; std::string name; };
			std::vector<Ref> refs;
			std::ifstream ifs (e);
			std::string line;
			while (std::getline (ifs, line)) {
				if (line.empty () || line[0] == ';') continue;
				std::istringstream ss (line);
				Ref r;
				if (!(ss >> r.l >> r.b >> r.band >> r.s10 >> r.tol)) continue;
				std::getline (ss >> std::ws, r.name);
				refs.push_back (r);
			}
			std::vector<std::array<double, 2>> lb;
			for (auto &r : refs) lb.push_back ({ r.l, r.b });
			std::vector<std::array<float, GalaxySky::NBAND>> v;
			int pass = 0, n = (int)refs.size () + 1;
			if (refs.size () && galaxy->Probe (lb, v)) {
				for (size_t i = 0; i < refs.size (); i++) {
					double x = v[i][std::clamp (refs[i].band, 0, GalaxySky::NBAND - 1)];
					bool ok = std::abs (x / refs[i].s10 - 1.0) <= refs[i].tol;
					pass += ok;
					report ("sky check %s: %.1f S10 (reference %.1f +- %.0f%%) %s", refs[i].name.c_str (), x, refs[i].s10, refs[i].tol * 100.0, ok ? "ok" : "FAILED");
				}
			}
			double zd = galaxy->ZodiacalLight ().Deviation ();
			pass += zd <= 0.3;
			report ("sky check zodiacal light: within %.0f%% of the measured %s", zd * 100.0, zd <= 0.3 ? "ok" : "FAILED");
			report ("sky check: %d of %d %s", pass, n, pass == n && refs.size () ? "passed" : "FAILED");
		}
	}
	else galaxy.reset ();

	// a black hole: its lens on the sky (ORBITER_VK_LENS=0: none)
	if (galaxy && !(getenv ("ORBITER_VK_LENS") && !atoi (getenv ("ORBITER_VK_LENS")))) {
		bool any = false;
		for (DWORD i = 0; i < oapiGetGbodyCount (); i++)
			any |= oapiGetObjectType (oapiGetGbodyByIndex (i)) == OBJTP_BLACKHOLE;
		if (any) {
			lens = std::make_unique<BlackHoleLens> ();
			if (!lens->Init (ctx, galaxy.get (), starField.get ())) lens.reset ();
		}
	}

	// the sun lights the scene
	for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		if (oapiGetObjectType (h) == OBJTP_STAR) { hSun = h; break; }
	}
	return true;
}

void Scene::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	for (auto &it : visuals) gc->UnregisterVisObject (it.first);
	visuals.clear();
	ctx->Drain (); // releases deferred by the visuals use the shared renderers' pools
	if (tiles) tiles->Shutdown ();
	tiles.reset();
	if (atm) atm->Shutdown ();
	atm.reset();
	for (auto &sm : shadows) {
		if (sm) sm->Shutdown ();
		sm.reset();
	}
	if (stars) stars->Shutdown ();
	stars.reset();
	if (starField) starField->Shutdown ();
	starField.reset();
	if (lens) lens->Shutdown ();
	lens.reset();
	if (galaxy) galaxy->Shutdown ();
	galaxy.reset();
	if (ao) ao->Shutdown ();
	ao.reset();
	if (rtl) rtl->Shutdown ();
	rtl.reset();
	if (gc->TexMgr()) gc->TexMgr()->onDestroy = nullptr;
	if (rt) rt->Shutdown ();
	rt.reset();
	if (ssr) ssr->Shutdown ();
	ssr.reset();
	if (taa) taa->Shutdown ();
	taa.reset();
	if (particles) particles->Shutdown ();
	particles.reset();
	if (plumes) plumes->Shutdown ();
	if (plasma) plasma->Shutdown ();
	plasma.reset();
	plumes.reset();
	ctx->DestroyImage (reactive);
	ctx->DestroyImage (reactiveMS);
	if (vclouds) vclouds->Shutdown ();
	if (tracks) tracks->Shutdown ();
	vclouds.reset();
	if (fsr) fsr->Shutdown ();
	fsr.reset();
	ctx->DestroyBuffer (sphereVtx);
	ctx->DestroyBuffer (sphereIdx);
	for (auto &u : frameUbo) ctx->DestroyBuffer (u);
	for (auto &u : probeFrameUbo) ctx->DestroyBuffer (u);
	if (probe) probe->Shutdown ();
	probe.reset();
	VkPipeline pipes[] = { starPipe, meshOpaque, meshBlend, meshAdditive, ringPipe, ringShadowPipe, meshPrepass, meshOpaqueInst, meshPrepassInst, meshOpaqueImp, meshPrepassImp, buildOpaque, buildPrepass, lampPipe, particleAdditive, particleBlend };
	for (auto p : pipes) if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	vkDestroyPipelineLayout (ctx->device, meshLayout, nullptr);
	vkDestroyPipelineLayout (ctx->device, particleLayout, nullptr);
	vkDestroyPipelineLayout (ctx->device, lampLayout, nullptr);
	gc->TexMgr()->Release (exhaustTex);
	gc->TexMgr()->Release (glowTex);
	gc->TexMgr()->Release (cityNone);
	if (citySampler) vkDestroySampler (ctx->device, citySampler, nullptr);
	vkDestroyDescriptorPool (ctx->device, framePool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, frameLayout, nullptr);
	ctx = nullptr;
}

// ======================================================================
// Pipelines

VkPipeline Scene::CreateMeshPipeline (bool blend, bool additive, bool depthWrite, bool prepass, bool instanced, bool impostor, MeshShader shader)
{
	bool building = shader == MeshShader::Building;
	VkShaderModule vs = building ?
		(prepass ? ctx->CreateShaderModule (spv_prepass_building_vert, spv_prepass_building_vert_size)
		         : ctx->CreateShaderModule (spv_building_vert, spv_building_vert_size)) :
		shader != MeshShader::Mesh && shader != MeshShader::Star ? ctx->CreateShaderModule (spv_ring_vert, spv_ring_vert_size) :
		impostor ?
		(prepass ? ctx->CreateShaderModule (spv_prepass_impostor_inst_vert, spv_prepass_impostor_inst_vert_size)
		         : ctx->CreateShaderModule (spv_impostor_inst_vert, spv_impostor_inst_vert_size)) :
		instanced ?
		(prepass ? ctx->CreateShaderModule (spv_prepass_inst_vert, spv_prepass_inst_vert_size)
		         : ctx->CreateShaderModule (spv_mesh_inst_vert, spv_mesh_inst_vert_size)) :
		(prepass ? ctx->CreateShaderModule (spv_prepass_vert, spv_prepass_vert_size)
		         : ctx->CreateShaderModule (spv_mesh_vert, spv_mesh_vert_size));
	VkShaderModule fs = shader == MeshShader::Star ? ctx->CreateShaderModule (spv_star_frag, spv_star_frag_size) :
		shader == MeshShader::Ring ? ctx->CreateShaderModule (spv_ring_frag, spv_ring_frag_size) :
		shader == MeshShader::RingShadow ? ctx->CreateShaderModule (spv_ringshadow_frag, spv_ringshadow_frag_size) :
		prepass ? ctx->CreateShaderModule (spv_prepass_frag, spv_prepass_frag_size) :
		building ? ctx->CreateShaderModule (spv_building_frag, spv_building_frag_size)
		         : ctx->CreateShaderModule (spv_mesh_frag, spv_mesh_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};

	VkVertexInputBindingDescription bind[2] = {
		{ 0, sizeof(NTVERTEX), VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, INSTANCE_STRIDE, VK_VERTEX_INPUT_RATE_INSTANCE } // (instanced: rows of the transform)
	};
	VkVertexInputAttributeDescription attr[6] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, x) },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, nx) },
		{ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NTVERTEX, tu) },
		{ 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
		{ 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = instanced ? 2 : 1;
	vi.pVertexBindingDescriptions = bind;
	vi.vertexAttributeDescriptionCount = instanced ? 6 : 3;
	vi.pVertexAttributeDescriptions = attr;
	// buildings: shapes stretched by the instances (building.glsl)
	VkVertexInputBindingDescription bbind[2] = {
		{ 0, BUILDING_VERTEX, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, BUILDING_INSTANCE, VK_VERTEX_INPUT_RATE_INSTANCE }
	};
	VkVertexInputAttributeDescription battr[10] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24 },
		{ 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 40 },
		{ 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
		{ 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
		{ 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 },
		{ 8, 1, VK_FORMAT_R32G32_SFLOAT, 64 },
		{ 9, 1, VK_FORMAT_R32G32_UINT, 72 }
	};
	if (building) {
		vi.vertexBindingDescriptionCount = 2;
		vi.pVertexBindingDescriptions = bbind;
		vi.vertexAttributeDescriptionCount = prepass ? 8 : 10;
		vi.pVertexAttributeDescriptions = battr;
	}

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_BACK_BIT;
	// Orbiter meshes use clockwise front faces in a left-handed frame; with the
	// y flip of the projection they remain clockwise in framebuffer coordinates
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = prepass ? VK_SAMPLE_COUNT_1_BIT : ctx->sceneSamples;

	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z

	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	if (blend) {
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
		cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
		if (shader == MeshShader::Ring) // (premultiplied)
			cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		else if (shader == MeshShader::RingShadow) { // (multiplied)
			cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
			cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		}
	}
	VkPipelineColorBlendAttachmentState cbas[AmbientOcclusion::PREPASS_TARGETS] = { cba, cba, cba };
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = prepass ? AmbientOcclusion::PREPASS_TARGETS : 1;
	cb.pAttachments = cbas;

	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;

	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = prepass ? AmbientOcclusion::PREPASS_TARGETS : 1;
	rci.pColorAttachmentFormats = prepass ? AmbientOcclusion::PrepassFormats () : &ctx->sceneFormat;
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
	gpi.layout = meshLayout;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return pipe;
}

void Scene::CreatePipelines ()
{
	VkDescriptorSetLayout sets[4] = { frameLayout, gc->TexMgr()->Layout(), atm->SetLayout(), gc->TexMgr()->MapLayout() };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 4;
	plci.pSetLayouts = sets;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &meshLayout));

	meshOpaque = CreateMeshPipeline (false, false, true);
	starPipe = CreateMeshPipeline (false, false, true, false, false, false, MeshShader::Star);
	meshBlend = CreateMeshPipeline (true, false, false);
	meshAdditive = CreateMeshPipeline (true, true, false);
	ringPipe = CreateMeshPipeline (true, false, false, false, false, false, MeshShader::Ring);
	ringShadowPipe = CreateMeshPipeline (true, false, false, false, false, false, MeshShader::RingShadow);
	meshPrepass = CreateMeshPipeline (false, false, true, true);
	meshOpaqueInst = CreateMeshPipeline (false, false, true, false, true);
	meshPrepassInst = CreateMeshPipeline (false, false, true, true, true);
	meshOpaqueImp = CreateMeshPipeline (false, false, true, false, true, true);
	meshPrepassImp = CreateMeshPipeline (false, false, true, true, true, true);
	buildOpaque = CreateMeshPipeline (false, false, true, false, true, false, MeshShader::Building);
	buildPrepass = CreateMeshPipeline (false, false, true, true, true, false, MeshShader::Building);

	// billboards: view-space vertices, one texture
	VkDescriptorSetLayout texLayout = gc->TexMgr()->Layout();
	VkPushConstantRange ppcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(proj) + 4 * sizeof(float) }; // (+ the relativistic view's velocity)
	VkPipelineLayoutCreateInfo plci2{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci2.setLayoutCount = 1;
	plci2.pSetLayouts = &texLayout;
	plci2.pushConstantRangeCount = 1;
	plci2.pPushConstantRanges = &ppcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci2, nullptr, &particleLayout));
	particleAdditive = CreateParticlePipeline (true);
	particleBlend = CreateParticlePipeline (false);
	// street lamps: instanced glows, positions in a planet frame
	VkPushConstantRange lpcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * 64 + 4 * 16 };
	plci2.pPushConstantRanges = &lpcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci2, nullptr, &lampLayout));
	lampPipe = CreateParticlePipeline (true, true);
}

VkPipeline Scene::CreateParticlePipeline (bool additive, bool lamps)
{
	VkShaderModule vs = lamps ? ctx->CreateShaderModule (spv_lamp_vert, spv_lamp_vert_size)
	                          : ctx->CreateShaderModule (spv_particle_vert, spv_particle_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_particle_frag, spv_particle_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	VkVertexInputBindingDescription bind{ 0, sizeof(PVtx), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PVtx, x) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PVtx, u) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PVtx, r) }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 3;
	vi.pVertexAttributeDescriptions = attr;
	// (lamps: a vec4 per instance)
	VkVertexInputBindingDescription lbind{ 0, 16, VK_VERTEX_INPUT_RATE_INSTANCE };
	VkVertexInputAttributeDescription lattr{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
	if (lamps) {
		vi.pVertexBindingDescriptions = &lbind;
		vi.vertexAttributeDescriptionCount = 1;
		vi.pVertexAttributeDescriptions = &lattr;
	}
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
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
	gpi.layout = lamps ? lampLayout : particleLayout;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return pipe;
}

void Scene::DrawBillboards (VkTexture *tex, bool additive, const PVtx *v, size_t n, bool seen)
{
	if (!n || !tex) return;
	VkCommandBuffer cmd = ctx->Cmd();
	vkc::Context::Transient vb = ctx->AllocTransient (n * sizeof(PVtx));
	memcpy (vb.ptr, v, n * sizeof(PVtx));
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, additive ? particleAdditive : particleBlend);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleLayout, 0, 1, &tex->set, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);
	float ppc[20];
	memcpy (ppc, proj, sizeof(proj));
	VECTOR3 bv = seen ? _V(0, 0, 0) : RelBetaView ();
	ppc[16] = (float)bv.x, ppc[17] = (float)bv.y, ppc[18] = (float)bv.z, ppc[19] = 0.0f;
	vkCmdPushConstants (cmd, particleLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ppc), ppc);
	vkCmdDraw (cmd, (uint32_t)n, 1, 0, 0);
}

void Scene::DrawLamps (VkBuffer buf, uint32_t count, const Mat4 &frameToView, const VECTOR3 &centre, const VECTOR3 &sun,
	float fadeFrom, float range, float halfSize, float bright)
{
	if (!count || !buf) return;
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lampPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lampLayout, 0, 1, &glowTex->set, 0, nullptr);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &buf, &ofs);
	float pc[48];
	frameToView.ToFloat (pc);
	memcpy (pc + 16, proj, sizeof(float) * 16);
	// (glows at least about 1.5 pixels across)
	VkExtent2D e = ctx->RenderExtent ();
	pc[32] = (float)centre.x, pc[33] = (float)centre.y, pc[34] = (float)centre.z;
	pc[35] = 1.5f / (std::max (proj[5] < 0.0f ? -proj[5] : proj[5], 1e-3f) * (float)e.height);
	pc[36] = (float)sun.x, pc[37] = (float)sun.y, pc[38] = (float)sun.z, pc[39] = bright;
	pc[40] = fadeFrom, pc[41] = range, pc[42] = halfSize, pc[43] = 0.0f;
	VECTOR3 bv = RelBetaView ();
	pc[44] = (float)bv.x, pc[45] = (float)bv.y, pc[46] = (float)bv.z, pc[47] = 0.0f;
	vkCmdPushConstants (cmd, lampLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
	vkCmdDraw (cmd, 6, count, 0, 0);
}

void Scene::EnsureReactiveMask ()
{
	if (reactiveGen == ctx->SwapchainGeneration ()) return;
	vkDeviceWaitIdle (ctx->device);
	ctx->DestroyImage (reactive);
	ctx->DestroyImage (reactiveMS);
	VkExtent2D e = ctx->RenderExtent ();
	auto create = [this, e](VkSampleCountFlagBits samples, VkImageUsageFlags usage) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = REACTIVE_FORMAT;
		ici.extent = { e.width, e.height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = samples;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = REACTIVE_FORMAT;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	reactive = create (VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (ctx->sceneSamples != VK_SAMPLE_COUNT_1_BIT)
		reactiveMS = create (ctx->sceneSamples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
	// (the images are written in every frame's scene before they are read)
	VkImage img = reactive.image;
	ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	reactiveGen = ctx->SwapchainGeneration ();
}

void Scene::BuildSphere ()
{
	// unit sphere, y axis = rotation axis, u = longitude, v = latitude
	const int nLat = 128, nLng = 256;
	std::vector<NTVERTEX> vtx;
	for (int i = 0; i <= nLat; i++) {
		double lat = PI05 - PI * i / nLat;
		for (int j = 0; j <= nLng; j++) {
			double lng = PI2 * j / nLng;
			NTVERTEX v;
			v.nx = v.x = (float)(cos(lat) * cos(lng));
			v.ny = v.y = (float)sin(lat);
			v.nz = v.z = (float)(cos(lat) * sin(lng));
			v.tu = (float)j / nLng;
			v.tv = (float)i / nLat;
			vtx.push_back (v);
		}
	}
	std::vector<uint32_t> idx32;
	for (int i = 0; i < nLat; i++)
		for (int j = 0; j < nLng; j++) {
			uint32_t a = i * (nLng+1) + j, b = a + 1, c = a + (nLng+1), d = c + 1;
			// clockwise when seen from outside (left-handed frame)
			idx32.insert (idx32.end(), { a, b, c, b, d, c });
		}
	sphereVtx = ctx->CreateDeviceBuffer (vtx.data(), vtx.size() * sizeof(NTVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	sphereIdx = ctx->CreateDeviceBuffer (idx32.data(), idx32.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	sphereNIdx = (uint32_t)idx32.size();
}

// ======================================================================
// Particle textures of fires and sparks: soft round puffs, white-yellow at
// the centre to orange and red at the rim (fire: billowing), additive

VkTexture *Scene::FireTexture ()
{
	// (four puffs in the quadrants, as the particles pick one: SmokeTexture)
	if (fireTex) return fireTex;
	const int Q = 128, N = 2 * Q;
	std::vector<uint8_t> px (N * N * 4);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			int k = (y / Q) * 2 + (x / Q);
			double u = ((x % Q) + 0.5) / Q * 2.0 - 1.0, v = ((y % Q) + 0.5) / Q * 2.0 - 1.0;
			double r = sqrt (u*u + v*v), a = atan2 (v, u) + 1.7 * k;
			double billow = 0.14 * sin (5.0 * a + 3.0 * r + k) + 0.09 * sin ((11.0 + k) * a - 7.0 * r) + 0.05 * sin (23.0 * a + 13.0 * r);
			double t = std::clamp (r * (1.0 + billow), 0.0, 1.0);
			double alpha = pow (std::max (1.0 - t, 0.0), 1.6);
			double heat = std::max (1.0 - 1.7 * t, 0.0); // (hot core)
			uint8_t *p = &px[(y * N + x) * 4];
			p[0] = (uint8_t)(255.0 * std::min (1.0, 0.95 + 0.05 * heat));
			p[1] = (uint8_t)(255.0 * std::min (1.0, 0.32 + 0.6 * heat));
			p[2] = (uint8_t)(255.0 * std::min (1.0, 0.06 + 0.55 * heat * heat));
			p[3] = (uint8_t)(255.0 * alpha);
		}
	fireTex = gc->TexMgr()->CreateRGBA (N, N, px.data());
	return fireTex;
}

VkTexture *Scene::FireballTexture (int stage)
{
	// flames of a fireball by their temperature: ragged tongues (fractal
	// noise eats into the puff's edge and makes holes in its thin parts),
	// a radial ramp within each; four shapes in the quadrants, as the
	// particles pick one. 0: white-yellow (the hot core, first moments),
	// 1: orange, 2: dark red (cooling, at the rim and late); 3: a soft
	// white puff without billows (vapour, lit; the plasma wake); 4: neutral
	// ragged flames, brighter inside (coloured by the particles' temperature
	// ramp: ParticleSystem::Emission::ramp); 5: a smooth glow, no
	// structure at all (the plasma wake: luminous gas, not puffs)
	stage = std::clamp (stage, 0, 5);
	if (fireballTex[stage]) return fireballTex[stage];
	static const float inner[6][3] = { { 1.0f, 0.88f, 0.50f }, { 1.0f, 0.52f, 0.12f }, { 0.75f, 0.20f, 0.04f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
	static const float outer[6][3] = { { 1.0f, 0.62f, 0.20f }, { 0.90f, 0.28f, 0.04f }, { 0.40f, 0.06f, 0.02f }, { 1.0f, 1.0f, 1.0f }, { 0.45f, 0.45f, 0.45f }, { 1.0f, 1.0f, 1.0f } };
	static const double erode[6] = { 0.25, 0.45, 0.6, 0.0, 0.7, 0.0 }; // (the cooler, the more ragged)
	const int Q = 128, N = 2 * Q;
	auto hash = [](int x, int y, int k) {
		uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)k * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return ((h ^ (h >> 16)) & 0xffffff) / 16777215.0;
	};
	auto noise = [&](double x, double y, int k) {
		int ix = (int)std::floor (x), iy = (int)std::floor (y);
		double fx = x - ix, fy = y - iy;
		fx = fx * fx * (3.0 - 2.0 * fx), fy = fy * fy * (3.0 - 2.0 * fy);
		double a = hash (ix, iy, k), b = hash (ix + 1, iy, k), c = hash (ix, iy + 1, k), d = hash (ix + 1, iy + 1, k);
		return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
	};
	std::vector<uint8_t> px (N * N * 4);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			int k = (y / Q) * 2 + (x / Q) + 4 * stage;
			double u = ((x % Q) + 0.5) / Q * 2.0 - 1.0, v = ((y % Q) + 0.5) / Q * 2.0 - 1.0;
			double r = sqrt (u*u + v*v);
			double f = 0.0, amp = 0.5, fr = 3.0;
			for (int o = 0; o < 4; o++, amp *= 0.5, fr *= 2.0) f += amp * noise ((u + 1.3 * k) * fr, (v - 0.7 * k) * fr, k * 7 + o);
			f /= 0.9375; // (0..1)
			double body = 1.0 - r * r;                       // (dense in the middle)
			double c = body - erode[stage] * (1.0 - f) - 0.08 * r;
			double alpha = std::clamp (c / 0.35, 0.0, 1.0);
			alpha = alpha * alpha * (3.0 - 2.0 * alpha);
			if (stage == 3) alpha = body * body * (0.8 + 0.2 * f);
			if (stage == 5) alpha = std::max (body, 0.0) * exp (-2.5 * r * r); // (Gaussian-like, zero at the rim)
			if (r >= 1.0) alpha = 0.0;
			double h = std::clamp (1.0 - r * 1.2 + (stage == 4 ? 0.6 : 0.3) * (f - 0.5), 0.0, 1.0); // (hotter inside; neutral flames: blotchy, darker billows)
			uint8_t *p = &px[(y * N + x) * 4];
			for (int j = 0; j < 3; j++) p[j] = (uint8_t)(255.0 * (outer[stage][j] + (inner[stage][j] - outer[stage][j]) * h));
			p[3] = (uint8_t)(255.0 * alpha);
		}
	fireballTex[stage] = gc->TexMgr()->CreateRGBA (N, N, px.data());
	return fireballTex[stage];
}

VkTexture *Scene::SparkTexture ()
{
	if (sparkTex) return sparkTex;
	const int Q = 16, N = 2 * Q;
	std::vector<uint8_t> px (N * N * 4);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			double u = ((x % Q) + 0.5) / Q * 2.0 - 1.0, v = ((y % Q) + 0.5) / Q * 2.0 - 1.0;
			double r = sqrt (u*u + v*v);
			double alpha = exp (-r * r * 9.0);
			uint8_t *p = &px[(y * N + x) * 4];
			p[0] = 255, p[1] = (uint8_t)(255.0 * (0.75 + 0.25 * alpha)), p[2] = (uint8_t)(255.0 * (0.35 + 0.5 * alpha));
			p[3] = (uint8_t)(255.0 * alpha);
		}
	sparkTex = gc->TexMgr()->CreateRGBA (N, N, px.data());
	return sparkTex;
}

VkTexture *Scene::FragmentTexture ()
{
	if (fragTex) return fragTex;
	// a shard in each quadrant: an irregular polygon, sharp (one texel of
	// edge), lighter towards one corner (a lit face)
	const int Q = 32, N = 2 * Q;
	std::vector<uint8_t> px (N * N * 4, 0);
	std::mt19937 rng (1234);
	std::uniform_real_distribution<double> U (0.0, 1.0);
	for (int q = 0; q < 4; q++) {
		int nv = 5 + q % 3;
		std::vector<double> ang (nv), rad (nv);
		for (int k = 0; k < nv; k++) ang[k] = (k + 0.3 + 0.4 * U (rng)) / nv * 2.0 * PI, rad[k] = 0.45 + 0.45 * U (rng);
		int ox = (q & 1) * Q, oy = (q >> 1) * Q;
		for (int y = 0; y < Q; y++)
			for (int x = 0; x < Q; x++) {
				double u = (x + 0.5) / Q * 2.0 - 1.0, v = (y + 0.5) / Q * 2.0 - 1.0;
				double a = atan2 (v, u); if (a < 0.0) a += 2.0 * PI;
				double r = sqrt (u*u + v*v);
				// (the polygon's edge in this direction)
				int k = 0;
				while (k + 1 < nv && ang[k + 1] < a) k++;
				int k0 = a < ang[0] ? nv - 1 : k, k1 = (k0 + 1) % nv;
				double a0 = ang[k0], a1 = ang[k1] + (k1 == 0 ? 2.0 * PI : 0.0), aa = a < a0 ? a + 2.0 * PI : a;
				double f = (aa - a0) / std::max (a1 - a0, 1e-6), edge = rad[k0] + (rad[k1] - rad[k0]) * f;
				double alpha = std::clamp ((edge - r) * Q * 0.5 + 0.5, 0.0, 1.0);
				uint8_t *p = &px[((oy + y) * N + ox + x) * 4];
				uint8_t c = (uint8_t)(255.0 * (0.75 + 0.25 * std::clamp (0.5 - 0.35 * (u + v), 0.0, 1.0)));
				p[0] = p[1] = p[2] = c, p[3] = (uint8_t)(255.0 * alpha);
			}
	}
	fragTex = gc->TexMgr()->CreateRGBA (N, N, px.data());
	return fragTex;
}

// ======================================================================
// Visuals

VObject *Scene::GetVisual (OBJHANDLE hObj)
{
	auto it = visuals.find (hObj);
	return it != visuals.end() ? it->second.get() : nullptr;
}

void Scene::DeleteVisual (OBJHANDLE hObj)
{
	auto it = visuals.find (hObj);
	if (it == visuals.end()) return;
	gc->UnregisterVisObject (hObj);
	visuals.erase (it);
}

void Scene::Update ()
{
	UpdateCamera ();

	DWORD n = oapiGetObjectCount();
	for (DWORD i = 0; i < n; i++) {
		OBJHANDLE h = oapiGetObjectByIndex (i);
		int type = oapiGetObjectType (h);
		bool want;
		if (type == OBJTP_PLANET || type == OBJTP_STAR) {
			want = true;
		} else if (type == OBJTP_VESSEL) {
			VECTOR3 p;
			oapiGetGlobalPos (h, &p);
			double range = std::max (VESSEL_VIS_MIN, oapiGetSize (h) * VESSEL_VIS_RANGE);
			double d = length (p - camPos);
			bool have = visuals.count (h) != 0;
			want = have ? d < range * 1.2 : d < range; // hysteresis
		} else {
			want = false;
		}
		bool have = visuals.count (h) != 0;
		if (want && !have) {
			std::unique_ptr<VObject> vo;
			if (type == OBJTP_VESSEL) vo = std::make_unique<VVessel> (h, this);
			else vo = std::make_unique<VPlanet> (h, this, type == OBJTP_STAR);
			VObject *raw = vo.get();
			visuals[h] = std::move (vo);
			gc->RegisterVisObject (h, (VISHANDLE)raw);
		} else if (!want && have) {
			DeleteVisual (h);
		}
	}

	// pieces of debris near the camera (not in the object list; handles of
	// deleted pieces may be reused: told apart by their serial numbers)
	std::set<OBJHANDLE> debris;
	DWORD nd = oapiGetDebrisCount();
	for (DWORD i = 0; i < nd; i++) {
		OBJHANDLE h = oapiGetDebrisByIndex (i);
		DEBRISINFO info;
		if (!oapiGetDebrisInfo (h, &info)) continue;
		debris.insert (h);
		VECTOR3 p;
		oapiGetGlobalPos (h, &p);
		double range = std::max (VESSEL_VIS_MIN, info.size * VESSEL_VIS_RANGE);
		auto it = visuals.find (h);
		VDebris *vd = it != visuals.end() ? dynamic_cast<VDebris*>(it->second.get()) : nullptr;
		if (vd && vd->Serial() != info.serial) visuals.erase (it), vd = nullptr;
		double d = length (p - camPos);
		bool want = vd ? d < range * 1.2 : d < range;
		if (want && !vd) visuals[h] = std::make_unique<VDebris> (h, this, info.serial);
		else if (!want && vd) visuals.erase (h);
	}
	for (auto it = visuals.begin(); it != visuals.end(); ) {
		if (dynamic_cast<VDebris*>(it->second.get()) && !debris.count (it->first)) it = visuals.erase (it);
		else ++it;
	}

	for (auto &it : visuals) it.second->Update();
}

// ======================================================================
// Camera

void Scene::UpdateCamera ()
{
	oapiCameraGlobalPos (&camPos);
	oapiCameraRotationMatrix (&camRot);
	// ORBITER_VK_SKY_LOOK=l,b: the view turned to Galactic longitude l,
	// latitude b [deg], Galactic north up (for tests of the sky)
	if (const char *e = getenv ("ORBITER_VK_SKY_LOOK")) {
		double l, b;
		if (sscanf (e, "%lf,%lf", &l, &b) == 2) {
			double G[3][3];
			galactic::EclipticToGalactic (G);
			auto toEcl = [&](double x, double y, double z) {
				return _V(G[0][0] * x + G[1][0] * y + G[2][0] * z, G[0][1] * x + G[1][1] * y + G[2][1] * z, G[0][2] * x + G[1][2] * y + G[2][2] * z);
			};
			l *= RAD, b *= RAD;
			VECTOR3 f = toEcl (cos (b) * cos (l), cos (b) * sin (l), sin (b));
			VECTOR3 n = toEcl (0, 0, 1);
			VECTOR3 u = unit (n - f * dotp (n, f));
			VECTOR3 r = crossp (u, f);
			camRot = _M(r.x, u.x, f.x, r.y, u.y, f.y, r.z, u.z, f.z);
		}
	}
	// in the cockpit, the view jolted by the vessel's aerodynamic jolts
	// (turbulence, buffeting: its aerodynamic acceleration's change within
	// about half a second; the head lagging, a few tenths of a degree per
	// m/s^2); ORBITER_VK_SHAKE scales it (0: off)
	{
		const double scale = vkc::Gfx ().cameraShake;
		VESSEL *v = oapiCameraInternal () && scale > 0.0 ? oapiGetFocusInterface () : nullptr;
		double dt = oapiGetSimStep ();
		if (v && dt > 0.0 && oapiGetTimeAcceleration () <= 2.0) {
			VECTOR3 L, D;
			v->GetLiftVector (L);
			v->GetDragVector (D);
			VECTOR3 a = (L + D) / std::max (v->GetMass (), 1.0);
			if (length (shakeLP) == 0.0) shakeLP = a; // (entering the cockpit: no jolt)
			shakeLP += (a - shakeLP) * (1.0 - exp (-dt / 0.4));
			shakeV += ((a - shakeLP) - shakeV) * (1.0 - exp (-dt / 0.04));
			const double k = 0.25 * RAD * scale;
			auto clampA = [](double x) { return std::clamp (x, -2.0 * RAD, 2.0 * RAD); };
			double p = clampA (-k * shakeV.y), r = clampA (0.5 * k * shakeV.x), y = clampA (0.3 * k * shakeV.x - 0.2 * k * shakeV.z);
			double cp = cos (p), sp = sin (p), cr = cos (r), sr = sin (r), cy = cos (y), sy = sin (y);
			MATRIX3 Rx = { 1, 0, 0,  0, cp, -sp,  0, sp, cp };
			MATRIX3 Ry = { cy, 0, sy,  0, 1, 0,  -sy, 0, cy };
			MATRIX3 Rz = { cr, -sr, 0,  sr, cr, 0,  0, 0, 1 };
			camRot = mul (camRot, mul (Ry, mul (Rx, Rz)));
		} else if (!v) {
			shakeLP = shakeV = _V(0, 0, 0);
		}
	}
	// view = transpose(camRot): global direction -> camera frame
	MATRIX3 vt = {
		camRot.m11, camRot.m21, camRot.m31,
		camRot.m12, camRot.m22, camRot.m32,
		camRot.m13, camRot.m23, camRot.m33
	};
	view = Mat4::FromRotTrans (vt, _V(0, 0, 0));

	VkExtent2D ext = ctx->RenderExtent();
	viewW = std::max (1u, ext.width);
	viewH = std::max (1u, ext.height);
	tanAperture = tan (oapiCameraAperture());
	double fy = 1.0 / tanAperture;
	double fx = fy * viewH / viewW;

	// view (x right, y up, z forward) -> Vulkan clip (y down, reversed z, infinite far plane)
	memset (proj, 0, sizeof(proj));
	proj[0] = (float)fx;
	proj[5] = (float)-fy;
	proj[11] = 1.0f;               // w = z
	proj[14] = (float)nearPlane;   // z = near  ->  depth = near / z
	UpdateRelativity ();
}

void Scene::UpdateRelativity ()
{
	const double C = 299792458.0;
	static const bool off = getenv ("ORBITER_VK_RELVIEW") && !atoi (getenv ("ORBITER_VK_RELVIEW"));
	static const char *forced = getenv ("ORBITER_VK_RELVIEW_BETA");
	relOn = false, relBeta = _V(0, 0, 0), camVel = _V(0, 0, 0), relSelf = nullptr, relSelfRad = 0.0;
	if (off) return;
	OBJHANDLE tgt = oapiCameraTarget ();
	if (forced) {
		double b[3];
		int n = sscanf (forced, "%lf,%lf,%lf", b, b + 1, b + 2);
		if (n == 3) relBeta = _V(b[0], b[1], b[2]);
		else if (n >= 1) relBeta = _V(camRot.m13, camRot.m23, camRot.m33) * b[0];
	} else if (tgt) {
		VECTOR3 v;
		oapiGetGlobalVel (tgt, &v);
		relBeta = v / C;
	}
	double b = length (relBeta);
	if (b >= 0.999999) relBeta *= 0.999999 / b, b = 0.999999;
	camVel = relBeta * C;
	if (b < REL_VIEW_BETA) { relBeta = _V(0, 0, 0); return; }
	relOn = true;
	if (tgt && oapiGetObjectType (tgt) == OBJTP_VESSEL) relSelf = tgt, relSelfRad = 1.2 * oapiGetSize (tgt);
}

VkImageView Scene::PrepassView () const
{
	return ao ? ao->PrepassView () : VK_NULL_HANDLE;
}

void Scene::RelSelfView (float out[4]) const
{
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	if (!relOn || !relSelf) return;
	VECTOR3 sp;
	oapiGetGlobalPos (relSelf, &sp);
	VECTOR3 o = view.TransformPoint (sp - camPos);
	for (int k = 0; k < 3; k++) out[k] = (float)o.data[k];
	out[3] = (float)relSelfRad;
}

namespace {
	// The Doppler shift of a linear RGB colour: as Shaders/doppler.glsl (the
	// same model and constants: reflectance from the channels, interpolated,
	// times a blackbody of the Sun's temperature; Gaussian camera channels)
	void DopplerShiftRGB (const double rgb[3], double D, double out[3])
	{
		const int N = 16;
		const double L0 = 380.0, L1 = 720.0, TSUN = 5772.0;
		auto planck = [](double l, double T) {
			double um = l * 1e-3, x = std::min (14387.77 / (um * T), 80.0);
			return 1.0 / (um * um * um * um * um * (std::exp (x) - 1.0));
		};
		auto filter = [](double l, double f[3]) {
			const double c[3] = { 600.0, 545.0, 455.0 }, w[3] = { 45.0, 40.0, 35.0 };
			for (int i = 0; i < 3; i++) { double d = (l - c[i]) / w[i]; f[i] = std::exp (-0.5 * d * d); }
		};
		auto basis = [](double l, double h[3]) {
			h[0] = h[1] = h[2] = 0.0;
			if (l >= 600.0) h[0] = 1.0;
			else if (l <= 455.0) h[2] = 1.0;
			else if (l >= 545.0) { double t = (l - 545.0) / 55.0; h[0] = t, h[1] = 1.0 - t; }
			else { double t = (l - 455.0) / 90.0; h[1] = t, h[2] = 1.0 - t; }
		};
		auto response = [&](double d, double M[3][3]) {
			for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = 0.0;
			for (int k = 0; k < N; k++) {
				double l = L0 + (L1 - L0) / N * (k + 0.5), f[3], h[3];
				filter (l, f), basis (l * d, h);
				double B = planck (l, TSUN * d);
				for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] += f[i] * h[j] * B;
			}
		};
		double M1[3][3], MD[3][3];
		response (1.0, M1), response (D, MD);
		// the reflectance whose colour at rest is rgb (Cramer's rule)
		auto det = [](const double m[3][3]) {
			return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
			     - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
			     + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		};
		double d0 = det (M1), rho[3];
		for (int j = 0; j < 3; j++) {
			double m[3][3];
			for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) m[i][k] = k == j ? rgb[i] : M1[i][k];
			rho[j] = std::max (det (m) / d0, 0.0);
		}
		for (int i = 0; i < 3; i++) out[i] = std::max (0.0, MD[i][0] * rho[0] + MD[i][1] * rho[1] + MD[i][2] * rho[2]);
	}
}

VECTOR3 Scene::RelApparentView (const VECTOR3 &p) const
{
	if (!relOn) return p;
	VECTOR3 b = RelBetaView ();
	double r = length (p), b2 = dotp (b, b);
	if (r <= 0.0 || b2 <= 0.0) return p;
	VECTOR3 n = p / r;
	double g = 1.0 / std::sqrt (1.0 - b2), nb = dotp (n, b);
	VECTOR3 a = (n + b * (g * g / (g + 1.0) * nb + g)) / (g * (1.0 + nb)); // (as relativity.glsl's RelAberrate)
	return a * (r * g * (1.0 + nb));
}

void Scene::StarDiscRGB (double teff, float rgb[3])
{
	// (a black body at T is the Sun's spectrum seen at Doppler factor T / T_sun)
	double one[3] = { 1.0, 1.0, 1.0 }, c[3];
	DopplerShiftRGB (one, teff / 5772.0, c);
	for (int k = 0; k < 3; k++) rgb[k] = (float)c[k];
}

void Scene::UpdateLens ()
{
	// the nearest black hole whose lens shows: its Einstein ring (sqrt (4M/r)
	// for the sky far behind it) wider than two pixels
	lensOn = false, lensDonor = nullptr;
	if (!lens || !galaxy || !skySettings.milkyWay) return;
	const double C = 299792458.0;
	OBJHANDLE best = nullptr;
	BLACKHOLEINFO bi{}, b{};
	double bestR = 0.0;
	double pix = 2.0 / (ProjX () * std::max (1.0, (double)ctx->RenderExtent ().width)); // [rad]
	for (DWORD i = 0; i < oapiGetGbodyCount (); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		if (oapiGetObjectType (h) != OBJTP_BLACKHOLE || !oapiGetBlackHoleInfo (h, &b)) continue;
		VECTOR3 hp;
		oapiGetGlobalPos (h, &hp);
		double r = length (camPos - hp) / b.M;
		double thE = std::sqrt (4.0 / r);
		if (thE < 2.0 * pix) continue;
		// (and only where it shows: in the view, or so near it that the sky
		// is bent visibly, within LENS_VIEW_MARGIN rings, where the deflection
		// thE^2 / beta is still a tenth of a ring. In a narrow view the ring
		// of a hole far away passed the size test from anywhere in the sky
		// (Nyx, 24.5 pc: below 0.45 deg aperture at 1080 lines) and the lens
		// took over the view: a white sky (its trace from 5e9 M out
		// overflowed the float, kerr.glsl KerrR, since made safe))
		if (thE < LENS_VIEW_ALWAYS) {
			VECTOR3 dv = unit (view.TransformPoint (hp - camPos));
			double off = std::acos (std::clamp (dv.z, -1.0, 1.0)); // (from the view's axis)
			double half = std::atan (std::sqrt (1.0 / (ProjX () * ProjX ()) + 1.0 / (ProjY () * ProjY ()))); // (to its corners)
			if (off > half + LENS_VIEW_MARGIN * thE) continue;
		}
		if (!best || r < bestR) best = h, bestR = r, bi = b;
	}
	if (!best) return;

	BlackHoleLens::Frame f;
	kerr::Hole K (1.0, bi.a / bi.M);
	VECTOR3 hp, hv;
	oapiGetGlobalPos (best, &hp);
	oapiGetGlobalVel (best, &hv);
	VECTOR3 rc = (camPos - hp) / bi.M;
	f.cam[0] = dotp (rc, bi.ex), f.cam[1] = dotp (rc, bi.ey), f.cam[2] = dotp (rc, bi.axis);
	f.a = K.a;
	// the camera moves with its target (relative to the hole); else at
	// rest in the coordinates; where neither can be, falling with the
	// slicing's normal observer
	double u[4];
	bool ok = false;
	OBJHANDLE tgt = oapiCameraTarget ();
	VECTOR3 v = _V(0, 0, 0);
	if (tgt) {
		oapiGetGlobalVel (tgt, &v);
		v = (v - hv) / C;
	}
	double V[3] = { dotp (v, bi.ex), dotp (v, bi.ey), dotp (v, bi.axis) };
	ok = K.CoordinateVelocity (f.cam, V, u);
	if (!ok) K.NormalObserver (f.cam, u);
	K.BoostFrame (f.cam, u, f.e);
	// view -> global (camRot) -> Kerr; Kerr -> global -> Galactic
	VECTOR3 ax[3] = { bi.ex, bi.ey, bi.axis };
	double G[3][3];
	galactic::EclipticToGalactic (G);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			f.v2k[i][j] = ax[i].x * camRot.data[j] + ax[i].y * camRot.data[3 + j] + ax[i].z * camRot.data[6 + j];
			f.k2g[i][j] = G[i][0] * ax[j].x + G[i][1] * ax[j].y + G[i][2] * ax[j].z;
		}
	f.rh = K.Horizon (), f.rm = K.InnerHorizon (), f.rph = K.PhotonOrbit (true);
	f.inside = K.R (f.cam) < f.rh;
	f.fx = ProjX (), f.fy = ProjY ();
	f.jitter[0] = jitterPixels[0], f.jitter[1] = jitterPixels[1];
	// the hole's frame relative to the cosmic background: the barycentre's
	// 369.82 km/s towards l 264.021, b 48.253 and the hole's velocity
	{
		const double DEG = 3.14159265358979323846 / 180.0;
		double l = 264.021 * DEG, bb = 48.253 * DEG;
		double Vg[3] = { std::cos (bb) * std::cos (l) * 369.82e3 / C, std::cos (bb) * std::sin (l) * 369.82e3 / C, std::sin (bb) * 369.82e3 / C };
		double hg[3];
		VECTOR3 hb = hv / C;
		for (int i = 0; i < 3; i++) hg[i] = G[i][0] * hb.x + G[i][1] * hb.y + G[i][2] * hb.z;
		double v2 = Vg[0] * Vg[0] + Vg[1] * Vg[1] + Vg[2] * Vg[2], gV = 1.0 / std::sqrt (1.0 - v2);
		double Vv = Vg[0] * hg[0] + Vg[1] * hg[1] + Vg[2] * hg[2];
		for (int i = 0; i < 3; i++) f.betaBg[i] = (Vg[i] + hg[i] / gV + Vg[i] * (gV / (1.0 + gV) * Vv)) / (1.0 + Vv);
	}
	const char *ce = getenv ("ORBITER_VK_SKY_COSMIC");
	f.cosmic = !ce || atoi (ce) != 0;
	f.stars = starField && skySettings.stars;
	lensDisc = bi.discRate > 0.0;
	ObservedPos (best, lensPos); // (drawn objects bent about where it is seen)
	lensM = bi.M;
	if (bi.discRate > 0.0) {
		f.discTK = bi.discTempScale, f.discIn = bi.isco / bi.M, f.discOut = bi.discOuter / bi.M;
		f.glowRim = bi.discGlow > 0.0 ? f.discOut : 0.0, f.glow = 0.25 * bi.discGlow;
		f.jets = bi.jets, f.jetLength = bi.jetLength / bi.M;
		// (the disc seen out to where it glows: beyond, cool and thin, the sky through it)
		const double DISC_T_MIN = 3000.0; // [K] (cooler: dark in visible light; it would hide the sky behind)
		for (double r = 1.5 * f.discIn; r < f.discOut; r *= 1.02)
			if (bi.discTempScale * std::pow (K.DiscFluxShape (r), 0.25) < DISC_T_MIN) { f.discOut = r; break; }
		f.time = oapiGetSimTime () * C / bi.M;
		// (a donor star: its stream to the disc's rim, where it strikes the
		// hot spot; the inner Lagrange point by Eggleton's Roche lobe)
		if (bi.donor) {
			VECTOR3 dp;
			oapiGetGlobalPos (bi.donor, &dp);
			VECTOR3 r = (dp - hp) / bi.M;
			double x = dotp (r, bi.ex), y = dotp (r, bi.ey);
			double q = oapiGetMass (bi.donor) / bi.mass, q3 = std::cbrt (q), q23 = q3 * q3;
			double rl = 0.49 * q23 / (0.6 * q23 + std::log1p (q3));
			f.streamA = std::sqrt (x * x + y * y);
			f.streamPhi = std::atan2 (y, x);
			f.streamL1 = f.streamA * (1.0 - rl);
			f.streamHot = bi.discOuter / bi.M;
			// (its body: drawn in the lensed sky too, for its images about the
			// hole, behind its gas)
			f.donorPos[0] = x, f.donorPos[1] = y, f.donorPos[2] = dotp (r, bi.axis);
			{
				// (its orbit's angular velocity [rad/M]: the trace sees it, and
				// its stream, where they were when the light left)
				VECTOR3 dv;
				oapiGetGlobalVel (bi.donor, &dv);
				dv = (dv - hv) / C;
				double vx = dotp (dv, bi.ex), vy = dotp (dv, bi.ey);
				f.donorOmega = x * x + y * y > 0.0 ? (x * vy - y * vx) / (x * x + y * y) : 0.0;
			}
			f.donorR = oapiGetSize (bi.donor) / bi.M;
			for (const NearStar &s : nearStars) if (s.body == bi.donor) f.donorTeff = s.teff;
			lensDonor = bi.donor;
		}
	}
	lens->Trace (f);
	lensOn = true;
	// (ORBITER_VK_LENS_DONOR_CHECK: once, after the view settled, the donor
	// where it is drawn against where the trace sees it)
	static const int donorCheck = getenv ("ORBITER_VK_LENS_DONOR_CHECK") ? std::max (atoi (getenv ("ORBITER_VK_LENS_DONOR_CHECK")), 2) : 0;
	static int donorFrame = 0;
	if (donorCheck && lensDonor && ++donorFrame == donorCheck) {
		VECTOR3 dp;
		ObservedPos (lensDonor, dp);
		VECTOR3 v = RelApparentView (view.TransformPoint (dp - camPos));
		double vd[3] = { v.x, v.y, v.z };
		lens->CheckDonor (vd);
	}
}

void Scene::UpdateNearStars ()
{
	nearStars.clear ();
	constexpr double AU_M = 1.49597870700e11;
	// the candidates: the Sun, the named stars near the camera, black holes'
	// accretion discs; the brightest the light (shadow maps, the sky's
	// scattering, specular), the next MAX_EXTRA_LIGHTS of note extra lights
	std::vector<StarLight> cand;
	sunAsBody = false;
	if (hSun) {
		StarLight l;
		oapiGetGlobalPos (hSun, &l.pos);
		l.radius = oapiGetSize (hSun);
		l.body = hSun;
		double d = std::max (length (l.pos - camPos), 1.0);
		l.flux = (AU_M / d) * (AU_M / d);
		sunAsBody = l.flux >= NEAR_FLUX;
		cand.push_back (l);
	}
	// (the first named star is the Galactic Centre: a point)
	int n = oapiNamedStarCount ();
	for (int i = 1; i < n; i++) {
		NAMEDSTAR s;
		// (a star of the world placed by its configuration, Hemera, the
		// S-stars: no point of the star field's, index -1)
		if (!oapiGetNamedStar (i, &s) || s.teff <= 0.0) continue;
		if (s.body) ObservedPos (s.body, s.pos); // (Hemera, fast about Nyx: seen where it was)
		double d = std::max (length (s.pos - camPos), 1.0);
		double flux = s.lumV * (AU_M / d) * (AU_M / d);
		if (flux < NEAR_FLUX) continue;
		uint32_t seed = s.index >= 0 ? (uint32_t)s.index : 0x40000000u + (uint32_t)i;
		nearStars.push_back (NearStar{ s.index, seed, s.pos, s.radius, s.teff, flux, d, s.body });
		StarLight l;
		l.pos = s.pos, l.radius = s.radius, l.teff = s.teff, l.flux = flux;
		l.sun = false, l.body = s.body;
		StarFlare fl[2];
		StarFlares (seed, s.teff, oapiGetSimTime (), fl, l.flare);
		cand.push_back (l);
	}
	// black holes' accretion discs: light from the hole's direction, of the
	// colour of their hot inner gas, the disc's luminosity foreshortened by
	// the angle to its axis (a face's 2 |cos i| L / (4 pi d^2); edge on at
	// least a fifth of that: the lensed far side); its bright inner part the
	// light's size (soft shadows)
	for (DWORD i = 0; i < oapiGetGbodyCount (); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		BLACKHOLEINFO bi;
		if (oapiGetObjectType (h) != OBJTP_BLACKHOLE || !oapiGetBlackHoleInfo (h, &bi) || bi.discRate <= 0.0) continue;
		VECTOR3 hp;
		oapiGetGlobalPos (h, &hp);
		VECTOR3 dv = camPos - hp;
		double d = std::max (length (dv), 10.0 * bi.M);
		double ci = std::max (std::fabs (dotp (dv, bi.axis)) / std::max (length (dv), 1.0), 0.2);
		StarLight l;
		l.pos = hp, l.radius = 10.0 * bi.M, l.teff = 0.8 * bi.discTmax;
		l.flux = bi.discLuminosity / 3.828e26 * 2.0 * ci * (AU_M / d) * (AU_M / d);
		l.sun = false, l.disc = true;
		cand.push_back (l);
	}
	std::stable_sort (cand.begin (), cand.end (), [](const StarLight &a, const StarLight &b) { return a.flux > b.flux; });
	light = StarLight ();
	if (cand.empty ()) light.flux = 0.0; // (no light)
	else light = cand[0];
	extraLights.clear ();
	for (size_t k = 1; k < cand.size () && extraLights.size () < MAX_EXTRA_LIGHTS; k++)
		if (cand[k].flux > EXTRA_LIGHT_MIN * light.flux) extraLights.push_back (cand[k]);
}

void Scene::RenderNearStars (bool transparent)
{
	// as the Sun (VPlanet): a disc far brighter than any lit surface, and
	// glare about it with a least apparent size
	constexpr float STAR_GLOW = 2.0f;
	constexpr double STAR_GLOW_SIZE = 6.0, STAR_GLOW_MIN_ANGLE = 0.02;
	for (const NearStar &s : nearStars) {
		float c[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		StarDiscRGB (s.teff, c);
		RelShiftColour (s.pos, c);
		if (!transparent) {
			if (PixelSize (s.radius, s.dist) < 0.5) continue;
			// (a black hole's donor seen close behind it: its images about the
			// hole, the lensed sky's (lens_trace.comp), not its mesh drawn
			// bent, the pixels' rays straight)
			if (lensOn && s.body && s.body == lensDonor) {
				double dl = length (lensPos - camPos), ds = length (s.pos - camPos), dls = length (s.pos - lensPos);
				double cosA = dotp (lensPos - camPos, s.pos - camPos) / std::max (dl * ds, 1e-30);
				double thE = ds > dl ? std::sqrt (4.0 * lensM * dls / (dl * ds)) : 0.0;
				if (cosA > std::cos (3.0 * thE + std::asin (std::min (s.radius * 1.5 / ds, 1.0)))) continue;
			}
			float d[3] = { c[0] * STAR_BRIGHTNESS, c[1] * STAR_BRIGHTNESS, c[2] * STAR_BRIGHTNESS };
			// (its photosphere, turning: a G star in ~25 days, cool dwarfs slower)
			double P = (s.teff < 4000.0 ? 80.0 : s.teff < 5300.0 ? 40.0 : 25.0) * 86400.0;
			double ang = std::fmod (oapiGetSimTime (), P) / P * 6.283185307179586;
			MATRIX3 R = _M(std::cos (ang), 0, -std::sin (ang), 0, 1, 0, std::sin (ang), 0, std::cos (ang));
			// (a black hole's donor, filling its Roche lobe: turning with its
			// orbit, drawn out towards the hole, its x axis; y the orbit's axis)
			VECTOR3 hx, hy;
			if (DonorFrame (s.body, s.pos, hx, hy)) {
				VECTOR3 hz = crossp (hx, hy);
				R = _M(hx.x, hy.x, hz.x, hx.y, hy.y, hz.y, hx.z, hy.z, hz.z);
				const VECTOR3 tide = _V(1, 0, 0);
				// (its Roche lobe within the mesh: star.frag's ROCHE_BOX)
				DrawStar (ModelView (s.pos, R) * Mat4::Scale (1.45 * s.radius, 0.93 * s.radius, 0.97 * s.radius), d, s.teff, s.radius, s.seed, &tide, true);
				continue;
			}
			DrawStar (ModelView (s.pos, R) * Mat4::Scale (s.radius, s.radius, s.radius), d, s.teff, s.radius, s.seed);
			continue;
		}
		VECTOR3 vp = RelApparentView (ViewPos (s.pos));
		if (vp.z <= 0.0) continue;
		// (its glare: the optics' response to its light, of its colour, as the
		// Sun's while it is a body: not its disc's radiance, a red dwarf's a
		// few hundredths of the Sun's; at least of the minimum size)
		double h = StarGlowSize (s.radius, vp.z, STAR_GLOW_SIZE, STAR_GLOW_MIN_ANGLE);
		// (at its near surface: hidden by what is in front, not by its own
		// disc, which drawn dimmer than the glare was a dark hole in it)
		double vl = length (vp), fs = vl > s.radius ? std::max (vl - 1.01 * s.radius, 0.5 * vl) / vl : 1.0;
		vp *= fs, h *= fs;
		double f = STAR_GLOW / std::max (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2], 1e-6);
		f *= StarGlowFade (PixelSize (s.radius, s.dist));
		if (f <= 0.0) continue;
		float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z, hh = (float)h;
		float r = (float)(c[0] * f), g = (float)(c[1] * f), b = (float)(c[2] * f);
		PVtx q[4] = {
			{ x - hh, y - hh, z, 0.0f, 0.0f, r, g, b, 1 }, { x + hh, y - hh, z, 1.0f, 0.0f, r, g, b, 1 },
			{ x + hh, y + hh, z, 1.0f, 1.0f, r, g, b, 1 }, { x - hh, y + hh, z, 0.0f, 1.0f, r, g, b, 1 } };
		PVtx v[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
		DrawBillboards (GlowTex (), true, v, 6, true);
	}
}

double Scene::StarDiscAngle (double *teff) const
{
	double best = 0.0;
	auto look = [&](const VECTOR3 &p, double radius, double T) {
		VECTOR3 r = p - camPos;
		double d = length (r);
		if (d <= radius) return;
		double a = std::asin (radius / d);
		VECTOR3 v = ViewDir (r / d);
		double off = std::acos (std::clamp (v.z, -1.0, 1.0)); // (from the view's axis)
		if (off - a < 0.75 && a > best) { // (about a 60 deg view's corners)
			best = a;
			if (teff) *teff = T;
		}
	};
	if (hSun && sunAsBody) {
		VECTOR3 sp;
		oapiGetGlobalPos (hSun, &sp);
		look (sp, oapiGetSize (hSun), 5772.0);
	}
	for (const NearStar &s : nearStars) look (s.pos, s.radius, s.teff);
	return best;
}

bool Scene::DonorFrame (OBJHANDLE body, const VECTOR3 &pos, VECTOR3 &x, VECTOR3 &y) const
{
	if (!body) return false;
	for (DWORD i = 0; i < oapiGetGbodyCount (); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		BLACKHOLEINFO bi;
		if (oapiGetObjectType (h) != OBJTP_BLACKHOLE || !oapiGetBlackHoleInfo (h, &bi) || bi.donor != body) continue;
		VECTOR3 hp;
		oapiGetGlobalPos (h, &hp);
		x = unit (hp - pos);
		y = unit (bi.axis - x * dotp (bi.axis, x));
		return true;
	}
	return false;
}

void Scene::RelShiftColour (const VECTOR3 &gpos, float rgb[3]) const
{
	if (!relOn) return;
	VECTOR3 n = unit (gpos - camPos);
	double b2 = dotp (relBeta, relBeta), D = (1.0 + dotp (n, relBeta)) / std::sqrt (1.0 - b2);
	double in[3] = { rgb[0], rgb[1], rgb[2] }, out[3];
	DopplerShiftRGB (in, D, out);
	for (int k = 0; k < 3; k++) rgb[k] = (float)out[k];
}

void Scene::ObservedPos (OBJHANDLE h, VECTOR3 &p) const
{
	// (light-time: where it was when its light left it; the camera's own
	// vessel moving with it)
	if (h == relSelf) oapiGetGlobalPos (h, &p);
	else oapiGetObservedPos (h, camPos, camVel, &p);
}

void Scene::RenderShadows ()
{
	frameNo++;
	for (bool &v : shadowValid) v = false;
	// planet surfaces select their tiles and upload data first (transfers
	// end rendering scopes, and shadow maps are cleared when bound)
	for (auto &it : visuals) it.second->PrepareFrame ();
	if (!hSun) return;
	if (rtl && rtl->Enabled ()) return; // ray traced shadows instead (glass and probe faces are unshadowed)
	OBJHANDLE hTgt = oapiCameraTarget ();
	VECTOR3 c = camPos;
	double size = 0.0;
	if (hTgt) {
		oapiGetGlobalPos (hTgt, &c);
		size = oapiGetSize (hTgt);
	}
	VECTOR3 sp = light.pos; // (the light: the Sun, or a star near the camera)
	auto render = [&](int i, const VECTOR3 &centre, double radius) {
		shadows[i]->Begin (centre, radius, unit (sp - centre));
		for (auto &it : visuals) it.second->RenderShadow (*shadows[i]);
		shadows[i]->End ();
		shadowValid[i] = true;
	};
	// around the camera target
	double r = std::clamp (SHADOW_RANGE * length (camPos - c) + 2.0 * size, SHADOW_RADIUS_MIN, SHADOW_RADIUS_MAX);
	if (Sunlight (c) > 0.0) render (0, c, r); // else the region is in darkness anyway
	// around the camera, for structures and terrain beyond the first map
	if (Sunlight (camPos) <= 0.0) return;
	for (int k = 0; k < 2; k++)
		if (r < 0.5 * SHADOW_CAM_RADIUS[k]) {
			render (k + 1, camPos, SHADOW_CAM_RADIUS[k]);
			r = SHADOW_CAM_RADIUS[k];
		}
}

double Scene::SunVisibility (const VECTOR3 &gpos, OBJHANDLE exclude) const
{
	// (the light's star, the Sun or another: the bodies in front of it; a
	// black hole's disc, or a star of no body: none)
	if (light.flux <= 0.0 || light.disc || !light.body) return 1.0;
	VECTOR3 s = light.pos - gpos;
	double ds = length (s);
	double as = asin (std::min (1.0, light.radius / ds)); // angular radius of the sun
	double f = 1.0;
	auto occult = [&](OBJHANDLE h) {
		VECTOR3 pp;
		oapiGetGlobalPos (h, &pp);
		VECTOR3 p = pp - gpos;
		double dp = length (p);
		if (dp >= ds) return; // behind the sun
		double ap = asin (std::min (1.0, oapiGetSize (h) / dp)); // angular radius of the body
		double theta = acos (std::clamp (dotp (s, p) / (ds * dp), -1.0, 1.0));
		// linear transition across the penumbra
		double vis = std::clamp ((theta - (ap - as)) / (2.0 * as), 0.0, 1.0);
		f = std::min (f, vis);
	};
	// a point near the body excluded (within 1.1 of its radius: its tiles):
	// only the bodies that can eclipse the light somewhere there. Seen from
	// the body's centre c, a body h at angle thetaC from the light moves by
	// at most asin (Rr / dpc), the light by asin (Rr / dsc), and both look
	// at most as large as from Rr nearer: no eclipse anywhere if thetaC
	// less those shifts is at least their angular radii
	if (exclude) {
		VECTOR3 c;
		oapiGetGlobalPos (exclude, &c);
		double Rr = 1.1 * oapiGetSize (exclude);
		if (length (gpos - c) <= Rr) {
			EclipseCands &ec = eclipseCands[exclude];
			if (ec.frame != frameNo || ec.lightPos.x != light.pos.x || ec.lightPos.y != light.pos.y || ec.lightPos.z != light.pos.z) {
				ec.frame = frameNo, ec.lightPos = light.pos;
				ec.bodies.clear ();
				VECTOR3 sc = light.pos - c;
				double dsc = length (sc);
				for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
					OBJHANDLE h = oapiGetGbodyByIndex (i);
					if (h == light.body || h == exclude) continue;
					VECTOR3 pp;
					oapiGetGlobalPos (h, &pp);
					VECTOR3 p = pp - c;
					double dpc = length (p);
					if (dpc <= 1.001 * Rr || dsc <= 1.001 * Rr) { ec.bodies.push_back (h); continue; }
					if (dpc - Rr >= dsc + Rr) continue; // behind the sun from everywhere there
					double thetaC = atan2 (length (crossp (sc, p)), dotp (sc, p));
					double shift = asin (Rr / dpc) + asin (Rr / dsc);
					double reach = asin (std::min (1.0, oapiGetSize (h) / (dpc - Rr))) + asin (std::min (1.0, light.radius / (dsc - Rr)));
					if (thetaC - shift < reach * 1.001) ec.bodies.push_back (h);
				}
			}
			for (OBJHANDLE h : ec.bodies) occult (h);
			return f;
		}
	}
	for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		if (h != light.body && h != exclude) occult (h);
	}
	return f;
}

double Scene::DeckTransmittance (const VECTOR3 &gpos, bool direct) const
{
	OBJHANDLE hp = oapiCameraProxyGbody ();
	if (!hp || light.flux <= 0.0) return 1.0;
	auto it = visuals.find (hp);
	if (it == visuals.end()) return 1.0;
	const VPlanet *vp = dynamic_cast<const VPlanet*>(it->second.get());
	const VolumetricClouds::Decks *dk = vp ? vp->Decks () : nullptr;
	if (!dk) return 1.0;
	VECTOR3 pp, sp = light.pos;
	oapiGetGlobalPos (hp, &pp);
	VECTOR3 r = gpos - pp;
	double rl = length (r), alt = rl - oapiGetSize (hp);
	if (alt > dk->Top () + 50e3) return 1.0;
	double mu = dotp (r / rl, unit (sp - gpos));
	if (mu < -0.1) return 0.0;
	// (the direct beam and the diffuse light under the decks and the gas)
	ATMPARAM prm;
	oapiGetPlanetAtmParams (hp, rl, &prm);
	double dir, l = dk->Light (alt, mu, prm.p / 1e5, &dir);
	return direct ? dir : l;
}

bool Scene::DeckPath (const VECTOR3 &a, const VECTOR3 &b, double &direct, double &diffuse) const
{
	direct = 1.0, diffuse = 0.0;
	OBJHANDLE hp = oapiCameraProxyGbody ();
	if (!hp) return false;
	auto it = visuals.find (hp);
	if (it == visuals.end()) return false;
	const VPlanet *vp = dynamic_cast<const VPlanet*>(it->second.get());
	const VolumetricClouds::Decks *dk = vp ? vp->Decks () : nullptr;
	if (!dk) return false;
	VECTOR3 pp;
	oapiGetGlobalPos (hp, &pp);
	double R = oapiGetSize (hp), len = length (b - a);
	// optical depths along the segment (16 samples): the decks' and haze's
	// mean extinction (as Decks::ColumnAbove), the gas's Rayleigh
	// scattering and absorption (per bar, over the local scale height)
	const int N = 16;
	double ts = 0.0, tsr = 0.0, ta = 0.0;
	for (int i = 0; i < N; i++) {
		VECTOR3 x = a + (b - a) * ((i + 0.5) / N) - pp;
		double rl = length (x), alt = rl - R, e = 0.0, ea = 0.0;
		for (const VolumetricClouds::Deck &d : dk->decks)
			if (alt > d.base && alt < d.top) e += d.ext * 0.35 * d.coverage, ea += d.ext * 0.35 * d.coverage * (1.0 - d.albedo[1]);
		e += dk->hazeExt * exp (-alt / dk->hazeH);
		ATMPARAM p0, p1;
		oapiGetPlanetAtmParams (hp, rl, &p0);
		oapiGetPlanetAtmParams (hp, rl + 1e3, &p1);
		double bar = p0.p / 1e5, ib = p1.p > 0.0 ? log (p0.p / p1.p) / 1e3 : 0.0;
		double ds = len / N;
		ts += e * ds, tsr += dk->gasRay[1] * bar * ib * ds, ta += (ea + dk->GasAbsRate (1, bar) * bar * ib) * ds;
	}
	// the beam: all of it; what the clouds scatter forwards and diffuse on
	// (conservative scattering through a slab, as Decks::Light: the
	// similarity-scaled depth; the absorption as in the diffusion)
	const double GC = 0.85;
	double tr = (1.0 - GC) * ts + tsr;
	direct = exp (-(ts + tsr + ta));
	double tot = exp (-sqrt (3.0 * ta * (ta + tr))) / (1.0 + 0.75 * tr);
	diffuse = std::max (tot - direct, 0.0);
	return true;
}

double Scene::Sunlight (const VECTOR3 &gpos)
{
	double f = SunVisibility (gpos) * DeckTransmittance (gpos);
	objLight = (float)f;
	return f;
}

RTScene *Scene::RT () const
{
	return rt && rt->Enabled () ? rt.get() : nullptr;
}

void Scene::SetObjectMotion (const Mat4 &modelView, const Mat4 &prevModelView)
{
	// view space now -> object -> view space then -> clip then
	Mat4 m = Mat4::FromFloat (prevProj) * prevModelView * modelView.RigidInverse ();
	m.ToFloat (objMotion);
}

Mat4 Scene::PrevModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const
{
	if (!havePrev) return ModelView (gpos, grot);
	return prevView * Mat4::FromRotTrans (grot, gpos - prevCamPos);
}

Mat4 Scene::ModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const
{
	return view * Mat4::FromRotTrans (grot, gpos - camPos);
}

double Scene::PixelSize (double size, double dist) const
{
	if (dist <= size) return 1e10;
	return size / (dist * tanAperture) * viewH;
}

// ======================================================================
// Rendering

void Scene::WriteAODescriptors ()
{
	VkDescriptorImageInfo dii{ ao->Sampler(), ao->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	for (uint32_t j = 0; j < 2 * vkc::FRAMES_IN_FLIGHT; j++) {
		uint32_t i = j % vkc::FRAMES_IN_FLIGHT;
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = j < vkc::FRAMES_IN_FLIGHT ? frameSet[i] : probeFrameSet[i];
		wr.dstBinding = 3;
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &dii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void Scene::WriteRTLightDescriptors ()
{
	// (without ray tracing: the occlusion image, never read)
	bool on = rtl && rtl->Enabled ();
	VkDescriptorImageInfo dii[2] = {
		{ on ? rtl->Sampler() : ao->Sampler(), on ? rtl->VisView() : ao->View(), on ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ on ? rtl->Sampler() : ao->Sampler(), on ? rtl->LightView() : ao->View(), on ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	for (uint32_t j = 0; j < 2 * vkc::FRAMES_IN_FLIGHT; j++) {
		uint32_t i = j % vkc::FRAMES_IN_FLIGHT;
		for (int k = 0; k < 2; k++) {
			VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			wr.dstSet = j < vkc::FRAMES_IN_FLIGHT ? frameSet[i] : probeFrameSet[i];
			wr.dstBinding = 6 + k;
			wr.descriptorCount = 1;
			wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr.pImageInfo = &dii[k];
			vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
		}
	}
}

void Scene::WriteSSRDescriptors ()
{
	// (the sets of the current frame slot are not in use)
	uint32_t fi = ctx->FrameIndex();
	VkDescriptorImageInfo dii{ ssr->Sampler(), ssr->View(), VK_IMAGE_LAYOUT_GENERAL };
	for (VkDescriptorSet set : { frameSet[fi], probeFrameSet[fi] }) {
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = set;
		wr.dstBinding = 5;
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &dii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void Scene::SetCityMap (VkTexture *fine, VkTexture *coarse, VkTexture *far, OBJHANDLE hPlanet, const VECTOR3 &centre,
	const VECTOR3 &X, const VECTOR3 &Y, double fineSize, double coarseSize, double farSize)
{
	cityMap[0] = fine, cityMap[1] = coarse, cityMap[2] = far;
	cityPlanet = hPlanet;
	cityCentre = centre, cityX = X, cityY = Y;
	citySize[0] = fineSize, citySize[1] = coarseSize, citySize[2] = farSize;
}

void Scene::WriteCityDescriptors ()
{
	// (the sets of the current frame slot are not in use)
	uint32_t fi = ctx->FrameIndex();
	VkTexture *t[3];
	for (int k = 0; k < 3; k++) t[k] = cityMap[k] ? cityMap[k] : cityNone;
	if (cityBound[fi][0] == t[0] && cityBound[fi][1] == t[1] && cityBound[fi][2] == t[2]) return;
	for (int k = 0; k < 3; k++) cityBound[fi][k] = t[k];
	VkDescriptorImageInfo dii[3];
	for (int k = 0; k < 3; k++) dii[k] = { citySampler, t[k]->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	for (VkDescriptorSet set : { frameSet[fi], probeFrameSet[fi] }) {
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = set;
		wr.dstBinding = 9;
		wr.descriptorCount = 3;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = dii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void Scene::ApplySettings ()
{
	const vkc::GfxSettings &g = vkc::Gfx ();
	bool single = ctx->sceneSamples == VK_SAMPLE_COUNT_1_BIT;
	if (ao) ao->enabled = g.ao;
	if (ssr) ssr->enabled = g.ssr && single;
	if (vclouds) vclouds->enabled = g.clouds;
	if (plumes) plumes->enabled = g.plumes, plumes->shimmer = g.shimmer && single;
	if (particles) particles->enabled = g.particles, particles->density = std::clamp (g.particleDensity, 1.0f, 16.0f);
	if (fsr) fsr->sharpness = g.sharpness;
}

void Scene::SkyOptionsChanged ()
{
	skySettings.LoadOptions (gc);
	if (galaxy) galaxy->SetStarLimit (skySettings.starLimit);
	if (starField) starField->SetLimit (skySettings.starLimit);
}

galactic::Observer Scene::SkyObserver () const
{
	galactic::Observer o;
	o.mjd = oapiGetSimMJD ();
	// ORBITER_VK_SKY_OBSERVER=x,y,z[,t]: heliocentric Galactic position [pc]
	// (for tests: the sky seen from elsewhere in the Galaxy), reached from
	// the Sun in t seconds of simulation time if given
	if (const char *e = getenv ("ORBITER_VK_SKY_OBSERVER")) {
		double t = 0.0;
		int n = sscanf (e, "%lf,%lf,%lf,%lf", &o.pos[0], &o.pos[1], &o.pos[2], &t);
		if (n >= 3) {
			if (n == 4 && t > 0.0) {
				double f = std::clamp (oapiGetSimTime () / t, 0.0, 1.0);
				for (auto &x : o.pos) x *= f;
			}
			return o;
		}
	}
	// (from the Sun)
	VECTOR3 sp = _V(0, 0, 0);
	if (hSun) oapiGetGlobalPos (hSun, &sp);
	double p[3] = { camPos.x - sp.x, camPos.y - sp.y, camPos.z - sp.z };
	galactic::GlobalToGalactic (p, o.pos);
	return o;
}

void Scene::SkyAdapt (float avgLogLum, double dt)
{
	// an image metered brighter than 2^-5 (lit objects in view) dims the
	// sky by 2 stops per stop; adapting to the light is fast, to the dark
	// slow. (The sky raised by the settings does not dim itself: the
	// threshold rises with it.)
	double boost = std::log2 (std::max (skySettings.intensity, 1e-3f)) + (skySettings.enhanced ? skySettings.gain : 0.0);
	double target = std::clamp ((avgLogLum + 5.0 - std::max (boost, 0.0)) * 2.0, 0.0, 16.0);
	double rate = target > skyDim ? 2.0 : 0.3;
	skyDim += (target - skyDim) * (1.0 - std::exp (-dt * rate));
}

void Scene::BeginFrame ()
{
	uint32_t fi = ctx->FrameIndex();
	WriteCityDescriptors ();
	if (ao->EnsureSize ()) WriteAODescriptors (), WriteRTLightDescriptors (); // the GPU is idle then
	if (rtl->EnsureSize (ao->PrepassView (), ao->VelocityView (), ao->MaterialView ())) WriteRTLightDescriptors ();
	ssr->EnsureSize (ao->PrepassView (), ao->MaterialView (), ao->VelocityView ());
	ssr->BeginFrame ();
	WriteSSRDescriptors ();
	EnsureReactiveMask ();
	// (before any of their sets are bound in this frame's commands)
	plumes->SetPrepass (ao->PrepassView ());
	particles->SetPrepass (ao->PrepassView ());
	tracks->BeginFrame (fi, camPos, camRot);
	if (rt) rt->BeginFrame ();
	ctx->GpuMark ("begin");
	if (galaxy) {
		// (a star's ring of dust within 2000 AU: the nearest; ecliptic,
		// right-handed: Orbiter's x, z, y)
		const double AU = 1.495978707e11;
		Zodiacal::Ring ring;
		double best = 2000.0 * AU;
		for (DWORD i = 0; i < oapiGetGbodyCount (); i++) {
			OBJHANDLE h = oapiGetGbodyByIndex (i);
			DUSTRINGINFO di;
			if (oapiGetObjectType (h) != OBJTP_STAR || !oapiGetDustRing (h, &di)) continue;
			VECTOR3 sp;
			oapiGetGlobalPos (h, &sp);
			VECTOR3 o = camPos - sp;
			double d = length (o);
			if (d >= best) continue;
			best = d;
			ring.obs[0] = o.x / AU, ring.obs[1] = o.z / AU, ring.obs[2] = o.y / AU;
			ring.axis[0] = di.axis.x, ring.axis[1] = di.axis.z, ring.axis[2] = di.axis.y;
			ring.radius = di.radius / AU, ring.width = di.width / AU, ring.fraction = di.fraction, ring.lum = di.luminosity;
		}
		galaxy->SetDustRing (ring);
		galaxy->Update (SkyObserver (), vkc::FrameDt (oapiGetSysStep ()));
	}
	ctx->GpuMark ("skybake");
	UpdateNearStars ();
	if (starField && skySettings.stars) {
		// (the Sun is drawn as a body; the catalogue's Sun only far from it,
		// where the observer is placed elsewhere)
		galactic::Observer o = SkyObserver ();
		// (relativistic view: the velocity in the Galactic frame, brightnesses seen moving)
		double G[3][3], bg[3] = { 0.0, 0.0, 0.0 };
		galactic::EclipticToGalactic (G);
		for (int i = 0; i < 3 && relOn; i++) bg[i] = G[i][0] * relBeta.x + G[i][1] * relBeta.y + G[i][2] * relBeta.z;
		// (the stars drawn as bodies left out: the Sun while bright, those near)
		std::vector<int> excl;
		for (const NearStar &s : nearStars) if (s.index >= 0) excl.push_back (s.index);
		starField->SetExcluded (excl);
		starField->Update (o, sunAsBody, bg);
		ctx->GpuMark ("starselect");
	}
	// the previous frame does not continue into this one (camera cut)
	cameraCut = !havePrev || length (camPos - prevCamPos) > 5e3;

	// sub-pixel projection jitter for temporal anti-aliasing (Halton 2, 3)
	jitter[0] = jitter[1] = 0.0f;
	if (taaEnabled) {
		auto halton = [](uint32_t i, uint32_t b) {
			double f = 1.0, r = 0.0;
			for (; i; i /= b) { f /= b; r += f * (i % b); }
			return r;
		};
		if (useFsr) { // the upscaler's sequence (longer when upscaling)
			FsrUpscaler::Jitter (jitterIndex++, viewW, ctx->Extent().width, jitterPixels[0], jitterPixels[1]);
		} else {
			jitterIndex = jitterIndex % 16 + 1;
			jitterPixels[0] = (float)(halton (jitterIndex, 2) - 0.5);
			jitterPixels[1] = (float)(halton (jitterIndex, 3) - 0.5);
		}
		// (Vulkan clip space: y down, as pixel rows)
		jitter[0] = 2.0f * jitterPixels[0] / viewW;
		jitter[1] = 2.0f * jitterPixels[1] / viewH;
	}
	proj[8] = jitter[0]; // clip.xy += jitter * z (= w)
	proj[9] = jitter[1];
	// (the lensed sky: its rays at the jittered pixels)
	UpdateLens ();
	ctx->GpuMark ("lens");
	if (!havePrev) {
		memcpy (prevProj, proj, sizeof(prevProj));
		prevProj[8] = prevProj[9] = 0.0f;
	}
	SetObjectMotion (Mat4::Identity (), Mat4::Identity ());

	// engine plumes (reflections march through them)
	auto tc0 = std::chrono::steady_clock::now ();
	plumes->BeginFrame (oapiGetSimTime ());
	for (auto &it : visuals) it.second->CollectPlumes (*plumes);
	{
		// (ORBITER_VK_PROFILE: CPU time of the plume collection, surface tests included)
		static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
		static double acc = 0.0;
		static int n = 0;
		acc += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - tc0).count ();
		if (profile && ++n == 120) {
			oapiWriteLogV ("VulkanClient: plume collection (CPU) %.3f ms, %u plumes", acc / n, plumes->Count ());
			acc = 0.0, n = 0;
		}
	}
	ssr->SetPlumeSlot (plumes->Slot ());

	// frame uniforms
	FrameUniforms fu{};
	FillFrameUniforms (&fu);
	fu.probe[0] = probeFacesDone >= 6 ? 1.0f : 0.0f;
	fu.probe[1] = (float)(EnvProbe::LEVELS - 1);
	// (relativistic view: no screen-space reflections, occlusion or traced lighting: they
	// rebuild positions from the depth, where geometry is seen aberrated)
	fu.probe[2] = ssr->Valid () && !cameraCut && !relOn ? 1.0f : 0.0f;
	fu.probe[3] = ssr->debug ? 1.0f : 0.0f;
	fu.rt[0] = rtl->Enabled () && !relOn ? 1.0f : 0.0f;
	if (relOn) fu.aoParams[3] = 1.0f;
	fu.rt[1] = (float)RTLighting::SHADOWED_LIGHTS;
	memcpy (frameUbo[fi].mapped, &fu, sizeof(fu));
	UpdateCamAtmSet ();
}

void Scene::UpdateCamAtmSet ()
{
	// atmosphere of the planet the camera is at, for vessels and bases
	camAtmSet = atm->NoAtmosphereSet ();
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		auto it = visuals.find (hp);
		if (it != visuals.end())
			if (Atmosphere *a = it->second->GetAtmosphere ())
				camAtmSet = a->Set (); // also without atmosphere: the planet is reflected
	}
}

void Scene::PlanetAlbedo (OBJHANDLE hp, float alb[3])
{
	// Bond albedo and tint of the bodies most often seen up close
	char name[64];
	oapiGetObjectName (hp, name, 64);
	struct { const char *name; float a[3]; } tab[] = {
		{ "Earth", { 0.27f, 0.30f, 0.36f } }, { "Moon", { 0.12f, 0.12f, 0.11f } },
		{ "Mars", { 0.30f, 0.20f, 0.14f } }, { "Mercury", { 0.09f, 0.09f, 0.085f } },
		{ "Venus", { 0.78f, 0.72f, 0.58f } }, { "Jupiter", { 0.36f, 0.33f, 0.28f } },
		{ "Saturn", { 0.36f, 0.33f, 0.26f } }, { "Titan", { 0.27f, 0.22f, 0.14f } } };
	for (auto &t : tab)
		if (!strcmp (name, t.name)) { for (int k = 0; k < 3; k++) alb[k] = t.a[k]; return; }
	alb[0] = alb[1] = alb[2] = 0.25f;
}

void Scene::FillFrameUniforms (void *pfu)
{
	FrameUniforms &fu = *(FrameUniforms*)pfu;
	// the cities' road map: its centre and axes in view space
	if (cityMap[0] && cityMap[1] && cityMap[2] && cityPlanet) {
		MATRIX3 Rp;
		oapiGetRotationMatrix (cityPlanet, &Rp);
		VECTOR3 gp;
		oapiGetGlobalPos (cityPlanet, &gp);
		VECTOR3 o = view.TransformPoint (gp + mul (Rp, cityCentre) - camPos);
		VECTOR3 x = view.TransformPoint (mul (Rp, cityX)), y = view.TransformPoint (mul (Rp, cityY));
		for (int k = 0; k < 3; k++) fu.city[0][k] = (float)o.data[k], fu.city[1][k] = (float)x.data[k], fu.city[2][k] = (float)y.data[k];
		fu.city[0][3] = 1.0f;
		fu.city[3][0] = (float)(1.0 / citySize[0]), fu.city[3][1] = (float)(1.0 / citySize[1]), fu.city[3][2] = (float)(1.0 / citySize[2]);
	}
	fu.city[3][3] = cityAlt; // (the hand-off to the night light texture: with or without the maps)
	// relativistic view: the camera's velocity, its own vessel (seen as it is)
	if (relOn) {
		VECTOR3 bv = RelBetaView ();
		for (int k = 0; k < 3; k++) fu.relBeta[k] = (float)bv.data[k];
		fu.relBeta[3] = 1.0f;
		if (relSelf) {
			VECTOR3 sp;
			oapiGetGlobalPos (relSelf, &sp);
			VECTOR3 o = view.TransformPoint (sp - camPos);
			for (int k = 0; k < 3; k++) fu.relSelf[k] = (float)o.data[k];
			fu.relSelf[3] = (float)relSelfRad;
		}
	}
	fu.jitter[0] = jitter[0], fu.jitter[1] = jitter[1];
	fu.jitter[2] = (float)viewW, fu.jitter[3] = (float)viewH;
	fu.aoParams[0] = 1.0f / ao->Extent().width;
	fu.aoParams[1] = 1.0f / ao->Extent().height;
	fu.aoParams[2] = ao->directFrac;
	memcpy (fu.proj, proj, sizeof(proj));
	// the light: the Sun, or a star near the camera; in its colour, the
	// Sun's white, fading where its flux falls below the Sun's at 100 AU
	VECTOR3 sunDir = _V(1, 0, 0);
	if (hSun || !light.sun) {
		sunDir = light.pos - camPos;
		double len = length (sunDir);
		if (len > 0) sunDir /= len;
	}
	VECTOR3 sv = view.TransformPoint (sunDir);
	fu.sunDir[0] = (float)sv.x, fu.sunDir[1] = (float)sv.y, fu.sunDir[2] = (float)sv.z;
	float lightRGB[3];
	StarDiscRGB (light.teff, lightRGB);
	double lightLum = 0.2126 * lightRGB[0] + 0.7152 * lightRGB[1] + 0.0722 * lightRGB[2];
	double lightScale = std::min (1.0, light.flux / NEAR_FLUX) / std::max (lightLum, 1e-6);
	// (an accretion disc: a large part of the sky at about a star's surface
	// brightness, the exposure adapted to it: its light brighter than the
	// Sun's at 1 AU, compressed; gently, as the exposure is the sky's when
	// facing away from it, where more blew lit surfaces out to white)
	if (light.disc && light.flux > 1.0) lightScale *= std::min (3.0, 1.0 + 0.5 * std::log10 (light.flux));
	// (as near a star: within a few of its radii its disc fills much of the
	// sky, the exposure adapted to it; inside Mercury's orbit as before)
	else if (light.flux > 10.0) lightScale *= std::min (10.0, 1.0 + 2.0 * std::log10 (light.flux / 10.0));
	// (the extra lights: as bright as the light's white, times their flux
	// ratio compressed, in their colours)
	double extraBase = lightScale * std::max (lightLum, 1e-6);
	lightScale *= light.flare; // (a flaring red dwarf: its light rises)
	for (int k = 0; k < 3; k++) fu.sunColor[k] = (float)(lightRGB[k] * lightScale);
	// relativistic view: the sunlight on the camera's own vessel in its frame,
	// from the aberrated direction, Doppler shifted (radiance), on a disc 1/D^2
	// the solid angle (irradiance)
	for (int k = 0; k < 4; k++) fu.relSunDir[k] = fu.sunDir[k], fu.relSunColor[k] = fu.sunColor[k];
	// a black hole's lens on the objects behind it
	for (int k = 0; k < 4; k++) fu.lens[k] = 0.0f;
	if (lensOn) {
		VECTOR3 hv = view.TransformPoint (lensPos - camPos);
		fu.lens[0] = (float)hv.x, fu.lens[1] = (float)hv.y, fu.lens[2] = (float)hv.z, fu.lens[3] = (float)lensM;
	}
	if (relOn) {
		double b2 = dotp (relBeta, relBeta), g = 1.0 / std::sqrt (1.0 - b2), nb = dotp (sunDir, relBeta);
		VECTOR3 n = (sunDir + relBeta * (g * g / (g + 1.0) * nb + g)) / (g * (1.0 + nb));
		VECTOR3 nv = view.TransformPoint (unit (n));
		for (int k = 0; k < 3; k++) fu.relSunDir[k] = (float)nv.data[k];
		// (a black body of T seen at D: one of D T)
		double D = g * (1.0 + nb);
		float c[3];
		StarDiscRGB (light.teff * D, c);
		for (int k = 0; k < 3; k++) fu.relSunColor[k] = (float)(c[k] * lightScale / (D * D));
	}
	// ambient light: the user's ambient level, plus skylight in daylight
	// inside an atmosphere (tinted with the sky colour)
	const DWORD *amb = (const DWORD*)gc->GetConfigParam (CFGPRM_AMBIENTLEVEL);
	float a0 = std::max (0.01f, amb ? (float)*amb / 255.0f * 0.5f : 0.03f);
	fu.ambient[0] = fu.ambient[1] = fu.ambient[2] = a0;
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		const ATMCONST *ac = oapiPlanetHasAtmosphere (hp) ? oapiGetPlanetAtmConstants (hp) : nullptr;
		if (ac && ac->rho0 > 0.0) {
			VECTOR3 pp;
			oapiGetGlobalPos (hp, &pp);
			ATMPARAM prm;
			oapiGetPlanetAtmParams (hp, length (camPos - pp), &prm);
			double f = 0.35 * std::sqrt (std::clamp (prm.rho / ac->rho0, 0.0, 1.0)) * Sunlight (camPos);
			char pname[64];
			oapiGetObjectName (hp, pname, 64);
			float sky[3], tint[3];
			// (the skylight in the star's colour)
			StarDiscRGB (light.teff, tint);
			double tl = std::max (1e-6, 0.2126 * tint[0] + 0.7152 * tint[1] + 0.0722 * tint[2]);
			if (OvercastSkylight (pname, sky)) { // (under a cloud deck or haze: lit mostly by the sky)
				double d = std::sqrt (std::clamp (prm.rho / ac->rho0, 0.0, 1.0)) * Sunlight (camPos);
				for (int k = 0; k < 3; k++) fu.ambient[k] += (float)(d * sky[k] * tint[k] / tl);
			} else {
				fu.ambient[0] += (float)(f * ac->color0.x * tint[0] / tl);
				fu.ambient[1] += (float)(f * ac->color0.y * tint[1] / tl);
				fu.ambient[2] += (float)(f * ac->color0.z * tint[2] / tl);
			}
		}
	}
	for (int k = 0; k < 3; k++) ambientRGB[k] = fu.ambient[k];
	// (w: the ambient's share left on the night side, at the camera: the
	// terrain's rule (tile_main.glsl: no skylight, a fraction of the ambient
	// level for the moon and the stars), for what is lit at the camera's
	// place without a surface normal of its own (the particles); 1 in
	// sunlight)
	fu.ambient[3] = 1.0f;
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		VECTOR3 pp;
		oapiGetGlobalPos (hp, &pp);
		double s = std::clamp ((dotp (unit (camPos - pp), sunDir) + 0.25) / 0.25, 0.0, 1.0);
		fu.ambient[3] = (float)std::max (0.15 + 0.85 * s * s * (3.0 - 2.0 * s), SunVisibility (camPos));
		// (contrails: at night about a fifth, so that they darken or lighten
		// the sky by no more than that)
		if (particles) particles->contrailNight = 0.22f + 0.78f * std::clamp ((fu.ambient[3] - 0.15f) / 0.85f, 0.0f, 1.0f);
		// (and no less than the night sky's own level: smoke and contrails
		// that are unlit at night showed as black bands against it (the
		// terrain keeps its 0.15); an absolute floor, 0.45 of the default
		// ambient level 0.01 (Orbiter's AmbientLevel 2: about the sky as drawn there), so that a user's
		// higher ambient level does not lift the smoke further)
		const float NIGHT_SMOKE_LIGHT = 0.45f * 0.01f;
		fu.ambient[3] = std::max (fu.ambient[3], std::min (NIGHT_SMOKE_LIGHT / a0, 1.0f));
	}
	// sunlight reflected by the planet nearby (a Lambertian sphere, by its phase)
	if (OBJHANDLE hp = oapiCameraProxyGbody ())
		if (hp != light.body && light.flux > 0.0) {
			VECTOR3 pp, sp = light.pos;
			oapiGetGlobalPos (hp, &pp);
			double R = oapiGetSize (hp);
			VECTOR3 toP = pp - camPos;
			double d = std::max (length (toP), R);
			double alpha = acos (std::clamp (dotp (unit (sp - pp), unit (camPos - pp)), -1.0, 1.0));
			double phase = (sin (alpha) + (PI - alpha) * cos (alpha)) / PI;
			double e = (R / d) * (R / d) * phase;
			float alb[3];
			PlanetAlbedo (hp, alb);
			VECTOR3 dv = view.TransformPoint (toP / length (toP));
			fu.planetDir[0] = (float)dv.x, fu.planetDir[1] = (float)dv.y, fu.planetDir[2] = (float)dv.z;
			fu.planetDir[3] = (float)std::min (1.0, R / d);
			for (int k = 0; k < 3; k++) fu.planetLight[k] = (float)(e * alb[k]);
			fu.planetLight[3] = e > 1e-5 ? 1.0f : 0.0f;
		}
	auto shadowUniforms = [this](const ShadowMap &sm, float *mat, float *prm) {
		sm.ReceiverMatrix (camPos, camRot).ToFloat (mat);
		double texel = sm.TexelSize ();
		prm[0] = 1.0f;
		prm[1] = (float)(1.5 * texel);                       // normal offset [m]
		prm[2] = (float)(texel / (6.0 * sm.Radius()));       // depth bias (one texel in depth units)
		prm[3] = 1.0f / ShadowMap::SIZE;
	};
	for (int i = 0; i < SHADOW_CASCADES; i++)
		if (shadowValid[i]) shadowUniforms (*shadows[i], fu.shadowMat[i], fu.shadowParams[i]);
	// local light sources, nearest to the camera first
	std::vector<LightSource> ls;
	for (auto &it : visuals) it.second->CollectLights (ls);
	// lightning in a gas giant's storms (the flashes of the last frame)
	for (const VolumetricClouds::Flash &fl : vclouds->Flashes ()) {
		if (fl.brightness <= 0.01) continue;
		// (through the clouds between: the beam, shadowed by the vessels'
		// meshes, and the diffused light, from all about: ambient)
		double dir = 1.0, dif = 0.0;
		DeckPath (fl.gpos, camPos, dir, dif);
		const float tint[3] = { 0.80f, 0.86f, 1.0f };
		double d2 = dotp (fl.gpos - camPos, fl.gpos - camPos);
		for (int k = 0; k < 3; k++) {
			float a = (float)(0.6 * dif * fl.brightness * 1.2e10 * tint[k] / (d2 + 25e6));
			fu.ambient[k] += a;
		}
		if (dir < 1e-4) continue;
		LightSource l{};
		l.pos = fl.gpos, l.dir = _V(0, 1, 0);
		for (int k = 0; k < 3; k++) l.col[k] = (float)(dir * fl.brightness * 1.2e10 * tint[k]);
		l.range = 300e3;
		l.att[0] = 25e6, l.att[1] = 0.0, l.att[2] = 1.0; // (inverse square, softened within 5 km)
		l.spot = false;
		l.owner = 0, l.shadowOfs = 0.0;
		ls.push_back (l);
	}
	std::sort (ls.begin(), ls.end(), [this](const LightSource &a, const LightSource &b) {
		return length (a.pos - camPos) - std::max (a.range, a.sortRange) < length (b.pos - camPos) - std::max (b.range, b.sortRange);
	});
	// the extra lights first (directional, ray traced shadows: the first four)
	int ne = 0;
	for (const StarLight &x : extraLights) {
		VECTOR3 r = x.pos - camPos;
		double d = std::max (length (r), 1.0);
		VECTOR3 dv = view.TransformPoint (r / d);
		float rgb[3];
		StarDiscRGB (x.teff, rgb);
		double lum = std::max (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2], 1e-6);
		double sc = extraBase / lum * std::pow (x.flux / std::max (light.flux, 1e-30), EXTRA_LIGHT_GAMMA) * x.flare;
		auto &u = fu.lights[ne++];
		u = {};
		u.pos[3] = 1e30f;
		u.dir[0] = (float)dv.x, u.dir[1] = (float)dv.y, u.dir[2] = (float)dv.z, u.dir[3] = 3.0f;
		for (int k = 0; k < 3; k++) u.col[k] = (float)(rgb[k] * sc);
		u.att[3] = (float)std::asin (std::min (1.0, x.radius / d)); // (its angular radius: soft shadows)
	}
	int nl = (int)std::min (ls.size(), (size_t)(MAX_LIGHTS - ne));
	fu.lightCount[0] = (float)(nl + ne);
	for (int i = 0; i < nl; i++) {
		const LightSource &l = ls[i];
		VECTOR3 p = view.TransformPoint (l.pos - camPos), d = view.TransformPoint (l.dir);
		auto &u = fu.lights[ne + i];
		u.pos[0] = (float)p.x, u.pos[1] = (float)p.y, u.pos[2] = (float)p.z, u.pos[3] = (float)l.range;
		u.dir[0] = (float)d.x, u.dir[1] = (float)d.y, u.dir[2] = (float)d.z, u.dir[3] = l.spot ? 2.0f : 1.0f;
		for (int k = 0; k < 3; k++) u.col[k] = l.col[k], u.att[k] = (float)l.att[k];
		u.att[3] = l.spot ? (float)cos (0.5 * l.umbra) : (float)l.radius; // (a point light: its source's radius)
		u.spot[0] = (float)cos (0.5 * l.penumbra);
		u.spot[1] = (float)l.owner;
		u.spot[2] = (float)l.shadowOfs;
	}
	// the flames the smoke volume's light is occluded towards
	// (smoke_resolve.comp): the brightest vessel's (its flames' points by
	// their power, inverse square: col / att.z, summed), a point of them and
	// the farthest from it (soft shadows), and the next vessel's
	fu.lightCount[1] = fu.lightCount[2] = fu.lightCount[3] = -1.0f;
	{
		auto flame = [&](int i) { return fu.lights[i].dir[3] < 1.5f && fu.lights[i].att[3] > 0.0f && fu.lights[i].att[2] > 0.0f; };
		auto power = [&](int i) { const float *c = fu.lights[i].col; return (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) / fu.lights[i].att[2]; };
		float best[2] = { 0.0f, 0.0f };
		int pick[2] = { -1, -1 };
		for (int i = ne; i < ne + nl; i++) {
			if (!flame (i)) continue;
			float sum = 0.0f;
			bool first = true;
			for (int j = ne; j < ne + nl; j++)
				if (flame (j) && fu.lights[j].spot[1] == fu.lights[i].spot[1]) {
					if (j < i && power (j) >= power (i)) first = false;
					sum += power (j);
				}
			if (!first) continue; // (each vessel once: by its brightest point)
			if (sum > best[0]) best[1] = best[0], pick[1] = pick[0], best[0] = sum, pick[0] = i;
			else if (sum > best[1]) best[1] = sum, pick[1] = i;
		}
		if (pick[0] >= 0) {
			fu.lightCount[1] = (float)pick[0];
			float far = 1.0f;
			for (int j = ne; j < ne + nl; j++) {
				if (!flame (j) || fu.lights[j].spot[1] != fu.lights[pick[0]].spot[1]) continue;
				float d = 0.0f;
				for (int k = 0; k < 3; k++) d += (fu.lights[j].pos[k] - fu.lights[pick[0]].pos[k]) * (fu.lights[j].pos[k] - fu.lights[pick[0]].pos[k]);
				if (d > far * far) far = std::sqrt (d), fu.lightCount[2] = (float)j;
			}
		}
		if (pick[1] >= 0) fu.lightCount[3] = (float)pick[1];
	}
	Mat4::FromRotTrans (camRot, _V(0, 0, 0)).ToFloat (fu.viewToGlobal);
}


void Scene::RenderPrepass ()
{
	// (planet surfaces were prepared before the shadow maps)
	ao->BeginPrepass ();
	for (auto &it : visuals) {
		objOwner = it.second->Id ();
		VECTOR3 v{};
		if (it.second->Handle ()) oapiGetGlobalVel (it.second->Handle (), &v);
		objVel[0] = (float)v.x, objVel[1] = (float)v.y, objVel[2] = (float)v.z;
		it.second->Render (RenderPass::Depth);
	}
	objOwner = 0;
	objVel[0] = objVel[1] = objVel[2] = 0.0f;
	ctx->EndTarget ();
	ctx->GpuMark ("prepass");
	if (ao->enabled && !rtl->Enabled () && !relOn) ao->Compute (proj[0], -proj[5]);
	ctx->GpuMark ("ao");
	if (rt && rt->Enabled ()) {
		rt->Build ();
		ctx->GpuMark ("rt build");
		if (rtl->Enabled () && !relOn) {
			double sunRadius = 0.0047;
			if (hSun || !light.sun) // (the light's disc)
				sunRadius = asin (std::min (1.0, light.radius / std::max (length (light.pos - camPos), 1.0)));
			rtl->Compute (frameSet[ctx->FrameIndex()], rt->Set (), rt->TextureSet (), proj[0], -proj[5], jitter, (float)sunRadius, cameraCut);
			ctx->GpuMark ("rt lighting");
		}
		ssr->SetRTSets (frameSet[ctx->FrameIndex()], rt->Set (), rt->TextureSet (), camAtmSet);
	}
	if (!relOn) ssr->Compute (proj[0], -proj[5], jitter, cameraCut);
	ctx->GpuMark ("ssr");
	{
		VkDescriptorSet rtSets[2] = { rt && rt->Enabled () ? rt->Set () : VK_NULL_HANDLE, rt && rt->Enabled () ? rt->TextureSet () : VK_NULL_HANDLE };
		VECTOR3 sunDir = _V(1, 0, 0);
		if (hSun || !light.sun) sunDir = unit (light.pos - camPos);
		particles->Simulate (camPos, sunDir, frameSet[ctx->FrameIndex()], camAtmSet, rtSets[0] ? rtSets : nullptr, oapiGetSimTime ());
		particles->RenderScreenSmoke (frameSet[ctx->FrameIndex()]);
		ctx->GpuMark ("particles");
	}
	if (!relOn) RenderVolumetricClouds (); // (relativistic view: it marches through the depth seen)
	ctx->GpuMark ("clouds");
}

void Scene::RenderProbe ()
{
	if (!probeEnabled) return;
	OBJHANDLE hT = oapiCameraTarget ();
	if (!hT || oapiGetObjectType (hT) != OBJTP_VESSEL) { probeFacesDone = 0; return; }
	if (hT != probeTarget) probeTarget = hT, probeFacesDone = 0;
	VECTOR3 pos;
	oapiGetGlobalPos (hT, &pos);

	// camera of the cube face (Vulkan face order +x -x +y -y +z -z): right,
	// up and forward axes (global frame)
	static const VECTOR3 F[6][3] = {
		{ {0,0,-1}, {0,1,0}, {1,0,0} },  { {0,0,1}, {0,1,0}, {-1,0,0} },
		{ {1,0,0}, {0,0,-1}, {0,1,0} },  { {1,0,0}, {0,0,1}, {0,-1,0} },
		{ {1,0,0}, {0,1,0}, {0,0,1} },   { {-1,0,0}, {0,1,0}, {0,0,-1} } };
	int f = probeFace;
	const VECTOR3 &r = F[f][0], &u = F[f][1], &fw = F[f][2];

	// save the main view
	VECTOR3 sCamPos = camPos;
	MATRIX3 sCamRot = camRot;
	Mat4 sView = view;
	float sProj[16];
	memcpy (sProj, proj, sizeof(proj));
	uint32_t sW = viewW, sH = viewH;
	double sTan = tanAperture;
	VkDescriptorSet sAtm = camAtmSet;
	bool sClouds = cloudsActive;

	camPos = pos;
	camRot = { r.x, u.x, fw.x,  r.y, u.y, fw.y,  r.z, u.z, fw.z }; // columns: camera axes
	MATRIX3 vt = { camRot.m11, camRot.m21, camRot.m31, camRot.m12, camRot.m22, camRot.m32, camRot.m13, camRot.m23, camRot.m33 };
	view = Mat4::FromRotTrans (vt, _V(0, 0, 0));
	memset (proj, 0, sizeof(proj));
	proj[0] = 1.0f, proj[5] = -1.0f, proj[11] = 1.0f, proj[14] = (float)nearPlane; // 90 degrees
	viewW = viewH = EnvProbe::SIZE;
	tanAperture = 1.0;
	cloudsActive = false;
	inProbe = true;

	FrameUniforms fu{};
	float sJit[2] = { jitter[0], jitter[1] };
	jitter[0] = jitter[1] = 0.0f;
	FillFrameUniforms (&fu);
	jitter[0] = sJit[0], jitter[1] = sJit[1];
	fu.aoParams[3] = 1.0f; // no ambient occlusion
	fu.probe[0] = 0.0f;    // reflections from the analytic environment
	memcpy (probeFrameUbo[ctx->FrameIndex()].mapped, &fu, sizeof(fu));
	UpdateCamAtmSet ();

	vkc::Target ft = probe->FaceTarget (), fd = probe->FaceDepth ();
	ctx->SetSceneRedirect (&ft, &fd);
	ctx->BindScene (true);
	// (no stars: at the probe's resolution they would cover the sky with
	// speckle, and their reflections are too faint to see)
	for (auto &it : visuals) it.second->RenderSky ();
	for (auto &it : visuals)
		if (it.first != hT) it.second->Render (RenderPass::Opaque);
	ctx->GpuMark ("probe opaque");
	ctx->BindScene (true);
	for (auto &it : visuals) // cloud layers (vessel exhaust and glass are left out)
		if (dynamic_cast<VPlanet*>(it.second.get())) it.second->Render (RenderPass::Transparent);
	ctx->SetSceneRedirect (nullptr, nullptr);
	probe->StoreFace (f);
	probe->Prefilter ();

	// restore the main view
	inProbe = false;
	camPos = sCamPos, camRot = sCamRot, view = sView;
	memcpy (proj, sProj, sizeof(proj));
	viewW = sW, viewH = sH, tanAperture = sTan;
	camAtmSet = sAtm, cloudsActive = sClouds;
	probeFace = (probeFace + 1) % 6;
	if (probeFacesDone < 6) probeFacesDone++;
}

bool Scene::SunOnScreen (float uv[2]) const
{
	// (the lens flare's ghosts are a compact source's: none of an accretion
	// disc, spread over the sky)
	if (light.flux <= 0.0 || light.disc) return false;
	VECTOR3 sp = light.pos;
	VECTOR3 v = ViewDir (unit (sp - camPos));
	if (v.z <= 1e-3) return false;
	uv[0] = (float)(0.5 + 0.5 * proj[0] * v.x / v.z);
	uv[1] = (float)(0.5 + 0.5 * proj[5] * v.y / v.z);
	return uv[0] > -0.2f && uv[0] < 1.2f && uv[1] > -0.2f && uv[1] < 1.2f;
}

float Scene::CloudFadeRadius () const
{
	return cloudsActive ? (float)VolumetricClouds::RANGE : 0.0f;
}

void Scene::RenderVolumetricClouds ()
{
	cloudsActive = false;
	OBJHANDLE hp = oapiCameraProxyGbody ();
	if (!hp || !vclouds->enabled) return;
	auto it = visuals.find (hp);
	if (it == visuals.end()) return;
	VPlanet *vp = dynamic_cast<VPlanet*>(it->second.get());
	Atmosphere *a = vp ? vp->GetAtmosphere () : nullptr;
	if (vp && vp->Decks () && a && a->Present ()) {
		// a gas giant: its cloud decks, within reach of them
		const VolumetricClouds::Decks *dk = vp->Decks ();
		VECTOR3 pp;
		MATRIX3 prot;
		oapiGetGlobalPos (hp, &pp);
		oapiGetRotationMatrix (hp, &prot);
		double R = oapiGetSize (hp);
		VECTOR3 cam = tmul (prot, camPos - pp);
		if (length (cam) - R > dk->Top () + dk->reach) return;
		VolumetricClouds::GasGiantFrame f{};
		f.hPlanet = hp;
		f.decks = dk;
		f.atmSet = a->Set ();
		f.camPlanet = cam;
		f.radius = R;
		f.floorRadius = R - GG_FLOOR_DEPTH;
		f.prepass = ao->PrepassView ();
		f.fx = proj[0], f.fy = -proj[5];
		// skylight above the decks: the sunlit haze's colour
		const ATMCONST *ac = oapiGetPlanetAtmConstants (hp);
		for (int k = 0; k < 3; k++) f.sky[k] = 0.35f * (float)(ac ? (&ac->color0.x)[k] : 0.5) + 0.05f;
		f.time = oapiGetSimTime ();
		f.viewToPlanet = mul (transp (prot), camRot);
		vclouds->RenderGasGiant (f);
		cloudsActive = vclouds->Active ();
		return;
	}
	if (!vp || !vp->CloudLayer () || !a || !a->Present ()) return;
	VECTOR3 pp;
	MATRIX3 prot;
	oapiGetGlobalPos (hp, &pp);
	oapiGetRotationMatrix (hp, &prot);
	double R = oapiGetSize (hp);
	VECTOR3 cam = tmul (prot, camPos - pp);
	if (length (cam) - R > VolumetricClouds::MAX_CAM_ALT) return;
	VolumetricClouds::Frame f{};
	f.clouds = vp->CloudLayer ();
	f.atmSet = a->Set ();
	f.camPlanet = cam;
	f.planetRadius = R;
	f.cloudAlt = vp->CloudAlt ();
	f.prepass = ao->PrepassView ();
	f.fx = proj[0], f.fy = -proj[5];
	for (int k = 0; k < 3; k++) f.ambient[k] = ambientRGB[k];
	// slow drift of the cloud shapes
	double t = fmod (oapiGetSimTime (), 1e5);
	f.windOffset[0] = (float)(8.0 * t), f.windOffset[1] = 0.0f, f.windOffset[2] = (float)(5.0 * t);
	f.viewToPlanet = mul (transp (prot), camRot);
	vclouds->Render (f);
	cloudsActive = true;
}

void Scene::Render ()
{
	VkCommandBuffer cmd = ctx->Cmd();
	uint32_t fi = ctx->FrameIndex();

	// background stars
	float viewf[16], vp[16];
	view.ToFloat (viewf);
	MulMat4f (proj, viewf, vp);
	if (galaxy) {
		// view -> ecliptic (camRot) -> Galactic
		double G[3][3], R[3][3];
		galactic::EclipticToGalactic (G);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				R[i][j] = G[i][0] * camRot.data[j] + G[i][1] * camRot.data[3 + j] + G[i][2] * camRot.data[6 + j];
		float gain = (float)(skyDarkGain * std::exp2 (-skyDim));
		if (skySettings.milkyWay) {
			GalaxySky::Display d;
			d.gain = gain * skySettings.intensity;
			if (skySettings.enhanced) {
				d.gain *= std::exp2 (skySettings.gain);
				d.contrast = skySettings.contrast, d.highlight = skySettings.highlight, d.saturation = skySettings.saturation;
				d.camera = true;
			}
			VECTOR3 bv = RelBetaView ();
			float bvf[3] = { (float)bv.x, (float)bv.y, (float)bv.z };
			if (lensOn) lens->Render (cmd, d, (float)STAR_BRIGHTNESS); // (near a black hole: the sky through its spacetime, the background with it, its disc)
			else if (!getenv ("ORBITER_VK_SKY_COSMIC_ONLY")) galaxy->Render (cmd, R, (float)ProjX (), (float)ProjY (), d, bvf);
			// the extragalactic background, seen moving relative to the cosmic
			// microwave background: the Solar System's barycentre at 369.82 km/s
			// towards l 264.021, b 48.253 (Planck 2018), the camera's velocity
			// added (relativistically); drawn where it is shifted to be seen
			{
				const double DEG = 3.14159265358979323846 / 180.0;
				double l = 264.021 * DEG, b = 48.253 * DEG, gd[3] = { std::cos (b) * std::cos (l), std::cos (b) * std::sin (l), std::sin (b) };
				VECTOR3 V = _V(G[0][0] * gd[0] + G[1][0] * gd[1] + G[2][0] * gd[2],
				               G[0][1] * gd[0] + G[1][1] * gd[1] + G[2][1] * gd[2],
				               G[0][2] * gd[0] + G[1][2] * gd[1] + G[2][2] * gd[2]) * (369.82e3 / 299792458.0);
				VECTOR3 v = relOn ? relBeta : _V(0, 0, 0);
				double gV = 1.0 / std::sqrt (1.0 - dotp (V, V)), Vv = dotp (V, v);
				VECTOR3 u = (V + v / gV + V * (gV / (1.0 + gV) * Vv)) / (1.0 + Vv);
				double ul = std::min (length (u), 1.0 - 1e-15);
				// (ORBITER_VK_SKY_COSMIC=0: none; ORBITER_VK_SKY_COSMIC_ONLY: it alone, the Milky Way and the stars left out)
				const char *ce = getenv ("ORBITER_VK_SKY_COSMIC");
				if (!lensOn && (!ce || atoi (ce) != 0) && std::sqrt ((1.0 + ul) / (1.0 - ul)) > COSMIC_BG_DOPPLER) {
					VECTOR3 uv = view.TransformPoint (u);
					float uvf[3] = { (float)uv.x, (float)uv.y, (float)uv.z };
					galaxy->RenderBackground (cmd, (float)ProjX (), (float)ProjY (), d, uvf);
				}
			}
		}
		if (starField && skySettings.stars && !lensOn && !getenv ("ORBITER_VK_SKY_COSMIC_ONLY")) {
			// Galactic direction -> ecliptic (G^T) -> clip
			float gvp[16];
			for (int r = 0; r < 4; r++) {
				for (int c = 0; c < 3; c++)
					gvp[c * 4 + r] = (float)(vp[r] * G[c][0] + vp[4 + r] * G[c][1] + vp[8 + r] * G[c][2]);
				gvp[12 + r] = vp[12 + r];
			}
			ctx->GpuMark ("milky way");
			// (the camera's velocity in the Galactic frame: aberrated star directions)
			float bg[3] = { 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < 3 && relOn; i++) bg[i] = (float)(G[i][0] * relBeta.x + G[i][1] * relBeta.y + G[i][2] * relBeta.z);
			starField->Render (cmd, gvp, (float)ProjX (), (float)ProjY (), gain, galaxy->ShownLimit (), bg);
			ctx->GpuMark ("stars");
		}
	}
	stars->SetView (vp);

	std::vector<VObject*> list;
	for (auto &it : visuals) list.push_back (it.second.get());

	// skies of planetary atmospheres, over the stars
	for (auto *vo : list) vo->RenderSky ();
	ctx->GpuMark ("sky");

	// scene geometry
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	for (auto *vo : list)
		if (dynamic_cast<VPlanet*>(vo)) vo->Render (RenderPass::Opaque);
	RenderNearStars (false);
	ctx->GpuMark ("planets");
	for (auto *vo : list)
		if (!dynamic_cast<VPlanet*>(vo)) vo->Render (RenderPass::Opaque);
	ctx->GpuMark ("opaque");
	// (near a black hole: its gas over what lies behind it, a donor star)
	if (lensOn) lens->RenderGas (cmd, (float)nearPlane, (float)lensM);

	// scorch marks on the ground (darkening the lit surface)
	plumes->DrawScorch (camPos, camRot, FrameSet ());

	// volumetric clouds over the opaque scene (a gas giant's rings, seen
	// from within its decks, beyond them)
	if (cloudsActive) {
		for (auto *vo : list)
			if (auto *vp = dynamic_cast<VPlanet*>(vo)) vp->RenderRingBehindDecks ();
		ctx->BindScene (true);
		vclouds->Composite ();
	}

	std::sort (list.begin(), list.end(), [](VObject *a, VObject *b) { return a->CamDist() > b->CamDist(); });
	RenderNearStars (true); // (glare: the farthest)
	for (auto *vo : list) vo->Render (RenderPass::Transparent);

	// heat shimmer behind the exhaust (before the plumes and particles)
	ctx->GpuMark ("transparent");
	if (!relOn) plumes->Shimmer (proj[0], -proj[5], jitter);
	ctx->GpuMark ("shimmer");

	// effects that also write the reactive mask: engine plumes, particles
	vkc::Target rt{ reactive.image, reactive.view, REACTIVE_FORMAT, ctx->RenderExtent () };
	vkc::Target rtMS{ reactiveMS.image, reactiveMS.view, REACTIVE_FORMAT, ctx->RenderExtent () };
	plumes->Render (FrameSet (), camAtmSet, gc->TexMgr()->White()); // (at half resolution)
	ctx->BindSceneWithMask (rt, reactiveMS.image ? &rtMS : nullptr);
	plumes->Draw (FrameSet (), camAtmSet, gc->TexMgr()->White());
	ctx->GpuMark ("plumes");
	// reentry plasma: the glowing shock layers ahead of heated hulls
	if (PlasmaRenderer *pl = Plasma ()) {
		pl->Begin (FrameSet (), oapiGetSimTime ());
		for (auto *vo : list) vo->DrawPlasma (*pl);
		if (pl->Count ()) ctx->GpuMark ("plasma");
	}
	particles->DrawVolume (FrameSet ());
	particles->DrawScreenSmoke (FrameSet ());
	for (auto *ps : gc->ParticleStreams()) ps->Render (this);
	for (auto *vo : list) vo->DrawParticles ();
	particles->DrawRetired (FrameSet ()); // (smoke of vessels whose visuals are gone)
}

VkImageView Scene::ResolveTemporal (VkExtent2D &size)
{
	ssr->StoreColor (ctx->SceneImage ()); // reflected in the next frame
	VkImageView out = ctx->SceneView ();
	size = ctx->RenderExtent ();
	if (taaEnabled) {
		// rotation from the view now to the view then, for directions (sky)
		Mat4 rot = prevView * view.RigidInverse ();
		Mat4 skyMotion = Mat4::FromFloat (prevProj) * rot;
		// discard the history after a camera cut
		bool reset = cameraCut;
		VkImageView up = VK_NULL_HANDLE;
		if (useFsr) {
			double t = std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
			FsrUpscaler::Inputs in{};
			in.color = ctx->SceneImage (), in.colorFormat = ctx->sceneFormat;
			in.depth = ctx->DepthImage ();
			in.velocity = ao->VelocityImage (), in.velocityFormat = AmbientOcclusion::VELOCITY_FORMAT;
			in.reactive = reactive.image;
			in.jitter[0] = jitterPixels[0], in.jitter[1] = jitterPixels[1];
			in.frameTime = lastFrameTime < 0.0 ? 1.0f / 60.0f : (float)vkc::FrameDt (std::min (t - lastFrameTime, 0.5));
			lastFrameTime = t;
			in.nearPlane = (float)nearPlane;
			in.fovY = (float)(2.0 * oapiCameraAperture ());
			in.reset = reset;
			up = fsr->Dispatch (in);
			if (up) {
				out = up;
				size = ctx->Extent ();
			}
		}
		if (!up)
			out = taa->Resolve (ctx->SceneView (), ao->PrepassView (), ao->VelocityView (), reactive.view, skyMotion,
				proj[0], -proj[5], reset);
	}
	// this frame's camera and object poses, for the next frame's motion
	prevCamPos = camPos;
	prevView = view;
	memcpy (prevProj, proj, sizeof(prevProj));
	prevProj[8] = prevProj[9] = 0.0f;
	havePrev = true;
	for (auto &it : visuals) it.second->StorePrev ();
	return out;
}

void Scene::DrawMesh (const VkMesh *mesh, const Mat4 &modelView, RenderPass pass,
	const std::vector<Mat4> *groupTransforms, const std::vector<uint8_t> *groupVisible,
	const std::vector<GroupTexOverride> *texOverride, const std::vector<Mat4> *prevGroupTransforms)
{
	if (!mesh->VertexBuffer() || !mesh->IndexBuffer()) return;
	VkCommandBuffer cmd = ctx->Cmd();
	VkDeviceSize ofs = 0;
	VkBuffer vb = mesh->VertexBuffer();
	// billboards drawn in between use a different layout for set 0
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);

	VkPipeline bound = VK_NULL_HANDLE;
	VkDescriptorSet boundTex = VK_NULL_HANDLE, boundMaps = VK_NULL_HANDLE;
	rtParts.clear ();
	for (uint32_t g = 0; g < mesh->nGroup(); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || (grp.usrFlag & VkMesh::GRP_HIDDEN)) continue;
		if (groupVisible && g < groupVisible->size() && !(*groupVisible)[g]) continue;
		const GroupTexOverride *ovr = nullptr;
		if (texOverride)
			for (auto &o : *texOverride)
				if (o.group == g) { ovr = &o; break; }
		bool transparent = ovr ? ovr->additive : grp.transparent;
		if (transparent != (pass == RenderPass::Transparent)) continue;

		VkPipeline pipe = pass == RenderPass::Depth ? meshPrepass
			: !transparent ? meshOpaque : (ovr || (grp.usrFlag & VkMesh::GRP_ADDITIVE)) ? meshAdditive : meshBlend;
		if (pipe != bound) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			bound = pipe;
		}
		// the group, and the triangles with another texture and material
		// (material configuration SIDE_TEXTURE)
		bool split = grp.sideTex && grp.nSide && !ovr;
		for (int part = 0; part < (split ? 2 : 1); part++) {
			uint32_t mat = part ? grp.sideMaterial : grp.material;
			uint32_t first = part ? grp.firstIndex + grp.nIndex - grp.nSide : grp.firstIndex;
			uint32_t count = part ? grp.nSide : split ? grp.nIndex - grp.nSide : grp.nIndex;
			VkTexture *tex = part ? grp.sideTex : ovr ? ovr->tex : grp.tex ? grp.tex : gc->TexMgr()->White();
			if (tex->set != boundTex) {
				vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &tex->set, 0, nullptr);
				boundTex = tex->set;
			}
			const TexMaps *maps = ovr ? nullptr : tex->maps;
			VkDescriptorSet ms = maps ? maps->set : gc->TexMgr()->DefaultMapSet();
			if (ms != boundMaps) {
				vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 3, 1, &ms, 0, nullptr);
				boundMaps = ms;
			}

			const MATERIAL &m = mesh->Material (mat);
			MeshPushConstants pc;
			Mat4 mv = (groupTransforms && g < groupTransforms->size()) ? modelView * (*groupTransforms)[g] : modelView;
			mv.ToFloat (pc.modelView);
			pc.diffuse[0] = m.diffuse.r, pc.diffuse[1] = m.diffuse.g, pc.diffuse[2] = m.diffuse.b, pc.diffuse[3] = m.diffuse.a;
			pc.emissive[0] = m.emissive.r, pc.emissive[1] = m.emissive.g, pc.emissive[2] = m.emissive.b;
			pc.emissive[3] = (grp.usrFlag & VkMesh::GRP_NOLIGHT) ? 1.0f : 0.0f;
			pc.specular[0] = m.specular.r, pc.specular[1] = m.specular.g, pc.specular[2] = m.specular.b;
			pc.specular[3] = m.power;
			pc.light[0] = pc.light[1] = pc.light[2] = objLight;
			const PBRParams &pp = mesh->PBR (mat);
			pc.pbr[0] = pp.metalness, pc.pbr[1] = pp.roughness, pc.pbr[2] = pp.reflectivity, pc.pbr[3] = pp.f0;
			memcpy (pc.heat, objHeat, sizeof(objHeat));
			pc.maps[0] = maps ? (float)maps->flags : 0.0f;
			pc.maps[1] = transparent ? 1.0f : 0.0f; // not in the prepass: no ambient occlusion
			pc.maps[2] = grp.alphaTest;
			pc.maps[3] = 0.0f;
			if (transparent && !ovr)
				for (int k = 0; k < 3; k++) pc.emissive[k] += objGlassGlow[k];
			if (ovr) { // self-illuminated display
				pc.diffuse[0] = pc.diffuse[1] = pc.diffuse[2] = pc.diffuse[3] = 1.0f;
				pc.emissive[3] = 1.0f;
			}
			if (pass == RenderPass::Depth) { // model-view, motion and material (prepass.vert/frag)
				float pp[40];
				memcpy (pp, pc.modelView, sizeof(pc.modelView));
				memcpy (pp + 16, objMotion, sizeof(objMotion));
				if (groupTransforms && prevGroupTransforms && g < groupTransforms->size() &&
					memcmp ((*groupTransforms)[g].m, (*prevGroupTransforms)[g].m, sizeof(Mat4::m))) {
					// an animated group: view now -> group -> view then (as the
					// object's motion, with the group's last transform)
					Mat4 gm = Mat4::FromFloat (objMotion) * modelView * (*prevGroupTransforms)[g] * mv.AffineInverse ();
					gm.ToFloat (pp + 16);
				}
				memcpy (pp + 32, pc.pbr, sizeof(pc.pbr));
				pp[36] = pc.maps[0];
				pp[37] = pc.emissive[3]; // unlit
				pp[38] = grp.alphaTest;
				pp[39] = 0.0f;
				vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
			} else {
				vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
			}
			vkCmdDrawIndexed (cmd, count, 1, first, grp.vertexOffset, 0);
			if (pass == RenderPass::Depth && rt && rt->Enabled ()) { // into the ray tracing scene
				RTScene::Part part;
				part.geo = { mesh->Id(), mesh->GeomVersion(), mesh->VertexBuffer(),
					(VkDeviceSize)grp.vertexOffset * sizeof(NTVERTEX), mesh->GroupVertexCount (g),
					mesh->IndexBuffer(), first, count };
				part.tex = tex;
				RTScene::Material &rm = part.mat;
				memcpy (rm.diffuse, pc.diffuse, sizeof(rm.diffuse));
				memcpy (rm.emissive, pc.emissive, sizeof(rm.emissive));
				rm.light = objLight;
				memcpy (rm.pbr, pc.pbr, sizeof(rm.pbr));
				rm.unlit = pc.emissive[3] > 0.5f;
				rm.alphaTest = grp.alphaTest > 0.0f;
				rm.owner = objOwner;
				memcpy (rm.vel, objVel, sizeof(rm.vel));
				// static groups: one instance for the mesh (below); animated
				// ones on their own
				if (mesh->rtAnimated.size() != mesh->nGroup()) mesh->rtAnimated.assign (mesh->nGroup(), 0);
				if (groupTransforms && g < groupTransforms->size() && !(*groupTransforms)[g].IsIdentity ())
					mesh->rtAnimated[g] = 1;
				if (mesh->rtAnimated[g]) rt->AddInstance (&part, 1, mv);
				else rtParts.push_back (part);
			}
		}
	}
	if (!rtParts.empty()) rt->AddInstance (rtParts.data(), (uint32_t)rtParts.size(), modelView);
}

void Scene::DrawMeshInstanced (const VkMesh *mesh, VkBuffer inst, VkDeviceSize instOfs, uint32_t count, RenderPass pass,
	const Mat4 *frameToView, bool impostor, float range, const float *tint)
{
	if (!count || !mesh->VertexBuffer() || !mesh->IndexBuffer() || pass == RenderPass::Transparent) return;
	VkCommandBuffer cmd = ctx->Cmd();
	VkBuffer vbs[2] = { mesh->VertexBuffer(), inst };
	VkDeviceSize ofs[2] = { 0, instOfs };
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 2, vbs, ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == RenderPass::Depth ? (impostor ? meshPrepassImp : meshPrepassInst)
		: (impostor ? meshOpaqueImp : meshOpaqueInst));

	// (the instances' transforms are relative to the camera, in the global
	// frame: the view's rotation remains)
	VkDescriptorSet boundTex = VK_NULL_HANDLE, boundMaps = VK_NULL_HANDLE;
	for (uint32_t g = 0; g < mesh->nGroup(); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || grp.transparent || (grp.usrFlag & VkMesh::GRP_HIDDEN)) continue;
		VkTexture *tex = grp.tex ? grp.tex : gc->TexMgr()->White();
		if (tex->set != boundTex) {
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &tex->set, 0, nullptr);
			boundTex = tex->set;
		}
		const TexMaps *maps = tex->maps;
		VkDescriptorSet ms = maps ? maps->set : gc->TexMgr()->DefaultMapSet();
		if (ms != boundMaps) {
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 3, 1, &ms, 0, nullptr);
			boundMaps = ms;
		}
		const MATERIAL &m = mesh->Material (grp.material);
		const PBRParams &pp = mesh->PBR (grp.material);
		MeshPushConstants pc;
		(frameToView ? *frameToView : view).ToFloat (pc.modelView);
		pc.diffuse[0] = m.diffuse.r, pc.diffuse[1] = m.diffuse.g, pc.diffuse[2] = m.diffuse.b, pc.diffuse[3] = m.diffuse.a;
		if (tint) for (int c = 0; c < 3; c++) pc.diffuse[c] *= tint[c];
		pc.emissive[0] = m.emissive.r, pc.emissive[1] = m.emissive.g, pc.emissive[2] = m.emissive.b, pc.emissive[3] = 0.0f;
		pc.specular[0] = m.specular.r, pc.specular[1] = m.specular.g, pc.specular[2] = m.specular.b, pc.specular[3] = m.power;
		pc.light[0] = pc.light[1] = pc.light[2] = objLight;
		pc.pbr[0] = pp.metalness, pc.pbr[1] = pp.roughness, pc.pbr[2] = pp.reflectivity, pc.pbr[3] = pp.f0;
		memset (pc.heat, 0, sizeof(pc.heat));
		pc.maps[0] = maps ? (float)maps->flags : 0.0f;
		pc.maps[1] = 0.0f;
		pc.maps[2] = grp.alphaTest;
		pc.maps[3] = range;
		if (pass == RenderPass::Depth) {
			float pr[40];
			memcpy (pr, pc.modelView, sizeof(pc.modelView));
			memcpy (pr + 16, objMotion, sizeof(objMotion));
			memcpy (pr + 32, pc.pbr, sizeof(pc.pbr));
			pr[36] = pc.maps[0];
			pr[37] = 0.0f;
			pr[38] = grp.alphaTest;
			pr[39] = range;
			vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pr), pr);
		} else {
			vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		}
		vkCmdDrawIndexed (cmd, grp.nIndex, count, grp.firstIndex, grp.vertexOffset, 0);
	}
}

void Scene::DrawBuildings (VkBuffer vb, VkBuffer ib, uint32_t firstIndex, uint32_t nIndex, VkBuffer inst, VkDeviceSize instOfs,
	uint32_t count, RenderPass pass, const Mat4 &frameToView, VkDescriptorSet kit, float hour)
{
	if (!count || !nIndex || pass == RenderPass::Transparent) return;
	VkCommandBuffer cmd = ctx->Cmd();
	VkBuffer vbs[2] = { vb, inst };
	VkDeviceSize ofs[2] = { 0, instOfs };
	VkDescriptorSet fset = FrameSet ();
	VkDescriptorSet ms = gc->TexMgr()->DefaultMapSet();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	// (the prepass reads a plain texture there: prepass.frag)
	VkDescriptorSet tset = pass == RenderPass::Depth ? gc->TexMgr()->White()->set : kit;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &tset, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 3, 1, &ms, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 2, vbs, ofs);
	vkCmdBindIndexBuffer (cmd, ib, 0, VK_INDEX_TYPE_UINT16);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == RenderPass::Depth ? buildPrepass : buildOpaque);
	MeshPushConstants pc{};
	frameToView.ToFloat (pc.modelView);
	pc.diffuse[0] = pc.diffuse[1] = pc.diffuse[2] = pc.diffuse[3] = 1.0f;
	pc.light[0] = pc.light[1] = pc.light[2] = objLight;
	pc.pbr[0] = 0.0f, pc.pbr[1] = 0.8f, pc.pbr[2] = 0.0f, pc.pbr[3] = 1.0f;
	pc.heat[0] = hour; // (building.frag: the windows lit at night)
	if (pass == RenderPass::Depth) {
		float pr[40] = {};
		memcpy (pr, pc.modelView, sizeof(pc.modelView));
		memcpy (pr + 16, objMotion, sizeof(objMotion));
		memcpy (pr + 32, pc.pbr, sizeof(pc.pbr));
		vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pr), pr);
	} else {
		vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	}
	vkCmdDrawIndexed (cmd, nIndex, count, firstIndex, 0, 0);
}

void Scene::AddMeshRT (const VkMesh *mesh, const Mat4 &modelView, bool shadowOnly)
{
	if (!rt || !rt->Enabled () || !mesh->VertexBuffer()) return;
	rtParts.clear ();
	for (uint32_t g = 0; g < mesh->nGroup(); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || grp.transparent || (grp.usrFlag & VkMesh::GRP_HIDDEN)) continue;
		RTScene::Part part;
		part.geo = { mesh->Id(), mesh->GeomVersion(), mesh->VertexBuffer(),
			(VkDeviceSize)grp.vertexOffset * sizeof(NTVERTEX), mesh->GroupVertexCount (g),
			mesh->IndexBuffer(), grp.firstIndex, grp.nIndex };
		part.tex = grp.tex ? grp.tex : gc->TexMgr()->White();
		const MATERIAL &m = mesh->Material (grp.material);
		const PBRParams &pp = mesh->PBR (grp.material);
		RTScene::Material &rm = part.mat;
		rm.diffuse[0] = m.diffuse.r, rm.diffuse[1] = m.diffuse.g, rm.diffuse[2] = m.diffuse.b, rm.diffuse[3] = m.diffuse.a;
		rm.emissive[0] = m.emissive.r, rm.emissive[1] = m.emissive.g, rm.emissive[2] = m.emissive.b;
		rm.light = objLight;
		rm.pbr[0] = pp.metalness, rm.pbr[1] = pp.roughness, rm.pbr[2] = pp.reflectivity, rm.pbr[3] = pp.f0;
		rm.unlit = false;
		rm.alphaTest = grp.alphaTest > 0.0f;
		rm.owner = shadowOnly ? 0 : objOwner;
		memcpy (rm.vel, objVel, sizeof(rm.vel));
		rm.shadowOnly = shadowOnly;
		rtParts.push_back (part);
	}
	if (!rtParts.empty()) rt->AddInstance (rtParts.data(), (uint32_t)rtParts.size(), modelView);
}

void Scene::DrawStar (const Mat4 &modelView, const float rgb[3], double teff, double radius, uint32_t seed, const VECTOR3 *tide, bool roche)
{
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, starPipe);
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	VkDescriptorSet ts = gc->TexMgr()->White()->set;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &ts, 0, nullptr);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &sphereVtx.buffer, &ofs);
	vkCmdBindIndexBuffer (cmd, sphereIdx.buffer, 0, VK_INDEX_TYPE_UINT32);
	MeshPushConstants pc{}; // (star.frag's layout)
	modelView.ToFloat (pc.modelView);
	for (int k = 0; k < 3; k++) pc.diffuse[k] = rgb[k];
	pc.diffuse[3] = 1.0f;
	// activity: cool dwarfs spotted and flaring, the Sun moderately
	double act = teff < 3900.0 ? 0.8 : teff < 5300.0 ? 0.45 : teff < 6500.0 ? 0.3 : 0.1;
	double t = oapiGetSimTime ();
	pc.emissive[0] = (float)act;
	pc.emissive[1] = (float)std::fmod (t, 6e5); // (granules' evolution: ~10 min; repeats after a week)
	pc.emissive[2] = (float)std::clamp (1e6 / std::max (radius, 1.0), 1e-4, 0.02); // (granules ~1000 km)
	pc.emissive[3] = (float)(seed % 997);
	StarFlare fl[2];
	double boost;
	StarFlares (seed, teff, t, fl, boost);
	float *slot[2] = { pc.specular, pc.light };
	for (int k = 0; k < 2; k++) {
		slot[k][0] = (float)fl[k].dir.x, slot[k][1] = (float)fl[k].dir.y, slot[k][2] = (float)fl[k].dir.z;
		slot[k][3] = (float)fl[k].bright;
	}
	pc.pbr[0] = 0.06f; // (a flare's angular radius on the star)
	if (tide) pc.heat[0] = (float)tide->x, pc.heat[1] = (float)tide->y, pc.heat[2] = (float)tide->z, pc.heat[3] = roche ? 2.0f : 1.0f;
	vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed (cmd, sphereNIdx, 1, 0, 0, 0);
}

void Scene::StarFlares (uint32_t seed, double teff, double t, StarFlare out[2], double &boost)
{
	out[0] = out[1] = StarFlare ();
	boost = 1.0;
	if (teff >= 4000.0) return; // (flares of hotter stars are faint against their discs)
	auto hash = [](uint64_t x) {
		x += 0x9e3779b97f4a7c15ull;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
		x ^= x >> 31;
		return (double)(x >> 11) / (double)(1ull << 53);
	};
	// (per hour: a flare starting with probability RATE; brightness falling
	// as a power law, rare large ones; rising over 30 s, fading over
	// minutes to an hour)
	const double RATE = 0.6, HOUR = 3600.0;
	int64_t b0 = (int64_t)std::floor (t / HOUR);
	for (int64_t b = b0 - 3; b <= b0; b++) {
		uint64_t k = ((uint64_t)seed << 40) ^ (uint64_t)b * 0x100000001b3ull;
		if (hash (k) >= RATE) continue;
		double start = ((double)b + hash (k + 1)) * HOUR, dt = t - start;
		if (dt < 0.0) continue;
		double amp = std::min (30.0, 0.4 * std::pow (std::max (hash (k + 2), 1e-3), -1.2));
		double tau = 180.0 + 1500.0 * hash (k + 3);
		double f = dt < 30.0 ? amp * dt / 30.0 : amp * std::exp (-(dt - 30.0) / tau);
		if (f < 1e-3) continue;
		double lat = std::asin ((2.0 * hash (k + 4) - 1.0) * 0.7), lon = 6.283185307 * hash (k + 5);
		StarFlare s{ _V(std::cos (lat) * std::cos (lon), std::sin (lat), std::cos (lat) * std::sin (lon)), f };
		boost += f * 0.003 * 0.06 * 0.06 / 0.0036; // (its share of the disc's light, ~patch area)
		if (s.bright > out[0].bright) out[1] = out[0], out[0] = s;
		else if (s.bright > out[1].bright) out[1] = s;
	}
}

void Scene::DrawSphere (const Mat4 &modelView, const float colour[4], bool unlit, VkTexture *tex)
{
	DrawGeometry (sphereVtx.buffer, sphereIdx.buffer, sphereNIdx, modelView, tex, colour, unlit, false);
}

void Scene::DrawRing (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &modelView, VkTexture *tex,
	const float ring[4], const float legacy[4], bool shadow)
{
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? ringShadowPipe : ringPipe);
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &tex->set, 0, nullptr);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, ib, 0, VK_INDEX_TYPE_UINT32);
	MeshPushConstants pc{}; // (ring.glsl's layout)
	modelView.ToFloat (pc.modelView);
	memcpy (pc.diffuse, ring, 4 * sizeof(float));
	memcpy (pc.emissive, legacy, 4 * sizeof(float));
	vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed (cmd, nIdx, 1, 0, 0, 0);
}

void Scene::DrawGeometry (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &modelView,
	VkTexture *tex, const float colour[4], bool unlit, bool blend)
{
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend ? meshBlend : meshOpaque);
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	VkDescriptorSet ts = (tex ? tex : gc->TexMgr()->White())->set;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &ts, 0, nullptr);
	VkDescriptorSet ms = gc->TexMgr()->DefaultMapSet();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 3, 1, &ms, 0, nullptr);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, ib, 0, VK_INDEX_TYPE_UINT32);
	MeshPushConstants pc{};
	modelView.ToFloat (pc.modelView);
	memcpy (pc.diffuse, colour, 4 * sizeof(float));
	pc.light[0] = pc.light[1] = pc.light[2] = 1.0f;
	pc.pbr[1] = 1.0f; // matte, no reflections
	pc.emissive[3] = unlit ? 1.0f : 0.0f;
	pc.maps[1] = 1.0f; // not in the prepass: no ambient occlusion
	vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed (cmd, nIdx, 1, 0, 0, 0);
}
