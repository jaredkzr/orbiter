// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Nebulae of the simulated sky: the gas and dust around the catalogue's
// luminous stars, lit by them, baked into the Milky Way's cube with its
// light (GalaxySky; Shaders/nebula.glsl). Hot stars ionise the gas around
// them (emission nebulae: a front table per star holds the radius of the
// ionised region by direction, computed once from the gas, which follows
// the dust map); the dust around every luminous star scatters its light
// (reflection nebulae). Dark nebulae are the dust map's own.
// =======================================================================

#ifndef __NEBULAE_H
#define __NEBULAE_H

#include "VkContext.h"
#include "GalacticFrame.h"
#include <vector>

namespace galactic { class StarCatalogue; }
class GalaxySky;

class Nebulae {
public:
	/// The sources from the catalogue (none without one), and their
	/// ionisation fronts. sky: its model and dust map loaded.
	void Init (vkc::Context *ctx, const GalaxySky &sky, const galactic::StarCatalogue *cat);
	void Shutdown ();

	/// List the sources seen in each tile of a bake's cube from obs, into
	/// the lists of slot (0, 1: per target cube). True if the slot's buffer
	/// was replaced (its descriptor is to be written again).
	bool Bin (const galactic::Observer &obs, int slot);

	VkBuffer Sources () const { return sources.buffer; }
	VkBuffer Fronts () const { return fronts.buffer; }
	VkBuffer Tiles (int slot) const { return tiles[slot].buffer; }
	size_t Count () const { return pos.size(); }

private:
	vkc::Context *ctx = nullptr;
	vkc::Buffer sources, fronts, tiles[2];
	VkDeviceSize tileCapacity[2] = {}; // [entries]
	uint32_t cubeSize = 0;
	// (for the binning)
	std::vector<float> pos;            // xyz per source [pc]
	std::vector<float> radius;         // of the lit sphere [pc]
	std::vector<uint8_t> ionising;
};

#endif // !__NEBULAE_H
