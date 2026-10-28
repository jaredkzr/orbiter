// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Fracture walls: where a part broke off a vessel, its skin ends in an
// open edge. A thin wall is put along each such edge, reaching inward
// from it, so that the cut looks like torn structure of some depth
// instead of a paper-thin shell (on the vessel and on the piece). The
// inside of the skin is drawn too (dark), seen through the gap.
// =======================================================================

#ifndef __FRACTURE_H
#define __FRACTURE_H

#include "OrbiterAPI.h"
#include "VkMesh.h"
#include <cstdint>
#include <vector>

namespace fracture {

	/// A triangle of the skin
	struct Tri {
		VECTOR3 p[3];     ///< corners (a common frame for all triangles)
		VECTOR3 n;        ///< outward normal (unit)
		uint32_t src = 0; ///< where it came from (walls are returned by it)
		bool kept = true; ///< still there (false: broken off)
		bool open = false;///< at the fracture: its unshared edges are cut
	};

	/// The walls along the cut edges: edges of kept triangles shared with a
	/// triangle that broke off, or not shared at all on a triangle marked
	/// open. Corners shared within weld [m] are one. depth [m]: the walls'
	/// mean depth (varied along the cut, seeded by seed). out[src]: three
	/// vertices per triangle (both sides), in the triangles' frame.
	void Walls (const std::vector<Tri> &tris, double depth, double weld, uint32_t seed,
		std::vector<std::vector<NTVERTEX>> &out);

	/// The walls' depth [m] for a vessel of size (radius) [m]
	double WallDepth (double vesselSize);

	/// A material for the walls (charred, dark and rough), or for the inside
	/// of the skin
	VkMesh::Part WallPart (bool interior = false);

	/// The inside of a triangle of the skin (seen through the gap where a
	/// part broke off): its back, facing inward, ofs [m] inside it
	void AddInside (const NTVERTEX &a, const NTVERTEX &b, const NTVERTEX &c, double ofs, std::vector<NTVERTEX> &out);

	/// Triangles as mesh parts (16-bit indices: split as needed)
	void AddParts (const std::vector<NTVERTEX> &vtx, std::vector<VkMesh::Part> &parts, bool interior = false);

} // namespace fracture

#endif // !__FRACTURE_H
