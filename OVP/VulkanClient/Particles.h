// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Particle streams (exhaust, contrails, reentry plasma), simulated on the
// GPU: each stream owns a ring of particles in a shared pool. The CPU
// decides how many particles a stream emits along its source's path in a
// time step (as add-ons define them, PARTICLESTREAMSPEC); compute passes
// create them and move them (drag towards the air, turbulence), and light
// them (sunlight through the atmosphere, planet and vessel shadows). More,
// smaller particles than specified are emitted (ORBITER_VK_PARTICLE_DENSITY).
// =======================================================================

#ifndef __PARTICLES_H
#define __PARTICLES_H

#include "GraphicsAPI.h"
#include "VkContext.h"
#include "ParticlePool.h"
#include <cmath>
#include <string>
#include <random>
#include <vector>

class VkTexture;
class Scene;
class TextureManager;

/// Billboard vertex: view-space position, texture coordinates, linear colour
/// (flame billboards, beacons)
struct PVtx {
	float x, y, z, u, v;
	float r, g, b, a;
};

/// An emitter's ring of particles in the pool (ParticleSystem::Reserve,
/// Emit, Release): allocated when first needed, grown or shrunk with the
/// emitter's rate (a new ring; the old one is retired, its particles live
/// on), released when idle. Emitters derive their ring records from it.
struct ParticleRing {
	class ParticleSystem *sys = nullptr;        // (that of the ring: the pool belongs to the scene)
	uint32_t first = 0, capacity = 0, head = 0; // in the pool
	uint32_t id = 0;                            // (ParticlePool; 0: none)
	double lastEmit = -1e30, life = 0.0;        // [s]
	double need = 0.0, needT = -1e30;           // particles the ring is to hold (smoothed), when [s]
	bool chain = false;      // a segment of the emissions; when full of live particles, the next (this one retired)
	double segStart = -1e30; // its first emission since it wrapped [s]
	double shortfall = 1.0;  // capacity got / wanted
	bool idleRelease = true; // (Reserve)
	int prio = 1;
	bool failLogged = false;
};

class ParticleSystem {
public:
	static constexpr uint32_t CAPACITY = 1u << 18; ///< particles in the pool
	static constexpr uint32_t BLOCK = 256;         ///< allocation unit

	/// what an emission is (8 bits in the particle record): the look may be
	/// tuned per class, and the pool's degradation follows its priority
	/// (Priority). Auto: from the emission's flags (Emission's bools)
	enum class Class : uint8_t {
		EngineSmoke = 0, Trail, Dust, Spray, Fire, FireballSmoke, Spark, Fragment, Legacy,
		PlasmaWake, Contrail, WreckSmoke, Steam, Puff,
		Auto = 255
	};
	/// 0: kept longest (trails); 1: engine smoke, steam, fire, wakes,
	/// contrails; 2: dust, spray, legacy streams; 3: sparks, fragments, puffs
	/// (thinned first, live shorter, dropped when the pool is full)
	static int Priority (Class c);

	bool Init (vkc::Context *ctx, TextureManager *texmgr, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
		VkDescriptorSetLayout texLayout, VkImageView shapeNoise, VkImageView detailNoise);
	/// ray traced shadows (layouts of the ray tracing scene and its textures)
	void InitRT (VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex, VkDescriptorSetLayout atmLayout);
	void Shutdown ();

	/// A ring of particles in the pool (first particle, capacity; smaller
	/// than wanted if the pool is short, down to a quarter); false if full.
	/// (Emitters: prefer Reserve)
	bool Allocate (uint32_t want, uint32_t &first, uint32_t &capacity);
	void Free (uint32_t first, uint32_t capacity);

