// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Tracks of wheels and imprints of landing gear on bare ground: segments
// recorded in the body frame where a vessel's touchdown points meet the
// terrain, shaded by the tile shader as grooves with raised edges
// (tile_main.glsl's TrackShading), and scorch marks of fires and crashes.
// Per frame, the segments near each drawn
// tile are written in view space into a storage buffer (frame set binding
// 8); the tile's push constants give its range. Not saved with the
// scenario.
// =======================================================================

#ifndef __GROUNDTRACKS_H
#define __GROUNDTRACKS_H

#include "VkContext.h"
#include "OrbiterAPI.h"
#include <deque>
#include <map>
#include <vector>

class GroundTracks {
public:
	/// one segment as the shaders see it (view space)
	struct GpuSeg {
		float a[4];  // start; w: half width [m]
		float b[4];  // end; w: depth [m]
		float up[4]; // surface normal; w: 0 track, 1 imprint, 2 scorch mark
	};
	static constexpr uint32_t MAX_GPU = 4096;  // entries per frame (segments per tile, repeated)
	static constexpr size_t MAX_SEGS = 4096;   // recorded segments (the oldest go first)

	bool Init (vkc::Context *ctx);
	void Shutdown ();
	VkBuffer Buffer (uint32_t frame) const { return buf[frame].buffer; }
	static constexpr VkDeviceSize BUFFER_SIZE = MAX_GPU * sizeof(GpuSeg);

	/// Touchdown point idx of a vessel on the ground at gpos (global), on bare
	/// terrain with normal gnormal; halfWidth, depth of the track [m]
	void Contact (OBJHANDLE vessel, int idx, OBJHANDLE body, const VECTOR3 &gpos, const VECTOR3 &gnormal, double halfWidth, double depth);
	/// ... not on the ground (or on a base)
	void NoContact (OBJHANDLE vessel, int idx);
	/// A scorch mark (a fireball on the ground, a crash): radius, depth of
	/// the crater [m]
	void Scorch (OBJHANDLE body, const VECTOR3 &gpos, const VECTOR3 &gnormal, double radius, double depth);

	/// Start a frame (camera position and rotation, global frame)
	void BeginFrame (uint32_t frame, const VECTOR3 &camPos, const MATRIX3 &camRot);
	/// The segments on a tile of body (position ppos, rotation prot): the
	/// tile's centre direction (planet frame, unit) and angular radius
	/// [rad]; out: first entry and count (as floats, for the push constants)
	void ForTile (OBJHANDLE body, const VECTOR3 &ppos, const MATRIX3 &prot, const VECTOR3 &centreDir, double angRadius, float out[2]);

private:
	struct Seg { OBJHANDLE body; VECTOR3 a, b, n; double hw, depth; int kind; }; // (kind: as GpuSeg::up.w)
	struct Point { OBJHANDLE body = nullptr; VECTOR3 last{}; bool contact = false; };
	vkc::Context *ctx = nullptr;
	std::deque<Seg> segs;
	std::map<std::pair<OBJHANDLE, int>, Point> points;
	vkc::Buffer buf[vkc::FRAMES_IN_FLIGHT];
	uint32_t frame = 0, used = 0;
	VECTOR3 camPos{};
	MATRIX3 camRot{};
	// segments of the body near the camera this frame (indices into segs)
	OBJHANDLE candBody = nullptr;
	std::vector<uint32_t> cand;
	void Add (const Seg &s);
};

#endif // !__GROUNDTRACKS_H
