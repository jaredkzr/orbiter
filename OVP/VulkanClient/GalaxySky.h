// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The diffuse light of the Milky Way, simulated: a model of the Galaxy's
// stars (Config/CSphere/Sky/galaxy.cfg) seen through its dust (the
// Edenhofer et al. 2024 3D map near the Sun, Config/CSphere/Sky/dust.bin,
// and the model beyond), baked from the observer's position into a cube
// map of surface brightness in four spectral bands. The bake runs again,
// spread over frames, when the observer has moved far enough to change
// the view, and the new cube fades in over the old.
// =======================================================================

#ifndef __GALAXYSKY_H
#define __GALAXYSKY_H

#include "VkContext.h"
#include "GalacticFrame.h"
#include "Nebulae.h"
#include "SkyObjects.h"
#include "Zodiacal.h"
#include "SkySettings.h"
#include <array>
#include <chrono>
#include <string>
#include <vector>

class GalaxySky {
public:
	static constexpr uint32_t SIZE = 1024; // of a cube face
	static constexpr int NBAND = 4;

	/// Load the model and the dust map, bake the sky seen from obs (with
	/// the nebulae around the catalogue's stars). False (and nothing
	/// drawn) without the data files.
	bool Init (vkc::Context *ctx, const galactic::Observer &obs, const galactic::StarCatalogue &cat, const SkySettings &settings);
	void Shutdown ();
	bool Ready () const { return ready; }

	/// Advance the bake (outside rendering scopes): starts a new one when
	/// the observer has moved far enough from the current one's (or the
	/// limit of the stars drawn has changed); each frame's part within
	/// the settings' GPU time budget.
	void Update (const galactic::Observer &obs, double dt);

	/// Display of the diffuse light: realistic (contrast and saturation 1)
	/// or as a long-exposure photograph's: x = L / PIVOT_S10 becomes
	/// y = x^contrast / (1 + x^contrast / highlight), the contrast raised
	/// and bright cores (a nebula's) kept from washing out
	struct Display {
		float gain;              ///< scene radiance of 1 S10
		float contrast = 1.0f;
		float highlight = 0.0f;  ///< (0: none)
		float saturation = 1.0f;
		bool camera = false;     ///< a camera's colours rather than the eye's
		bool zodiacal = true;
	};
	static constexpr float PIVOT_S10 = 300.0f; // (the Milky Way's mean brightness, about)

	/// Draw the sky (in the scene pass, before the stars).
	/// galFromView: rotation view -> Galactic, row-major; fx, fy: the
	/// projection's scale
	void Render (VkCommandBuffer cmd, const double galFromView[3][3], float fx, float fy, const Display &display);

	/// Surface brightness [S10 per band] of the current bake in a Galactic
	/// direction (CPU readback, for tests); writes an equirectangular image
	/// of the whole sky to png if given
	bool Probe (const std::vector<std::array<double, 2>> &lb, std::vector<std::array<float, NBAND>> &out,
		const char *png = nullptr);

	/// limiting magnitude of the stars drawn as points (the rest is
	/// diffuse); a change bakes the sky again
	float StarLimit () const { return starLimit; }
	/// the limit of the stars the sky drawn leaves out (the stars drawn
	/// stop there: until a bake for a changed limit is shown)
	float ShownLimit () const { return cubes[front].limit; }
	void SetStarLimit (float m);

	/// shared with the star field: the model (set 0, binding 0 of
	/// galaxy.glsl), the dust grids and their sampler (binding 1)
	VkBuffer ModelBuffer () const { return model.buffer; }
	VkImageView DustView (int level) const { return dust[level].view; }
	VkSampler DustSampler () const { return dustSampler; }
	static constexpr int DustLevels () { return 3; }
	/// band -> linear RGB (rows), per S10
	const float (*BandColour () const)[NBAND] { return bandColour; }
	/// A(band) / A(V)
	const float *Extinction () const { return extinction; }
	/// the luminosity functions: number density at the Sun per magnitude
	/// (of M_V, in 1-mag bins about M), with the stars' temperature
	struct LFRow { int pop; double M, logn, T; };
	const std::vector<LFRow> &LFRows () const { return lfRows; }

	/// light per band of a black body of temperature T [K] of M_V = 0, in
	/// units of the light of M_V = 0 of the Sun's colour in the band
	void BandLight (double T, float out[NBAND]) const;
	/// bolometric light of a black body of temperature T [K] per its V
	/// light, in units of the Sun's
	double BolometricRatio (double T) const;
	/// the Sun's light in band b per its bolometric light
	double SolarBandFraction (int b) const;
	/// band edges [nm]
	const double (*Bands () const)[2] { return bands; }

