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
	double exitRadius;      // [m]
	double length;          // [m]
	double level;           // 0..1
	double ambientPressure; // [Pa]
	PlumeType type;
	float brightness;
	float flicker;          // random brightness factor for this frame
	float sunlight;         // fraction of the sun's light reaching the plume (smoke)
	VkTexture *tint;        // add-on exhaust texture: tints the colours (or null)
	uint32_t seed;
};

class PlumeRenderer {
public:
	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout texLayout,
		VkImageView shapeNoise, VkImageView detailNoise);
	void Shutdown ();
	/// The prepass image (view depth) the plumes are clipped against
	void SetPrepass (VkImageView prepass);
	/// Draw in the scene's rendering scope (transparent pass)
	void Draw (const PlumeDraw &p, VkDescriptorSet frameSet, VkTexture *white, double simt);

	bool enabled = true;
	PlumeConfig config;

private:
	struct Push;
	vkc::Context *ctx = nullptr;
	VkSampler clampSampler = VK_NULL_HANDLE, repeatSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkImageView prepassView = VK_NULL_HANDLE, shapeView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE;
};

#endif // !__PLUMES_H
