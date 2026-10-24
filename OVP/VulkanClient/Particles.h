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

class ParticleSystem {
public:
	static constexpr uint32_t CAPACITY = 1u << 18; ///< particles in the pool
	static constexpr uint32_t BLOCK = 256;         ///< allocation unit

	bool Init (vkc::Context *ctx, TextureManager *texmgr, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
		VkDescriptorSetLayout texLayout, VkImageView shapeNoise, VkImageView detailNoise);
	/// ray traced shadows (layouts of the ray tracing scene and its textures)
	void InitRT (VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex, VkDescriptorSetLayout atmLayout);
	void Shutdown ();

	/// A ring of particles in the pool (first particle, capacity); false if full
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
		float albedo[3];
	};
	void Queue (const Emission &e);
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
	/// rendering of volumetric particles (ORBITER_VK_SMOKE = sprites, volume, screen)
	enum SmokeMode { SMOKE_SPRITES, SMOKE_VOLUME, SMOKE_SCREEN };
	SmokeMode smokeMode = SMOKE_VOLUME;
	/// Draw a stream's particles (scene with reactive mask)
	void Draw (uint32_t first, uint32_t capacity, VkTexture *tex, bool emissive, VkDescriptorSet frameSet);

	/// billowing smoke puffs (four in the quadrants)
	VkTexture *SmokeTexture () const { return smokeTex; }
	/// drag of exhaust gas in air [1/s] at density rho [kg/m^3]: it takes
	/// on the air's velocity within a fraction of a second at sea level
	static double ExhaustDrag (double rho) { return 15.0 * std::sqrt (std::max (rho, 0.0) / 1.225); }

	bool enabled = true;
	float density = 4.0f; ///< particles per specified particle (smaller ones)

private:
	struct GpuEmission;
	struct UpdateParams;
	vkc::Context *ctx = nullptr;
	TextureManager *texmgr = nullptr;
	VkTexture *smokeTex = nullptr;
	vkc::Buffer pool;
	vkc::Buffer emitBuf[vkc::FRAMES_IN_FLIGHT];
	std::vector<Emission> queued;
	std::vector<std::pair<uint32_t, uint32_t>> freeList; // (first, capacity), in blocks of BLOCK
	std::vector<std::pair<uint32_t, uint32_t>> toClear;  // newly allocated rings
	double pendingDt = 0.0;
	float drawBody[12] = {}; // the planet nearby, for drawing: position (camera-relative), velocity, angular velocity
	VECTOR3 lastCam{};
	bool haveCam = false;
	VkSampler noiseSampler = VK_NULL_HANDLE, clampSampler = VK_NULL_HANDLE;
	VkImageView shapeView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE, prepassView = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet sets[vkc::FRAMES_IN_FLIGHT] = {};
	VkPipelineLayout emitLayout = VK_NULL_HANDLE, updateLayout = VK_NULL_HANDLE, updateRTLayout = VK_NULL_HANDLE, drawLayout = VK_NULL_HANDLE;
	VkPipeline emitPipe = VK_NULL_HANDLE, updatePipe = VK_NULL_HANDLE, updateRTPipe = VK_NULL_HANDLE;
	VkPipeline lightPipe = VK_NULL_HANDLE, lightRTPipe = VK_NULL_HANDLE, splatPipe = VK_NULL_HANDLE, sumPipe = VK_NULL_HANDLE;
	vkc::Image shadowAccum, shadowDepth; // smoke shadow grid
	VkPipeline sortKeysPipe = VK_NULL_HANDLE, sortPipe = VK_NULL_HANDLE;
	VkPipelineLayout sortLayout = VK_NULL_HANDLE;
	vkc::Buffer sortVals, sortKeys, blockRingBuf[vkc::FRAMES_IN_FLIGHT];
	std::vector<uint32_t> blockRing; // first block of the ring of each block (free: the block itself)
	vkc::Image volAccum[4], volOut, volColumns; // smoke volume
	VkSampler volSampler = VK_NULL_HANDLE;
	VkPipeline volSplatPipe = VK_NULL_HANDLE, volResolvePipe = VK_NULL_HANDLE, volMarchPipe = VK_NULL_HANDLE;
	VkPipelineLayout volMarchLayout = VK_NULL_HANDLE;
	float volNoise[4] = {};
	VkPipeline drawBlend = VK_NULL_HANDLE, drawAdd = VK_NULL_HANDLE;
	static constexpr uint32_t MAX_EMISSIONS = 4096;
};

class VkParticleStream : public oapi::ParticleStream {
public:
	enum class Kind { Generic, Exhaust, Reentry };

	VkParticleStream (oapi::GraphicsClient *gc, TextureManager *texmgr, PARTICLESTREAMSPEC *pss, Kind kind);
	~VkParticleStream ();

	/// Emit the particles of the simulation step dt [s]
	void Update (double dt);
	/// Draw the particles (scene with reactive mask)
	void Render (Scene *scene);
	/// Detached from its source and no particles left: can be deleted
	bool Expired () const;

private:
	double LevelAlpha (double lvl) const;
	double AtmAlpha (double rho) const;
	/// the ring of particles in the pool (allocated when first needed: the
	/// pool belongs to the scene, which may not exist yet)
	bool EnsureRing ();
	bool SourceState (VECTOR3 &pos, VECTOR3 &dir, VECTOR3 &vel, double &lvl);

	PARTICLESTREAMSPEC spec;
	Kind kind;
	TextureManager *texmgr;
	class VulkanClient *client;
	ParticleSystem *sys = nullptr; // (that of the ring)
	VkTexture *tex = nullptr;
	uint32_t first = 0, capacity = 0, head = 0; // ring in the pool
	bool allocated = false;
	double lastEmit = -1e30, simTime = 0.0;     // [s]
	std::mt19937 rng;
	double emitAccum = 0.0;
	bool havePrev = false;
	VECTOR3 prevSrc{};
	OBJHANDLE hAtmRef = nullptr;  // body whose atmosphere slows the particles
	double reentryLevel = 0.0;
};

#endif // !__PARTICLES_H
