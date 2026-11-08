// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Engine plumes: a volume per exhaust, ray-marched inside a bounding box.
// The plume's shape follows the ratio of the nozzle's exit pressure to the
// ambient pressure: a narrow jet with shock diamonds at sea level, a wide
// faint bell in vacuum. Its colours and brightness come from the engine
// class (propellant), inferred from the thruster's specific impulse and
// thrust, or set per vessel class in Config/VK/Plumes/<class>.cfg.
// =======================================================================

#ifndef __PLUMES_H
#define __PLUMES_H

#include "VkContext.h"
#include "VkMath.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

class VESSEL;
class VkTexture;

enum class PlumeType { Ion, Plasma, Hydrolox, Methalox, Kerolox, Hypergolic, Solid, RCS, ColdGas, Texture, Count };

/// Per-exhaust settings from the vessel's plume configuration
struct PlumeOverride {
	int type = -1;          // PlumeType, -1: inferred
	float scale = 1.0f;     // nozzle size factor
	float length = 1.0f;    // plume length factor
	float brightness = 1.0f;
};

/// Physical and visual parameters of an engine class
struct PlumeClass {
	const char *name;
	double exitPressure;    // [Pa]
	float core[3];          // emission of the jet core (linear, at the nozzle)
	float coreBrightness;
	float diamond[3];       // shock diamonds (Mach disks)
	float diamondBrightness;
	float outer[3];         // afterburning in the ambient air
	float outerBrightness;
	float smoke;            // extinction of the exhaust products downstream
	float vacuumBrightness; // brightness factor in vacuum
	float turbulence;
	float flow;             // turbulence drift [exit radii/s]
	// smoke (particles) where the flame ends, in air
	float smokeAlbedo[3];   // colour
	float smokeOpacity;     // opacity of a particle (at the specified density)
	float smokeLife;        // [s]
	float smokeAir0, smokeAir1; // fades in between these pressures [atm]
	float trail;            // strength of the long trail left in the air (0: none; solid motors 1)
	float contrail;         // water in the exhaust condensing in cold air (Contrail.h; 0: none)
	// the flame as a light source: its radiance (1 ~ a sunlit white surface;
	// 0: as drawn). Brightness 1 ~ sunlit white is right for drawing the
	// flame; a real flame's visible radiance is far higher (a few hundred
	// times for the 2500-3000 K alumina of a solid motor), which lights the
	// ground and smoke at night for kilometres (VVessel::CollectLights). The
	// drawn emission scaled to it at full thrust in sea-level air (throttle,
	// flicker, vacuum and BRIGHTNESS kept)
	float lightRadiance;
};

/// where a plume's flame ends and what it meets (PlumeRenderer::Add)
struct PlumeInfo {
	VECTOR3 end;            // end of the flame, view space [m]
	VECTOR3 endDir;         // flow direction there (view space)
	double endRadius;       // jet radius there [m]
	bool hit;               // a surface in the way
	VECTOR3 hitPos, hitN;   // where (view space), its normal (towards the jet)
	double coverage;        // fraction of the jet blocked
	double heat;            // heat of the jet at the surface (0..1)
	// the flame as a light source: an extended one, three points along it
	// (0.2, 0.5, 0.9 of its length; view space) sharing the area of its side
	// [m^2], the jet's radius at each [m]; its mean emission (radiance)
	VECTOR3 lightPos[3];
	double lightRadius[3];
	float lightCol[3];
	double lightArea;
};

class PlumeConfig {
public:
	/// Settings of exhaust idx of the vessel (class configuration, if any)
	PlumeOverride Get (VESSEL *v, unsigned idx);
	static const PlumeClass &Class (PlumeType t);
	static PlumeType Infer (VESSEL *v, void *th);
	/// the class keeps its add-on reentry particle streams (and no VK
	/// plasma sheath): ReentryLegacy = TRUE in its configuration
	bool ReentryLegacy (VESSEL *v);

private:
	struct Rule {
		int group = -1;               // -2: exhaust indices, -1: all, else THGROUP_* (attitude: 3)
		std::vector<int> indices;     // empty: all
		PlumeOverride o;
		bool hasType = false, hasScale = false, hasLength = false, hasBrightness = false;
	};
	const std::vector<Rule> &Rules (const std::string &cls);
	std::map<std::string, std::vector<Rule>> rules;
	std::map<std::string, bool> reentryLegacy; // (filled with the rules)
};

/// One plume to draw (filled by the vessel visual)
struct PlumeDraw {
	VECTOR3 pos;            // nozzle exit, view space [m]
	VECTOR3 dir;            // flow direction, view space
	VECTOR3 wind;           // velocity of the air relative to the nozzle, view space [m/s]
	double exitRadius;      // [m]
	double length;          // [m]
	double level;           // 0..1
	double thrust;          // at full level [N] (0: unknown)
	double ambientPressure; // [Pa]
	double airDensity;      // [kg/m^3]
	PlumeType type;
	float brightness;
	float flicker;          // random brightness factor for this frame
	float sunlight;         // fraction of the sun's light reaching the plume (smoke)
	VkTexture *tint;        // add-on exhaust texture: tints the colours (or null)
	uint32_t seed;
};

