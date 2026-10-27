// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Structure of a vessel, for damage: its parts (pieces that can break off)
// and the joints between them.
//
// The parts are made from the triangles of the vessel's external meshes:
// their surface is sampled into the cells of a grid, and the cells are
// clustered into parts of similar size which are connected (cells next to
// each other); vessels symmetric about their x = 0 plane get symmetric
// parts. Each triangle belongs to the part of the cell of its centre, each
// collision hull (CollisionHull.h) to the part nearest its centre. The parts form a tree, rooted at the core
// part (the one at the vessel's origin, its centre of gravity): each part
// hangs from its parent by a joint, which carries the loads of the part
// and of all the parts beyond it. The empty mass is shared out by the
// parts' surface area (thin shells).
//
// A joint's strength is the force it carries when the parts beyond it are
// accelerated at the design load factor, times the safety factor (each
// joint varied a little, so that symmetric parts do not fail together).
//
// Config/Damage/<class>.cfg overrides the defaults (see the README there).
// =======================================================================

#ifndef __STRUCTURE_H
#define __STRUCTURE_H

#include "Vecmat.h"
#include "ConvexGeometry.h"
#include <string>
#include <vector>
#include <cstdint>

namespace structure {

	const double G0 = 9.81;              ///< [m/s^2] standard gravity (load factors)
	const double LOADFACTOR = 6.0;       ///< design load factor [g], by default
	const double SAFETY = 1.5;           ///< safety factor, by default
	const double STRENGTH_SPREAD = 0.1;  ///< joints' strengths vary by up to this fraction

	/// A part of a vessel
	struct Part {
		std::string name;
		std::vector<int> hulls;  ///< indices into the vessel's hulls
		double area = 0.0;       ///< [m^2] surface area of its triangles
		double mass = 0.0;       ///< [kg]
		Vector cg;               ///< [m] its centre of gravity (vessel frame)
		double radius = 0.0;     ///< [m] bounding radius about cg
		int parent = -1;         ///< the part it hangs from (-1: the core part)
		double loadFactor = LOADFACTOR; ///< [g] design load factor of the joint to the parent
		double subtreeMass = 0.0;///< [kg] its mass and that of the parts beyond it
		double strength = 0.0;   ///< [N] the joint's strength (0: core part)
		std::vector<int> children;
	};

	/// Settings of a vessel class (Config/Damage/<class>.cfg)
	struct ClassConfig {
		double loadFactor = LOADFACTOR;
		double safety = SAFETY;
		int nparts = 0;              ///< number of automatic parts (0: from the vessel's size)
		bool indestructible = false;
		struct Named {
			std::string name;
			bool box = false;
			Vector lo, hi;               ///< BOX: hulls whose centre lies in it
			std::vector<std::pair<int,int>> groups; ///< GROUPS mesh:group: hulls mostly made of these
			double loadFactor = 0.0;     ///< (0: the class's)
			double strength = 0.0;       ///< [N] (0: from the load factor)
		};
		std::vector<Named> parts;
		bool found = false;
	};

	/// Reads Config/Damage/<classname>.cfg (defaults if there is none)
	ClassConfig ReadClassConfig (const char *classname);

	/// A triangle of the vessel's external meshes, for building
	struct Triangle {
		Vector a, b, c;     ///< vessel frame (mesh offset applied)
		int mesh, group;
	};

	/// The structure of a vessel
	class Model {
	public:
		/// Built from the vessel's hulls (vessel frame) and the triangles of
		/// its external meshes. emass: the empty mass to share out; size: the
		/// vessel's size (radius) [m]; seed: for the variation of strengths.
		void Build (const std::vector<convex::Polytope> &hulls, const std::vector<Triangle> &tris,
			double emass, double size, const ClassConfig &cfg, uint64_t seed);

		const std::vector<Part> &Parts () const { return parts; }
		bool Indestructible () const { return indestructible; }

		/// The part nearest to p (vessel frame; -1 if none)
		int PartAt (const Vector &p) const;

		/// The part of hull h (-1 if none)
		int PartOfHull (int h) const { return h >= 0 && h < (int)hullPart.size() ? hullPart[h] : -1; }

		/// The part of each triangle of the triangle list it was built from
		const std::vector<int> &TrianglePart () const { return triPart; }

	private:
		std::vector<Part> parts; // (parents before children; the core part first)
		std::vector<int> hullPart;
		std::vector<int> triPart;
		std::vector<Vector> cells;  // centres of the cells of the surface (Build)
		std::vector<int> cellPart;  // the part of each
		double cellSize = 0.0;
		bool indestructible = false;
	};

	/// Signed distance of p from a convex polytope (< 0 inside; outside: a
	/// lower bound, exact along the faces' normals)
	double SignedDistance (const convex::Polytope &h, const Vector &p);

} // namespace structure

#endif // !__STRUCTURE_H
