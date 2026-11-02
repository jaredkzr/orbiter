// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Procedural surfaces of planets without textures or elevation data (the
// exoplanets, oapiProcSurfaceParams): their tiles made on demand.
// - elevation: the core's own (oapiProcElevationTile), so that the meshes
//   are the ground the vessels stand on
// - surface colour from the ground (height, slope) and the climate: the
//   class (rock, ocean, ice, desert, lava), ice beyond the open water about
//   the substellar point of a tidally locked world (or beyond the
//   latitudes of the ice edge), colour noise of its own
// - mask: water (specular) and the glow of lava (night lights)
// - a gas giant's map: belts and zones (its cloud decks' colours)
// =======================================================================

#ifndef __PROCSURFACE_H
#define __PROCSURFACE_H

#include "OrbiterAPI.h"
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

class ProcPlanet {
public:
	/// the planet's procedural surface (null: none)
	static std::shared_ptr<ProcPlanet> Create (OBJHANDLE hPlanet);

	const PROCSURFACEPARAM &Prm () const { return prm; }
	bool Gas () const { return prm.surfClass == 5; }
	/// a mask layer: water (ocean worlds, open water in ice) or lava's glow
	bool HasMask () const { return prm.surfClass == 1 || prm.surfClass == 4 || (prm.surfClass == 2 && prm.openWater > 0.0); }
	/// finest levels with tiles of their own
	int MaxTexLevel () const { return texLevel; }
	int MaxElevLevel () const { return prm.maxLevel; }

	/// tiles of level lvl (4: the hemispheres) as files (thread safe):
	/// uncompressed DDS (256 x 256 RGBA), elevation (.elv)
	void SurfTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds);
	void MaskTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds);
	void ElevTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &elv);
	/// cloud cover (alpha): a tidally locked world's deck over the
	/// substellar point, scattered and streaked clouds beyond; else bands
	void CloudTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds) const;
	int MaxCloudLevel () const { return 9; }

	/// a gas giant's map (longitude -180 at the left, north at the top), RGBA (sRGB)
	void GasMap (int w, int h, std::vector<uint8_t> &rgba) const;

private:
	ProcPlanet () = default;
	/// the core's elevation nodes of a tile [m] (cached), its function no
	/// finer than the finest elevation level
	std::shared_ptr<const std::vector<float>> Elev (int lvl, int ilat, int ilng);
	void Texels (int lvl, int ilat, int ilng, std::vector<uint8_t> &rgba, bool mask);

	OBJHANDLE hPlanet = nullptr;
	PROCSURFACEPARAM prm{};
	double radius = 1.0, res = 1.0;
	int texLevel = 14;
	struct Cached { int lvl, ilat, ilng; std::shared_ptr<const std::vector<float>> h; };
	std::list<Cached> cache;
	std::mutex mtx;
};

#endif // !__PROCSURFACE_H
