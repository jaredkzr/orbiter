// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Stellar systems beyond the Milky Way's model, baked into the sky with
// its light (GalaxySky; Shaders/skyobjects.glsl): the other galaxies of
// the Local Group (Config/CSphere/Sky/galaxies.cfg: small models of disks,
// bulges, bars, star-forming knots and dust, at their distances and
// orientations) and the Milky Way's globular clusters (globulars.csv,
// Utils/SkyKit/build_clusters.py: Plummer spheres of old stars, whose
// brightest stars the star field generates when seen close enough).
// =======================================================================

#ifndef __SKYOBJECTS_H
#define __SKYOBJECTS_H

#include "VkContext.h"
#include <string>
#include <vector>

class GalaxySky;

class SkyObjects {
public:
	/// Load the galaxies and clusters (either may be missing)
	void Init (vkc::Context *ctx, const GalaxySky &sky);
	void Shutdown ();
	VkBuffer Buffer () const { return buffer.buffer; }

	struct Cluster {
		std::string name;
		double pos[3];  ///< centre, heliocentric Galactic [pc]
		double a, rt;   ///< Plummer and tidal radius [pc]
		double volume;  ///< its stars: those of this volume of the Sun's old population [pc^3]
		uint32_t seed;
	};
	const std::vector<Cluster> &Clusters () const { return clusters; }

private:
	bool LoadGalaxies (const std::string &path, const GalaxySky &sky, std::vector<float> &out, int &n);
	bool LoadClusters (const std::string &path, const GalaxySky &sky, std::vector<float> &out, int &n);

	vkc::Context *ctx = nullptr;
	vkc::Buffer buffer;
	std::vector<Cluster> clusters;
};

#endif // !__SKYOBJECTS_H
