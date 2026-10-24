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

enum class PlumeType { Ion, Plasma, Hydrolox, Methalox, Kerolox, Hypergolic, Solid, RCS, Texture, Count };

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
	// the flame as a light source: where (view space), its mean emission
	// (radiance), and the area of its side [m^2]
	VECTOR3 lightPos;
	float lightCol[3];
	double lightArea;
};

class PlumeConfig {
public:
	/// Settings of exhaust idx of the vessel (class configuration, if any)
	PlumeOverride Get (VESSEL *v, unsigned idx);
	static const PlumeClass &Class (PlumeType t);
	static PlumeType Infer (VESSEL *v, void *th);

private:
	struct Rule {
		int group = -1;               // -2: exhaust indices, -1: all, else THGROUP_* (attitude: 3)
		std::vector<int> indices;     // empty: all
		PlumeOverride o;
		bool hasType = false, hasScale = false, hasLength = false, hasBrightness = false;
	};
	const std::vector<Rule> &Rules (const std::string &cls);
	std::map<std::string, std::vector<Rule>> rules;
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

	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout texLayout,
		VkDescriptorSetLayout atmLayout, VkImageView shapeNoise, VkImageView detailNoise);
	void Shutdown ();
	/// Start collecting the plumes of this frame
	void BeginFrame (double simt);
	/// Add a plume of the object owner; returns its index (-1: none), and
	/// where its flame ends (info, if given)
	int Add (const PlumeDraw &p, uint32_t owner, PlumeInfo *info = nullptr);
	/// The prepass image (view depth) the plumes are clipped against
	void SetPrepass (VkImageView prepass);
	/// Draw the plumes, far to near (scene with reactive mask, Context::BindSceneWithMask)
	void Draw (VkDescriptorSet frameSet, VkDescriptorSet atmSet, VkTexture *white);

	/// plume buffer (plume.glsl), noise volumes
	VkBuffer Buffer () const { return buf.buffer; }
	VkDeviceSize BufferSize () const;
	VkImageView ShapeNoise () const { return shapeView; }
	VkImageView DetailNoise () const { return detailView; }
	/// frame slot of this frame's plumes in the buffer
	uint32_t Slot () const { return slot; }
	uint32_t Count () const { return (uint32_t)list.size(); }

	bool enabled = true;
	PlumeConfig config;
	/// nearest surface along a ray (view space, |d| = 1) within maxDist,
	/// other than the object owner's: distance and unit normal
	std::function<bool (uint32_t owner, const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n)> rayCast;

private:
	struct Params;
	struct Entry { uint32_t owner, index; VkTexture *tint; double dist; float box[36 * 3]; };
	vkc::Context *ctx = nullptr;
	vkc::Buffer buf;
	uint32_t slot = 0;
	double simt = 0.0;
	std::vector<Entry> list;
	VkSampler clampSampler = VK_NULL_HANDLE, repeatSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkImageView prepassView = VK_NULL_HANDLE, shapeView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE;
};

#endif // !__PLUMES_H