	/// particles emitted by a stream in a time step (global frame)
	struct Emission {
		VECTOR3 p0, p1;      // source at the start and the end of the step
		VECTOR3 vel, dir;    // source velocity, direction opposite to the emission
		double v0, spread;   // emission speed, velocity spread [m/s]
		double lifetime, dt;
		double size, growth, alpha, slowdown;
		uint32_t first, capacity, head, count;
		bool emissive, reentry;
		bool tinted;          // smoke of a colour (albedo)
		bool planar;          // the spread is along the surface with the normal -dir
		bool ballistic;       // falls under gravity (dust)
		bool volumetric;      // rendered in the smoke volume (large smoke)
		bool cold;            // tinted, but does not rise (dust)
		bool speck;           // small bits (fragments, sparks): at least about a pixel, under gravity, not streaked; lit ones glint
		bool spherical;       // the spread is the same in all directions (a sphere, not a cube)
		float albedo[3];
		Class cls = Class::Auto;
		bool trail = false;   // a trail's smoke (long-lived, carried by the wind); also slowdown < 0
		/// emissive: a flame cooling over the particle's life: white-yellow,
		/// orange, dark red, its brightness falling as (1 - t)^1.5 (opacity:
		/// faded in only). Times the albedo when tinted (Class::Fire); a
		/// PlasmaWake starts at its albedo and cools to the ramp's colours
		bool ramp = false;
		/// emissive: eroded by noise as it ages (thin parts first: flames
		/// break up into tongues as they cool)
		bool erode = false;
		/// its motion relative to the air drawn as a streak over this time
		/// [s] (up to 0.5; brightness spread over the streak). 0: the
		/// class's (Spark 0.03, PlasmaWake 0.04, others none), < 0: none
		float streak = 0.0f;
	};
	/// Queue an emission (e.first, capacity, head, count set): thinned if the
	/// pool is short (e.count, size, alpha, lifetime changed: advance the
	/// ring's head by e.count after)
	void Queue (Emission &e);
	/// the class of an emission (Class::Auto: from its flags)
	static Class Resolve (const Emission &e);
	/// Make sure the emitter's ring takes rate particles a second (with the
	/// density setting) living life [s] (up to 1.5 of it), plus extra
	/// (bursts): allocated, grown or shrunk as needed; released by the
	/// system when idle (idleRelease) for 1.5 lifetimes + 1 s. tex,
	/// emissive: how its particles are drawn once it is retired (null: the
	/// smoke texture). False: no ring (the pool is full).
	bool Reserve (ParticleRing &r, double rate, double life, Class cls, double extra = 0.0,
		VkTexture *tex = nullptr, bool emissive = false, bool idleRelease = true);
	/// Queue n particles into the ring (thinned if the pool is short: fewer,
	/// larger ones), its head advanced; fills in e's ring fields
	void Emit (ParticleRing &r, Emission &e, uint32_t n);
	/// the emitter is gone: its ring retired (its particles live on) or freed
	void Release (ParticleRing &r);
	/// the ring is the emitter's still (not released while idle)
	bool Valid (const ParticleRing &r) const { return r.sys == this && slots.Valid (r.first, r.id); }
	/// Draw an emitter's ring (scene with reactive mask)
	void Draw (const ParticleRing &r, VkTexture *tex, bool emissive, VkDescriptorSet frameSet)
	{ if (Valid (r)) Draw (r.first, r.capacity, tex, emissive, frameSet); }

	/// a ring whose stream is gone: its particles live on, the ring is
	/// freed after sim time until (tex null: as it was drawn last)
	void Retire (uint32_t first, uint32_t capacity, double until, VkTexture *tex, bool emissive);
	/// Draw the retired rings' particles, and then all sprites (scene with
	/// reactive mask; after the streams' and objects' Draw calls)
	void DrawRetired (VkDescriptorSet frameSet);
	/// simulation time passed (clbkUpdate)
	void AddTime (double dt) { pendingDt += dt; }