	/// the nebulae's parameters (galaxy.cfg, NEB_*)
	struct NebParams {
		double gasPerDust = 716.0;   // ionised gas density per dust [cm^-3 per A_V/pc]
		double minGas = 0.1;         // [cm^-3]
		double maxRadius = 150.0;    // of an ionised region [pc]
		double clumpSigma = 1.5;     // of the log density per unit noise
		double clumpWavelength = 4.0;// largest [pc]
		double albedo = 0.65, g = 0.55;
		double localRadius = 1.0;    // of the dust about a star the map misses (its shell's scale) [pc]
		double lightMV = -1.0;       // stars brighter than this light the dust
		double reachMag = -5.0;      // out to where they look as bright as a star of this magnitude
		// natal cloud of an ionising star, core and envelope: central
		// density [cm^-3], share of its ionising photons each takes
		double coreDensity = 1000.0, coreShare = 0.3, envDensity = 30.0, envShare = 0.4;
	};
	const NebParams &Neb () const { return neb; }
	/// the other galaxies and the globular clusters
	const SkyObjects &Objects () const { return objects; }
	const SkySettings &Settings () const { return settings; }
	const Zodiacal &ZodiacalLight () const { return zodiacal; }

private:
	struct Model;
	bool LoadModel (const std::string &path, std::vector<float> &buf);
	bool LoadDust (const std::string &path);
	void CreateCubes ();
	void CreatePipelines ();
	void BindNebulae (int cube);
	void BakeRows (VkCommandBuffer cmd, int cube, int face, int row0, int nRows, const galactic::Observer &obs, float mLim);
	// the first bake, kept for the next session (ORBITER_VK_SKY_CACHE=0: not)
	static constexpr const char *CACHE = "Cache/VulkanClient/Sky/milkyway.cube";
	uint64_t CacheKey () const;
	bool LoadCache (const galactic::Observer &obs);
	void SaveCache ();
	void ReadBakeTime (); // of the last block
	int BakeBlock (); // rows of the next block, within the time budget
	void UpdateAsync (); // the bake on the async compute queue
	void CubeBarrier (VkCommandBuffer cmd, int cube, VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

	vkc::Context *ctx = nullptr;
	bool ready = false;
	SkySettings settings;
	float starLimit = 8.0f;
	float bandColour[3][NBAND] = {}; // S10 per band -> linear RGB (luminance of a solar spectrum: 1)
	float cameraColour[3][NBAND] = {}; // (a camera's: the enhanced display)
	double bands[NBAND][2] = {};     // [nm]
	float extinction[NBAND] = {};
	std::vector<LFRow> lfRows;
	NebParams neb;
	Nebulae nebulae;
	SkyObjects objects;
	Zodiacal zodiacal;

	vkc::Buffer model;
	vkc::Image dust[3];
	struct Cube {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation alloc = VK_NULL_HANDLE;
		VkImageView cubeView = VK_NULL_HANDLE, arrayView = VK_NULL_HANDLE;
		galactic::Observer obs;
		float limit = 0.0f; // of the stars left out (drawn as points)
	} cubes[2];
	int front = 0;            // cube drawn
	float fade = 0.0f;        // of the back cube over the front one, after a bake
	bool baking = false;      // into the back cube
	bool bakeAgain = false;   // (the star limit changed)
	int bakeFace = 0, bakeRow = 0;
	int fixedRows = 0;        // rows per frame, not by the time budget (test: ORBITER_VK_SKY_ROWS)
	double msPerRow = 0.0;    // GPU time of a row of the last measured block [ms]
	int lastRows = 0;         // of the last block
	VkQueryPool queries = VK_NULL_HANDLE; // timestamps about each frame slot's block
	int slotRows[vkc::FRAMES_IN_FLIGHT] = {};
	int bakeFrames = 0;       // of the bake under way
	double bakeStats[3] = {}; // its measured GPU time: sum, largest [ms], blocks
	std::chrono::steady_clock::time_point bakeStart;
	// the bake on the async compute queue: a block at a time, after the
	// graphics queue's frame bakeAfterFrame (which may have drawn the cube)
	int asyncRows = 16;  // per dispatch (more hold more of the GPU's units, which the frames then wait for)
	int asyncChain = 8;  // dispatches per submission, one after the other
	VkCommandPool asyncPool = VK_NULL_HANDLE;
	VkCommandBuffer asyncCmd = VK_NULL_HANDLE;
	VkSemaphore asyncDone = VK_NULL_HANDLE; // timeline: the blocks done
	uint64_t asyncValue = 0, bakeAfterFrame = 0;
	galactic::Observer bakeObs;
	float bakeLimit = 8.0f;

	VkSampler sampler = VK_NULL_HANDLE, dustSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout bakeLayout = VK_NULL_HANDLE, drawLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet bakeSet[2] = {};   // per target cube
	VkDescriptorSet drawSet[2][2] = {}; // per front cube: [0] the front cube alone, [1] fading to the other
	VkPipelineLayout bakePL = VK_NULL_HANDLE, drawPL = VK_NULL_HANDLE;
	VkPipeline bakePipe = VK_NULL_HANDLE, drawPipe = VK_NULL_HANDLE;
};

#endif // !__GALAXYSKY_H
