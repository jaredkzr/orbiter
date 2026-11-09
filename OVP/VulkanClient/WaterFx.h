// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Disturbances of a planet's water drawn by the tile shader (tile_main.glsl,
// WaterFxEval): jets blowing on it, gear standing in it, splashes.
// Producers feed sources (each frame, under a key) or fire impulses;
// Atmosphere::Update packs the nearest into its uniform block.
// =======================================================================

#ifndef __WATERFX_H
#define __WATERFX_H

#include "OrbiterAPI.h"
#include <cstdint>
#include <vector>

class WaterFx {
public:
	static constexpr int MAX_GPU = 8;       // as WATERFX_MAX (atmosphere.glsl)
	static constexpr int VEC4 = 6;          // as WATERFX_VEC4
	static constexpr int MAX_SOURCES = 48;
	enum class Kind : uint8_t { Jet = 0, Contact, Splash };

	WaterFx ();                             // reads ORBITER_VK_WATERFX, ORBITER_VK_WATERFX_LOG

	/// a source fed this frame. key: the producer's (the same key: the same source follows it);
	/// gpos: global position on the water; R0: footprint radius [m]; J: strength at the water 0..1;
	/// Fs: force reaching the water [N] (0: none: Rw = 2 R0, no cavity)
	void Feed (OBJHANDLE body, uint64_t key, const VECTOR3 &gpos, double R0, double J, double Fs, Kind kind);

	/// a splash: foam at once, a 0.6 s pulse of rings (strength 0..1)
	void Impulse (OBJHANDLE body, const VECTOR3 &gpos, double R0, double strength);

	/// once per frame before the producers (Scene::BeginFrame): ageing, release of the sources not
	/// fed since the last call, retirement of spent ones, the log
	void Update ();

	/// the sources of a body near the camera (camPlanet: camera, planet frame) for the uniform
	/// block: hdr = (count, 0, 0, 0), src[MAX_GPU * VEC4][4] (DESIGN 2.4). Returns count
	int Pack (OBJHANDLE body, const VECTOR3 &camPlanet, float hdr[4], float src[][4]) const;

	bool Enabled () const { return enabled; }

private:
	void MakeRoom (double now); // retire one source when the list is full: the spent first, else the oldest
	struct Source {
		OBJHANDLE body = nullptr; uint64_t key = 0; Kind kind = Kind::Jet;
		VECTOR3 c{}, a{}, t1{};   // centre, anchor (fixed), tangent at the anchor: planet frame
		double R0 = 1, Rr = 1, Rw = 2, g = 9.81; // Rr: ring source radius, grows while fed
		double Jfed = 0, J = 0, depth = 0, foam = 0, foamTarget = 0, ringA = 0, ringGain = 0.3;
		double t0 = 0, tRel = -1; // sim time born; released (< 0: fed; may lie ahead: Impulse's pulse)
		double psi[3] = {0,0,0}; // ring phases, integrated
		double run = 0, inner = 0; // front beyond R0, inner edge: integrated
		bool fed = false;
	};
	std::vector<Source> list;
	double lastT = -1.0, logT = -1e9;
	bool enabled = true, log = false, first = true;
};

#endif // !__WATERFX_H