	/// Move, emit and light the particles (outside rendering scopes, after
	/// the ray tracing scene was built); rt: sets of the ray tracing scene
	/// and textures (or null)
	/// sunDir: towards the sun (global frame; smoke shadows)
	void Simulate (const VECTOR3 &camPos, const VECTOR3 &sunDir, VkDescriptorSet frameSet, VkDescriptorSet atmSet,
		const VkDescriptorSet *rt, double simt);
	void SetPrepass (VkImageView prepass);
	/// Draw the smoke volume (scene with reactive mask, before the particles)
	void DrawVolume (VkDescriptorSet frameSet);
	/// Screen-space smoke: thickness of the volumetric particles, blurred
	/// (outside rendering scopes, after Simulate); its composite (scene with
	/// reactive mask, before the particles)
	void RenderScreenSmoke (VkDescriptorSet frameSet);
	void DrawScreenSmoke (VkDescriptorSet frameSet);
	void EnsureScreenSmoke ();
	/// the graphics settings' smoke rendering and trails (live: each frame)
	void ApplySettings ();
	/// rendering of volumetric particles (GfxSettings::smokeRendering; ORBITER_VK_SMOKE = sprites, volume, screen)
	enum SmokeMode { SMOKE_SPRITES, SMOKE_VOLUME, SMOKE_SCREEN };
	SmokeMode smokeMode = SMOKE_VOLUME;
	/// Draw a stream's particles (scene with reactive mask): registers how
	/// its ring is drawn this frame; all sprites are drawn together, sorted,
	/// by DrawRetired (the last call)
	void Draw (uint32_t first, uint32_t capacity, VkTexture *tex, bool emissive, VkDescriptorSet frameSet);

	/// billowing smoke puffs (four in the quadrants)
	VkTexture *SmokeTexture () const { return smokeTex; }
	/// drag of exhaust gas in air [1/s] at density rho [kg/m^3]: it takes
	/// on the air's velocity within a fraction of a second at sea level
	static double ExhaustDrag (double rho) { return 15.0 * std::sqrt (std::max (rho, 0.0) / 1.225); }