/// The plumes of a frame: collected before the prepass (screen-space
/// reflections march through them too), drawn in the transparent pass
class PlumeRenderer {
public:
	static constexpr uint32_t MAX_PLUMES = 64; ///< per frame (plume.glsl)
	static constexpr VkFormat HALF_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT; ///< the half-size image of the plumes' light

	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout texLayout,
		VkDescriptorSetLayout atmLayout, VkImageView shapeNoise, VkImageView detailNoise);
	void Shutdown ();
	/// Start collecting the plumes of this frame
	void BeginFrame (double simt);
	/// Add a plume of the object owner; returns its index (-1: none), and
	/// where its flame ends (info, if given)
	int Add (const PlumeDraw &p, uint32_t owner, PlumeInfo *info = nullptr);
	/// the solid angle of the flames added this frame [sr] (their bright
	/// parts, unoccluded)
	double SolidAngle () const { return solidAngle; }
	/// The prepass image (view depth) the plumes are clipped against
	void SetPrepass (VkImageView prepass);
	/// At half resolution (halfRes): march the plumes into half-size images
	/// (outside the scene's rendering scope, before Draw)
	void Render (VkDescriptorSet frameSet, VkDescriptorSet atmSet, VkTexture *white);
	/// Draw the plumes, far to near, or composite those of Render (scene
	/// with reactive mask, Context::BindSceneWithMask)
	void Draw (VkDescriptorSet frameSet, VkDescriptorSet atmSet, VkTexture *white);

	/// Heat shimmer: distort the scene drawn so far behind the hot exhaust
	/// (before the plumes are drawn; outside rendering scopes or in the
	/// scene's). fx, fy: projection factors; jitter: projection jitter (NDC)
	void Shimmer (float fx, float fy, const float jitter[2]);

	/// Scorch marks where hot jets meet the ground: add heat to the mark at a
	/// global position (merged with a mark nearby), and draw them (scene's
	/// rendering scope, after the opaque scene; camRot: camera axes)
	void Scorch (OBJHANDLE body, const VECTOR3 &gpos, const VECTOR3 &gnormal, double radius, double amount);
	void DrawScorch (const VECTOR3 &camPos, const MATRIX3 &camRot, VkDescriptorSet frameSet);

	/// plume buffer (plume.glsl), noise volumes
	VkBuffer Buffer () const { return buf.buffer; }
	VkDeviceSize BufferSize () const;
	VkImageView ShapeNoise () const { return shapeView; }
	VkImageView DetailNoise () const { return detailView; }
	/// frame slot of this frame's plumes in the buffer
	uint32_t Slot () const { return slot; }
	uint32_t Count () const { return (uint32_t)list.size(); }

	bool enabled = true;
	/// marched at half resolution, composited by depth (ORBITER_VK_PLUME_FULLRES=1: full)
	bool halfRes = true;
	bool shimmer = true;          ///< heat shimmer (ORBITER_VK_SHIMMER)
	float shimmerStrength = 3.0f; ///< displacement [pixels] at unit heat
	PlumeConfig config;
	/// nearest surface along a ray (view space, |d| = 1) within maxDist,
	/// other than the object owner's: distance, unit normal, and what was met
	/// (kind, if given: 0 terrain, 1 a surface base, 2 a vessel)
	std::function<bool (uint32_t owner, const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n, int *kind)> rayCast;

private:
	struct Params;
	struct Entry { uint32_t owner, index; VkTexture *tint; double dist; float box[36 * 3]; };
	vkc::Context *ctx = nullptr;
	vkc::Buffer buf;
	uint32_t slot = 0;
	double solidAngle = 0.0;
	double simt = 0.0;
	std::vector<Entry> list;
	VkSampler clampSampler = VK_NULL_HANDLE, repeatSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	// half resolution: the plumes' light and transmittance, their reactive
	// mask (and a depth buffer the pass needs); the composite's set and pipeline
	VkPipeline halfPipe = VK_NULL_HANDLE;
	vkc::Image halfColor, halfMask, halfDepth;
	uint32_t halfGen = ~0u;
	VkExtent2D halfExtent{};
	bool halfDrawn = false;
	VkDescriptorSetLayout compSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool compPool = VK_NULL_HANDLE;
	VkDescriptorSet compSet = VK_NULL_HANDLE;
	VkPipelineLayout compLayout = VK_NULL_HANDLE;
	VkPipeline compPipe = VK_NULL_HANDLE;
	void CreateHalf ();
	void DrawList (VkDescriptorSet frameSet, VkDescriptorSet atmSet, VkTexture *white, VkPipeline p, uint32_t scale);
	VkImageView prepassView = VK_NULL_HANDLE, shapeView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE;
	// heat shimmer: a copy of the scene, its set and pipeline
	vkc::Image shimmerSrc;
	uint32_t shimmerGen = ~0u;
	VkDescriptorSetLayout shimmerSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool shimmerPool = VK_NULL_HANDLE;
	VkDescriptorSet shimmerSet = VK_NULL_HANDLE;
	VkPipelineLayout shimmerLayout = VK_NULL_HANDLE;
	VkPipeline shimmerPipe = VK_NULL_HANDLE;
	VkSampler linearClamp = VK_NULL_HANDLE;
	void CreateShimmerSource ();
	struct Mark { OBJHANDLE body; VECTOR3 pos, normal; double radius, dark; uint32_t seed; }; // (body frame)
	std::vector<Mark> marks;
	VkPipelineLayout scorchLayout = VK_NULL_HANDLE;
	VkPipeline scorchPipe = VK_NULL_HANDLE;
};

#endif // !__PLUMES_H
