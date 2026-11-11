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
#include <cmath>
#include <functional>
#include <vector>

class FireballRenderer {
public:
	static constexpr uint32_t MAX_MARCHED = 4;          ///< fireballs marched at once (the others: sprites)
	static constexpr uint32_t GX = 96, GY = 96, GZ = 112; ///< grid size (x along its path, y across, z up)
	static constexpr double MIN_PIXELS = 12.0;          ///< marched from this screen radius [pixels] up (below: sprites)
	static constexpr double KEEP_PIXELS = 8.0;          ///< one marched stays marched down to this (hysteresis: no flicker at the threshold)
	static constexpr double DRAW_PIXELS = 5.0;          ///< one fading out to sprites is still drawn down to this

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
		VECTOR3 lean{};       // view space: its top displaced per unit height (the momentum it keeps)
		// out of the air (VACUUM_PLAN.md 3.3): no ball - the free cloud, its
		// shape the same at all times in units of its scale [m] (s0 + v t),
		// its peak extinction sigma [1/m] (falling as 1 / scale^3); white-hot
		// as it flashes, its core cooling as it expands (r: the box's; tf:
		// when it has thinned out)
		bool vacuum = false;
		double scale = 1.0, sigma = 0.0;
		double g = 9.81;      // [m/s^2] the gravity it rises in (its rise, its ring's turn: sqrt (g R))
		double blend = 1.0;   // its weight (0..1): in thin air the ball's share of the burst, its opacity and light scaled by it (its extinction would not show it: still opaque at a sixth)
		double ext = 1.0;     // its extinction's scale (thin air: its share of the burst, spread wider - VACUUM_PLAN V5)
		double fade = 1.0;    // its volume's weight (0..1): cross-faded with its sprites as it switches (phase 6)
	};
	/// the model's temperatures at u = t / t_f: the hottest gas (its core)
	/// and its surface (the mixing layer) [K] (fireball_grid.comp)
	static void ModelTemps (double u, double &Th, double &Ts);
	/// the motion of a crash fireball through its life (u = t / t_f), one
	/// model from its burst to its smoke cap, so that each stage flows into
	/// the next (no stage's motion begun or stopped at a set time):
	/// its lifting off (0..1): its buoyancy taking over from its spreading
	/// on the surface, smoothly from its birth, about full by 0.9 t_f
	static double Lift (double u) { return 1.0 - std::exp (-(u / LIFT_U) * (u / LIFT_U)); }
	/// its buoyant rise [m/s]: growing with Lift to some 0.7 sqrt (g R) about
	/// its burn-out, slowing after as u^-1/2 (a thermal drawing air in);
	/// the smoke cap it hands over to rises on as it did (VDebris: the
	/// cap's particles emitted at its velocity, braked at about the rate
	/// it slows by then)
	static double Rise (double u, double R, double g = 9.81) { return RISE_K * std::sqrt (g * R) * Lift (u) / std::sqrt (1.0 + u / RISE_U); } // (g: the body's surface gravity)
	/// its radius [m]: its burst slowed by the air (a third at once, nine
	/// tenths of R by 0.23 t_f, all of it approached smoothly - not grown to
	/// R and stopped), and growing on by a quarter of the height it rose
	/// (the air it draws in) - risen [m]
	static double Radius (double u, double R, double risen) { return R * (1.0 - 0.7 * std::exp (-u / BURST_U)) + ENTRAIN * risen; }
	static constexpr double LIFT_U = 0.5, RISE_K = 0.95, RISE_U = 1.5, BURST_U = 0.1, ENTRAIN = 0.25;
	/// the vortex ring's turn so far [rad] at u = t / t_f (about 1.4 sqrt (g / R)
	/// once lifted - its surface rolling at about its rise speed: the integral
	/// of Lift over u)
	static double RingTurn (double u, double tf, double R, double g = 9.81);

	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
		VkImageView shapeNoise, VkImageView detailNoise);
	void Shutdown ();
	/// the graphics settings (Fireballs: sprites, volumetric, full resolution; live)
	void ApplySettings ();
	/// start collecting this frame's fireballs (before the visuals' CollectFireballs)
	void BeginFrame (double pxPerRad);
	/// add a fireball of this frame (marched if among the MAX_MARCHED largest on screen)
	void Add (const Ball &b);
	/// a fireball of R [m] at dist [m] is to be marched (not sprites): large
	/// enough on screen (with hysteresis), not at a time warp beyond 100x,
	/// and marched in the last frame or room for it
	bool WantsVolume (uint32_t owner, double R, double dist) const;
	/// the prepass (view depth) the march stops at (each frame, first: the
	/// settings applied, the half-size images made when first wanted and
	/// with the swapchain - onOcclusion told of the new OcclusionView)
	void SetPrepass (VkImageView prepass);
	/// told when OcclusionView changes (the sprites' sets)
	std::function<void (VkImageView)> onOcclusion;
	/// the grids (compute; outside rendering scopes, after the frame's uniforms)
	void Compute (VkDescriptorSet frameSet);
	/// the march at half resolution into its own images (outside rendering scopes)
	void Render (VkDescriptorSet frameSet, VkDescriptorSet atmSet);
	/// composite over the scene (scene with reactive mask bound), dimmed by
	/// the smoke volume in front (its integral: fog, its sampler; null: none)
	void Draw (VkDescriptorSet frameSet, VkImageView fog, VkSampler fogSampler);
	/// half-size image: inverse view depth where the fireballs begin (r) and
	/// turn opaque (g), their transmittance (a); valid when Drawn () (a 1x1
	/// "nothing" while no fireball has been marched for two minutes)
	VkImageView OcclusionView () const { return halfAlloc ? halfOcc.view : dummyOcc.view; }
	bool Drawn () const { return drawn; }
	uint32_t Count () const { return (uint32_t)list.size (); }
	/// their share of the view (exposure metering: they count partly)
	double ScreenShare () const { return share; }

	bool enabled = true; ///< the setting (ORBITER_VK_FIREBALL=sprites: off - the sprites)
	bool fullRes = false; ///< marched at the full resolution (the setting's "full resolution")

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
	mutable bool wanted = false;       // (a fireball asked to be marched since the last frame: the images needed)
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
	vkc::Image halfColor, halfMask, halfOcc, halfDepth, dummyOcc;
	VkExtent2D halfExtent{};
	bool halfAlloc = false, halfFull = false;
	uint32_t halfGen = 0, halfSwapGen = ~0u; // (made the halfGen'th time; with the swapchain of halfSwapGen)
	void AllocHalf ();
	void FreeHalf ();
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
