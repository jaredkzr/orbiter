// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The sky seen near a black hole: its light bent by the hole's spacetime
// (the Kerr metric). Each frame a compute pass traces rays from the camera
// back through the spacetime (Shaders/lens_trace.comp) at the corners of a
// grid over the image (every GRID pixels), to where they come from far
// away and the shift of their light; the sky's pass then draws the Milky
// Way and the extragalactic background from there (Shaders/lenssky.frag):
// the hole's shadow, the Einstein ring, the images of the sky wound about
// the photon orbit, all as seen by the camera moving (its frame:
// aberration, Doppler shift and the gravitational shift together). The
// star field's stars are binned by direction (Shaders/lens_stars.comp) and
// gathered by the pixels whose footprints on the sky hold them: points,
// every image, magnified. A second pass traces every pixel's corners in
// the grid's cells that are not smooth (edges, thin images); the corners
// follow the projection's jitter (temporal anti-aliasing). Translucent gas
// (the disc's fading edge, its lit outer parts) is drawn over the sky from
// an overlay map.
// =======================================================================

#ifndef __BLACKHOLELENS_H
#define __BLACKHOLELENS_H

#include "VkContext.h"
#include "GalaxySky.h"

class StarField;

class BlackHoleLens {
public:
	/// A frame's view of the hole (lengths in its masses, M = 1)
	struct Frame {
		double cam[3] = {};      ///< the camera's position, Kerr coordinates
		double a = 0.0;          ///< spin
		double e[4][4] = {};     ///< the camera's frame: e[0] its 4-velocity, e[1..3] its axes ((t, x, y, z) contravariant)
		double v2k[3][3] = {};   ///< view direction -> components on e[1..3]
		double k2g[3][3] = {};   ///< Kerr axes -> Galactic
		double rh = 2.0, rm = 0.0; ///< outer and inner horizons
		double rph = 3.0;        ///< the prograde circular photon orbit
		bool inside = false;     ///< the camera inside the horizon
		double fx = 1.0, fy = 1.0; ///< the projection's scale
		double betaBg[3] = {};   ///< the hole's velocity relative to the cosmic background (Galactic, units of c)
		bool cosmic = true;      ///< the extragalactic background drawn
		bool stars = true;       ///< the stars drawn (the star field's)
		double discTK = 0.0;     ///< the accretion disc's temperature scale [K] (0: none)
		double discIn = 0.0, discOut = 0.0; ///< its edges (the ISCO, the outer)
		double time = 0.0;       ///< (the disc's turbulence: simulation time in masses)
		/// the stream from a donor star filling its Roche lobe to the disc's
		/// rim, and the hot spot where it strikes it (disc plane, masses):
		/// the donor's azimuth and distance (0: no stream), the hot spot's
		/// radius, the inner Lagrange point's
		double streamPhi = 0.0, streamA = 0.0, streamHot = 0.0, streamL1 = 0.0;
		/// the disc's cool outer parts drawn lit (an artistic liberty): its
		/// rim [M] (0: not drawn) and brightness
		double glowRim = 0.0, glow = 0.0;
		/// the projection's jitter [px] (temporal anti-aliasing)
		float jitter[2] = { 0.0f, 0.0f };
		/// the donor's centre [M] (Kerr), mean radius [M] (0: none), temperature [K]
		double donorPos[3] = {}, donorR = 0.0, donorTeff = 3300.0;
		/// jets along the axis (an artistic liberty): brightness (0: none), length [M]
		double jets = 0.0, jetLength = 250.0;
	};

	bool Init (vkc::Context *ctx, GalaxySky *sky, StarField *stars);
	void Shutdown ();

	/// Trace the rays (outside rendering scopes, in the current command
	/// buffer); the image's size: the scene's render extent
	void Trace (const Frame &f);
	/// Draw the lensed sky (in the scene pass, where the sky is drawn);
	/// sunSurface: the scene radiance of a surface as bright as the Sun's (the
	/// disc's scale)
	void Render (VkCommandBuffer cmd, const GalaxySky::Display &d, float sunSurface);
	/// Draw the translucent gas (the disc's fading edge and lit outer parts,
	/// the stream, the jets) after the opaque scene, at its depth: near the
	/// near plane [m], M the hole's mass [m]
	void RenderGas (VkCommandBuffer cmd, float nearPlane, float M);

	static constexpr double R_IN = 300.0;   ///< [M] rays passing farther: the weak field's bending
	static constexpr double R_OUT = 1000.0; ///< [M] rays leaving beyond (and the camera): the rest of the bending, the weak field's (its second order ~(M/r)^2: 100 M left 0.1 deg)
	static constexpr int MAX_STEPS = 600;   ///< (the few rays wound about the photon orbit longer: black)
	static constexpr double STEP = 0.06;    ///< of r (within 6M; growing as sqrt (r/6M) beyond)
	static constexpr uint32_t STAR_CAP = 1u << 20; ///< stars binned at most
	static constexpr uint32_t STAR_NCELL = 6 * 128 * 128; ///< (lensstars.glsl's LENS_NCELL)

private:
	struct Image {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation alloc = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};
	void CreateImage (Image &im, uint32_t w, uint32_t h, VkFormat fmt);
	void DestroyImage (Image &im);
	void EnsureMap (uint32_t w, uint32_t h);
	void CreatePipelines ();

	void BinStars (VkCommandBuffer cmd);
	void Check (); // ORBITER_VK_LENS_CHECK: the last frame's trace against Kerr.h's, in double precision
	Frame lastFrame;
	int checkFrame = 0;

	vkc::Context *ctx = nullptr;
	GalaxySky *sky = nullptr;
	StarField *starField = nullptr;
	bool stars = false;              // binned this frame
	vkc::Buffer counts, offsets, scratch, records;
	VkDescriptorSetLayout starLayout = VK_NULL_HANDLE;
	VkDescriptorSet starSet = VK_NULL_HANDLE;
	VkPipelineLayout starPL = VK_NULL_HANDLE;
	VkPipeline starPipe = VK_NULL_HANDLE;
	bool ready = false, traced = false;
	int grid = 2;                     // [px] between the grid's corners (ORBITER_VK_LENS_GRID)
	bool refine = true;               // the cells not smooth traced at every pixel (ORBITER_VK_LENS_REFINE)
	uint32_t mapW = 0, mapH = 0, imgW = 0, imgH = 0;
	// the grid's map and overlay, every pixel's (refined), all kept in the general layout
	Image map, overlay, fine, fineOverlay;
	bool mapsNew = true;
	vkc::Buffer params;
	bool cosmic = true;
	double betaBg[3] = {};
	uint32_t frame = 0;

	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout traceLayout = VK_NULL_HANDLE, drawLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet traceSet = VK_NULL_HANDLE, refineSet = VK_NULL_HANDLE, drawSet = VK_NULL_HANDLE;
	VkPipelineLayout tracePL = VK_NULL_HANDLE, drawPL = VK_NULL_HANDLE;
	VkPipeline tracePipe = VK_NULL_HANDLE, drawPipe = VK_NULL_HANDLE, gasPipe = VK_NULL_HANDLE;
	struct DrawParams { float colour[3][4]; float tone[4]; float grid[4]; float bg[4]; float disc[4]; float gas[4]; } drawParams{};
	bool drawn = false; // (the sky drawn this frame: its parameters for the gas)
};

#endif // !__BLACKHOLELENS_H
