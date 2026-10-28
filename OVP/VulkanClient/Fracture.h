// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Fracture walls: where a part broke off a vessel, its skin ends in an
// open edge. A thin wall is put along each such edge, reaching inward
// from it, so that the cut looks like torn structure of some depth
// instead of a paper-thin shell (on the vessel and on the piece). The
// inside of the skin is drawn too (dark), seen through the gap.
//
// The triangles at the fracture can be cut along a torn surface between
// the parts (Field), the same for the vessel and for the pieces broken off
// it: the break does not follow the edges of the mesh's triangles, which
// are long on low-poly meshes (ORBITER_VK_FRACTURE=walls: it does).
// =======================================================================

#ifndef __FRACTURE_H
#define __FRACTURE_H

#include "OrbiterAPI.h"
#include "VkMesh.h"
#include <cstdint>
#include <functional>
#include <vector>

class VESSEL;

namespace fracture {

	/// Whether the triangles are cut along the torn surface (Field):
	/// ORBITER_VK_FRACTURE=cut (default) or walls (the walls along the edges
	/// of the mesh's triangles only)
	bool CutTriangles ();

	/// The torn surface between the parts of a vessel: the part at a point
	/// (VESSEL::GetPartAt) is that at the point displaced by noise. The same
	/// for the vessel and the pieces broken off it (seed: from its name).
	class Field {
	public:
		Field (const VESSEL *v, double size, uint32_t seed);
		int PartAt (const VECTOR3 &p) const;      ///< (vessel frame)
		double Resolution () const { return h; }  ///< [m] the cuts follow the surface to about this
	private:
		const VESSEL *v;
		uint32_t seed;
		double cell, h;
	};

	/// Cuts a triangle (v: its corners; ofs is added to them for side) where
	/// side changes, finer than h [m]: the pieces where side is true are
	/// added to keep, the others to lose (three vertices each, the
	/// triangle's winding). Returns 0 if all of it is on the side (none
	/// added), 1 if none of it (none added), 2 if it was cut.
	int Cut (const NTVERTEX *v, const VECTOR3 &ofs, const std::function<bool (const VECTOR3 &)> &side, double h,
		std::vector<NTVERTEX> &keep, std::vector<NTVERTEX> &lose);

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
	/// open. Corners shared within weld [m] are one; an edge with corners of
	/// other triangles on it (cut finer there) is in pieces between them. depth [m]: the walls'
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

	/// Triangles as mesh parts (16-bit indices: split as needed): walls,
	/// the inside of the skin, or with the texture and material of proto
	void AddParts (const std::vector<NTVERTEX> &vtx, std::vector<VkMesh::Part> &parts, bool interior = false);
	void AddParts (const std::vector<NTVERTEX> &vtx, std::vector<VkMesh::Part> &parts, const VkMesh::Part &proto);

} // namespace fracture

#endif // !__FRACTURE_H
