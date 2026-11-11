// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Crash fireballs as volumes (out/fx-explosion/PLAN.md section 2): each
// burning ball a small grid of density, temperature and soot, filled every
// frame by a compute pass from a procedural model (an ellipsoid drawn out
// along its path, growing, lifting off and rolling as a vortex ring, its
// billows from noise carried along by that flow), the sunlight through it
// swept into a second grid (self-shadowing), then ray-marched at half
// resolution inside its box up to the opaque scene: blackbody emission by
// temperature, soot absorbing and scattering, and composited over the
// scene (and the smoke volume behind it) by depth, with the reactive mask.
// The sprites drawn after it are hidden behind it by its opacity at their
// depth (OcclusionView). The model is analytic in the fireball's age: the
// same at any frame rate, frozen while paused, right at any time warp.
// =======================================================================

#ifndef __FIREBALL_H
#define __FIREBALL_H

#include "VkContext.h"
#include "VkMath.h"
#include <vector>

class FireballRenderer {
public:
	static constexpr uint32_t MAX_MARCHED = 4;          ///< fireballs marched at once (the others: sprites)
	static constexpr uint32_t GX = 96, GY = 96, GZ = 112; ///< grid size (x along its path, y across, z up)
	static constexpr double MIN_PIXELS = 12.0;          ///< marched from this screen radius [pixels] up (below: sprites)

	/// one fireball of this frame (filled by its visual: VDebris)
	struct Ball {
		uint32_t owner = 0;
		VECTOR3 centre{};     // view space [m]
		VECTOR3 up{}, axis{}; // view space: the local vertical; the direction it was thrown along
		double r = 0.0;       // [m] its radius now
		double R = 0.0;       // [m] grown
		double t = 0.0, tf = 1.0; // [s] its age, its flames' time t_f
		double elong = 1.0;   // drawn out along its path (>= 1)
		double lift = 0.0;    // lifting off (0..1)
		double roll = 0.0;    // [rad] the vortex ring's turn so far
		double ground = -1e9; // [m] the ground (or sea) below its centre, along up (< 0: below; -1e9: far)
		uint32_t seed = 0;
	};

	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
		VkImageView shapeNoise, VkImageView detailNoise);
	void Shutdown ();
	/// start collecting this frame's fireballs (before the visuals' CollectFireballs)
	void BeginFrame (double pxPerRad);
	/// add a fireball of this frame (marched if among the MAX_MARCHED largest on screen)
	void Add (const Ball &b);
	/// a fireball of R [m] at dist [m] is to be marched (not sprites): large
	/// enough on screen, and marched in the last frame or room for it
	bool WantsVolume (uint32_t owner, double R, double dist) const;
	/// the prepass (view depth) the march stops at; the half-size images
	/// made again with the swapchain: true (then OcclusionView changed)
	bool SetPrepass (VkImageView prepass);
	/// the grids (compute; outside rendering scopes, after the frame's uniforms)
	void Compute (VkDescriptorSet frameSet);
	/// the march at half resolution into its own images (outside rendering scopes)
	void Render (VkDescriptorSet frameSet, VkDescriptorSet atmSet);
	/// composite over the scene (scene with reactive mask bound), dimmed by
	/// the smoke volume in front (its integral: fog, its sampler; null: none)
	void Draw (VkDescriptorSet frameSet, VkImageView fog, VkSampler fogSampler);
	/// half-size image: inverse view depth where the fireballs begin (r) and
	/// turn opaque (g), their transmittance (a); valid when Drawn ()
	VkImageView OcclusionView () const { return halfOcc.view; }
	bool Drawn () const { return drawn; }
	uint32_t Count () const { return (uint32_t)list.size (); }
	/// their share of the view (exposure metering: they count partly)
	double ScreenShare () const { return share; }

	bool enabled = true; ///< ORBITER_VK_FIREBALL=sprites: the sprites (off)

private:
	struct Params;
	struct Entry { Ball b; double px, dist; float box[36 * 3]; float rect[4]; };
	vkc::Context *ctx = nullptr;
	std::vector<Entry> list;
	std::vector<uint32_t> marchedLast; // (owners marched in the last frame)
	mutable std::vector<uint32_t> granted; // (owners told to be marched since: WantsVolume)
	double pxPerRad = 1000.0;
	double share = 0.0;
	bool drawn = false, computed = false;
	vkc::Buffer buf;                   // Params per frame slot and fireball
	vkc::Image gridA, gridB, gridC;    // density, temperature, soot; sun and sky optical depth; noise coordinates (a slab per fireball)
	bool gridAlloc = false;
	double gridIdleSince = 0.0;
	void AllocGrids ();
	void FreeGrids ();
	VkSampler linearClamp = VK_NULL_HANDLE, repeatSampler = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE;
	VkImageView shapeView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE, prepassView = VK_NULL_HANDLE;
	// compute: grids written (storage), read
	VkDescriptorSetLayout compSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet compSet = VK_NULL_HANDLE, marchSet = VK_NULL_HANDLE, compoSet[vkc::FRAMES_IN_FLIGHT] = {};
	VkPipelineLayout compLayout = VK_NULL_HANDLE;
	VkPipeline gridPipe = VK_NULL_HANDLE, sunPipe = VK_NULL_HANDLE;
	// march: the grids, the prepass, the noise
	VkDescriptorSetLayout marchSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout marchLayout = VK_NULL_HANDLE;
	VkPipeline marchPipe = VK_NULL_HANDLE;
	vkc::Image halfColor, halfMask, halfOcc, halfDepth;
	VkExtent2D halfExtent{};
	uint32_t halfGen = ~0u;
	void CreateHalf ();
	// composite
	VkDescriptorSetLayout compoSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout compoLayout = VK_NULL_HANDLE;
	VkPipeline compoPipe = VK_NULL_HANDLE;
	VkImageView compoFog[vkc::FRAMES_IN_FLIGHT] = {}, compoPrepass[vkc::FRAMES_IN_FLIGHT] = {};
	uint32_t compoGen[vkc::FRAMES_IN_FLIGHT] = { ~0u, ~0u };
	vkc::Image dummyFog;   // (no smoke volume: transmittance 1)
	void WriteMarchSet ();
};

#endif // !__FIREBALL_H