	bool enabled = true;
	float density = 4.0f; ///< particles per specified particle (smaller ones)
	/// opacity factor of contrails now (Contrail.h): 1 by day, about 0.22 at night (Scene::FillFrameUniforms)
	float contrailNight = 1.0f;
	/// long trails of engine smoke, carried by the wind (GfxSettings::smokeTrails; ORBITER_VK_SMOKE_TRAILS=0: none)
	bool trails = true;

private:
	struct GpuEmission;
	struct UpdateParams;
	float lastUp[64] = {}; // (Simulate's UpdateParams, for the march)
	VkDescriptorSet lastAtm = VK_NULL_HANDLE;
	vkc::Context *ctx = nullptr;
	TextureManager *texmgr = nullptr;
	VkTexture *smokeTex = nullptr;
	vkc::Buffer pool;
	vkc::Buffer emitBuf[vkc::FRAMES_IN_FLIGHT];
	static constexpr int WIND_LAYERS = 64;     // the wind table: a layer each kilometre
	static constexpr double TRAIL_NOISE_SCALE = 2e-4; // trails' eddies [1/m]
	vkc::Buffer windBuf[vkc::FRAMES_IN_FLIGHT];
	ParticlePool slots{ CAPACITY, BLOCK }; // (the rings in the pool)
	/// how each ring (at its first block) was drawn last (or is to be drawn
	/// when retired)
	struct RingLook { VkTexture *tex = nullptr; bool emissive = false; };
	std::vector<RingLook> ringLook = std::vector<RingLook> (CAPACITY / BLOCK);
	uint32_t AllocRing (uint32_t want, uint32_t minWant, uint32_t &first, bool idleRelease);
	/// a new ring for the emitter (capacity cap, at least minCap), the old
	/// one retired: its particles live on
	bool Replace (ParticleRing &r, uint32_t cap, uint32_t minCap, double simt);
	std::vector<Emission> queued;
	std::vector<std::pair<uint32_t, uint32_t>> toClear;  // newly allocated rings
	double lastFailLog = -1e30;                          // [s] wall
	double pendingDt = 0.0;
	// nothing alive: no emissions since the last particle's lifetime ended
	// (the passes are skipped then, after two more updates)
	double aliveUntil = -1e30;
	int idleUpdates = 0;
	bool idle = false;
	float drawBody[12] = {}; // the planet nearby, for drawing: position (camera-relative), velocity, angular velocity
	VECTOR3 lastCam{};
	bool haveCam = false;
	VkSampler noiseSampler = VK_NULL_HANDLE, clampSampler = VK_NULL_HANDLE;
	static constexpr int SMOKE_NOISE_SIZE = 64;   // the smoke volume's erosion noise (texels a side; mips)
	vkc::Image smokeNoise;
	VkSampler smokeNoiseSampler = VK_NULL_HANDLE;
	VkImageView shapeView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE, prepassView = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet sets[vkc::FRAMES_IN_FLIGHT] = {};
	VkPipelineLayout emitLayout = VK_NULL_HANDLE, updateLayout = VK_NULL_HANDLE, updateRTLayout = VK_NULL_HANDLE, drawLayout = VK_NULL_HANDLE;
	VkPipeline emitPipe = VK_NULL_HANDLE, updatePipe = VK_NULL_HANDLE, updateRTPipe = VK_NULL_HANDLE;
	VkPipeline lightPipe = VK_NULL_HANDLE, lightRTPipe = VK_NULL_HANDLE, splatPipe = VK_NULL_HANDLE, sumPipe = VK_NULL_HANDLE;
	vkc::Image shadowAccum, shadowDepth; // smoke shadow grid (three cascades)
	vkc::Buffer shadowOcc;               // its column tiles with smoke (two frames)
	uint32_t shadowParity = 0;
	VkPipeline sortKeysPipe = VK_NULL_HANDLE, sortPipe = VK_NULL_HANDLE, sortLocalPipe = VK_NULL_HANDLE;
	VkPipelineLayout sortLayout = VK_NULL_HANDLE;
	vkc::Buffer sortVals, sortKeys, blockListBuf[vkc::FRAMES_IN_FLIGHT];
	// the sprites: one indirect draw of the sorted list (its length counted
	// on the GPU, read back for the next sort's size), how each ring is
	// drawn (registered by Draw), the rings' textures (a table)
	vkc::Buffer drawArgs[vkc::FRAMES_IN_FLIGHT], drawRead[vkc::FRAMES_IN_FLIGHT], ringInfoBuf[vkc::FRAMES_IN_FLIGHT];
	bool drawValid[vkc::FRAMES_IN_FLIGHT] = {};
	uint32_t sortN[vkc::FRAMES_IN_FLIGHT] = {}, emitHist[vkc::FRAMES_IN_FLIGHT] = {};
	bool drawPending = false;
	SmokeMode lastMode = SMOKE_VOLUME;
	static constexpr uint32_t TEX_SLOTS = 32;
	VkDescriptorSetLayout texSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet texSets[vkc::FRAMES_IN_FLIGHT] = {};
	VkSampler texSampler = VK_NULL_HANDLE;
	VkTexture *slotTex[TEX_SLOTS] = {}, *setTex[vkc::FRAMES_IN_FLIGHT][TEX_SLOTS] = {};
	uint64_t slotFrame[TEX_SLOTS] = {}, frameNo = 0;
	uint32_t TexSlot (VkTexture *tex);
	void DrawSprites (VkDescriptorSet frameSet);
	VkPipelineLayout thickLayout = VK_NULL_HANDLE;
	// smoke volume (allocated while smoke is near in the volume's mode:
	// AllocVolume): accumulation (extinction, coverage, light), the coarse
	// grids for larger particles (each D, C, R, G, B, sun share, core: smokevol.glsl VA_*), the resolved volume, its core, the columns
	// with smoke, the smoke in front of each froxel (sprites fogged), the
	// column tiles with smoke (two frames), the half-resolution march
	vkc::Image volAccum[8], volMid[8], volCoarse[8], lvlL[2], lvlD[2], volOut, volCore, volColumns, volInteg;
	vkc::Image volOccl, occl; // the fine grid's extinction at the half resolution (accumulator, floats): the local lights' occlusion
	vkc::Image volDummy;                 // (the sprites' fog while there is no volume: none)
	vkc::Buffer volOcc;
	vkc::Image march[3];                 // near, far, depths (half resolution)
	VkExtent2D marchExtent{};
	bool volAlloc = false, volFull = true;
	uint32_t volParity = 0, marchGen = ~0u;
	double volIdleSince = -1e30;         // [s] wall: volume unused since (released after a while)
	void AllocVolume (VkCommandBuffer cmd);
	void FreeVolume (bool now);
	void ReapVolume (bool all);
	void AllocMarch (VkCommandBuffer cmd); // (null: submitted and waited for)
	void WriteVolumeSets (uint32_t slot);
	bool volSetDirty[vkc::FRAMES_IN_FLIGHT] = {};
	bool nearKnown = false, nearEmit = false; // (volumetric smoke within reach: read back, emitted)
	struct VolGrave { uint64_t frame; std::vector<vkc::Image> images; vkc::Buffer occ; };
	std::vector<VolGrave> volGrave;      // (released images, destroyed when no frame in flight uses them)
	void RenderMarch (VkDescriptorSet frameSet);
	VkSampler volSampler = VK_NULL_HANDLE;
	VkPipeline volSplatPipe = VK_NULL_HANDLE, volResolvePipe = VK_NULL_HANDLE, volMarchPipe = VK_NULL_HANDLE, marchPipe = VK_NULL_HANDLE;
	VkPipelineLayout volMarchLayout = VK_NULL_HANDLE;
	bool marchReady = false;             // the march ran this frame (DrawVolume composites it)
	float volNoise[4] = {};              // (screen-space smoke's noise)
	vkc::Image thick[2]; // screen-space smoke (half resolution)
	bool volActive = true;                                // the smoke volume this frame
	bool nearValid[vkc::FRAMES_IN_FLIGHT] = {};           // nearVol read back in the frame slot
	uint32_t thickGen = ~0u;
	VkDescriptorSetLayout blurSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet blurSets[2] = {};
	VkPipelineLayout blurLayout = VK_NULL_HANDLE;
	VkPipeline thickPipe = VK_NULL_HANDLE, blurPipe = VK_NULL_HANDLE, screenCompPipe = VK_NULL_HANDLE;
	VkPipeline drawBlend = VK_NULL_HANDLE; // (premultiplied; emissive: alpha 0)
	static constexpr uint32_t MAX_EMISSIONS = 16384;
	// telemetry (ORBITER_VK_PARTICLE_STATS = seconds between log lines; off: 0)
	struct Stats {
		double every = 0.0, next = 0.0;  // [s] wall
		uint32_t frames = 0, simFrames = 0, idleFrames = 0, volFrames = 0, volToggles = 0;
		uint32_t emissions = 0, maxEmissions = 0, dropped = 0, particles = 0, reentry = 0;
		uint32_t allocs = 0, allocFails = 0, failWant = 0, frees = 0;
		uint32_t grows = 0, shrinks = 0, segments = 0, overwrites = 0, idleFrees = 0, thinned = 0, shed = 0, short_ = 0;
		uint32_t lodThinned = 0, staleSrc = 0;
		bool ringLog = false;
		uint32_t live = 0, liveMax = 0, liveVol = 0, drawn = 0, drawnMax = 0, sortMax = 0;
		bool lastVol = false;
	} stats;
	void LogStats (double simt);
	// emitter LOD and the live budget (B8): particles a few pixels across at
	// most over their life are thinned (fewer, larger: Queue); the live
	// count (read back) against a budget thins new emissions
	static constexpr double LOD_PX = 4.0, LOD_MIN = 0.125;
	static constexpr double VACUUM_DUST_SIZE = 0.8; // [m] dust thrown in vacuum at most (Queue)
	static constexpr uint32_t LIVE_BUDGET = 160000;
	uint32_t liveNow = 0;
	double pxPerRad = 1000.0;  // (the render height over the vertical field of view)
	double LiveFactor (int prio) const;
public:
	/// pixels per unit of the tangent of the angle off the view axis (the render height over twice the tangent of the half aperture)
	double PxPerRad () const { return pxPerRad; }
};

#endif // !__PARTICLES_H
