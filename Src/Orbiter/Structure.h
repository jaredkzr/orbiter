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
// parts. Each triangle belongs to the part of the cell of its centre (the
// triangles of a small mesh group, such as a door, all to one part), each
// collision hull (CollisionHull.h) to the part nearest its centre. The parts form a tree, rooted at the core
// part (the one at the vessel's origin, its centre of gravity): each part
// hangs from its parent by a joint, which carries the loads of the part
// and of all the parts beyond it. The empty mass is shared out by the
// parts' surface area (thin shells).
//
// A joint's strength is the largest force it carries in the design load
// cases, times the safety factor (each joint varied a little, so that
// symmetric parts do not fail together): the parts beyond it accelerated
// at the design load factor, and the cases of the vessel's own forces
// (Vessel::Structure: aerodynamic, thrust, landing on the gear).
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
	const double CRUSH_STROKE = 0.3;     ///< [m] rigid impacts decelerate the structure over this distance
	const double CORE_FACTOR = 3.0;      ///< a joint at the core part loaded beyond this times its strength: the vessel breaks up
	const double MAXTEMP = 1800.0;       ///< [K] a part this hot fails (by default)
	const double SMALL_GROUP = 0.3;      ///< mesh groups smaller than this times the vessel's size are not cut (all in one part)

	/// Whether vessels can be damaged: the damage model is on (Orbiter.cfg
	/// DamageModel, Launchpad option) or ORBITER_DAMAGE=1 is set (tests;
	/// does not change the configuration)
	bool DamageEnabled ();

	/// A part of a vessel
	struct Part {
		std::string name;
		std::vector<int> hulls;  ///< indices into the vessel's hulls
		double area = 0.0;       ///< [m^2] surface area of its triangles
		double mass = 0.0;       ///< [kg]
		Vector cg;               ///< [m] its centre of gravity (vessel frame)
		double radius = 0.0;     ///< [m] bounding radius about cg
		Vector proj;             ///< [m^2] projected area along x, y, z (planform: y)
		Vector projc[3];         ///< centres of those projections (vessel frame)
		int parent = -1;         ///< the part it hangs from (-1: the core part)
		double loadFactor = LOADFACTOR; ///< [g] design load factor of the joint to the parent
		double subtreeMass = 0.0;///< [kg] its mass and that of the parts beyond it
		double strength = 0.0;   ///< [N] the joint's strength (0: core part)
		double spread = 1.0;     ///< (its variation)
		bool fixed = false;      ///< (strength given in the class's configuration)
		std::vector<int> children;
	};

	/// Settings of a vessel class (Config/Damage/<class>.cfg)
	struct ClassConfig {
		double loadFactor = LOADFACTOR;
		double safety = SAFETY;
		int nparts = 0;              ///< number of automatic parts (0: from the vessel's size)
		bool indestructible = false;
		double crushPressure = 0.0;  ///< [Pa] external pressure the vessel withstands (0: the planet's GGCrushPressure)
		double maxTemp = MAXTEMP;    ///< [K] temperature at which a part fails
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
		double CrushPressure () const { return crushPressure; } ///< (0: the planet's)
		double MaxTemp () const { return maxTemp; }             ///< [K]

		/// A design load case: forces F on the parts (vessel frame; one per
		/// part) while the vessel accelerates at a (without gravity); extra:
		/// mass at the core part besides the structure (propellant). The
		/// joints are made strong enough to carry it, times the safety factor.
		void DesignCase (const std::vector<Vector> &F, const Vector &a, double extra);

		double LoadFactor () const { return loadFactor; } ///< [g] the class's design load factor
		double Safety () const { return safety; }

		/// The part nearest to p (vessel frame; -1 if none)
		int PartAt (const Vector &p) const;

		/// The part of hull h (-1 if none)
		int PartOfHull (int h) const { return h >= 0 && h < (int)hullPart.size() ? hullPart[h] : -1; }

		/// The part of each triangle of the triangle list it was built from
		const std::vector<int> &TrianglePart () const { return triPart; }

		/// Parts next to a triangle (bit i: part i, for i < 64; its own part
		/// too): a triangle next to a part that broke off is at the fracture
		uint64_t TriangleNeighbours (size_t t) const { return t < triCell.size() ? cellNbr[triCell[t]] : 0; }

	private:
		std::vector<Part> parts; // (parents before children; the core part first)
		std::vector<int> hullPart;
		std::vector<int> triPart;
		std::vector<Vector> cells;  // centres of the cells of the surface (Build)
		std::vector<int> cellPart;  // the part of each
		std::vector<uint64_t> cellNbr; // the parts in and next to each
		std::vector<int> triCell;   // the cell of each triangle
		double cellSize = 0.0;
		bool indestructible = false;
		double crushPressure = 0.0;
		double loadFactor = LOADFACTOR, safety = SAFETY;
		double maxTemp = MAXTEMP;
	};

	/// Signed distance of p from a convex polytope (< 0 inside; outside: a
	/// lower bound, exact along the faces' normals)
	double SignedDistance (const convex::Polytope &h, const Vector &p);

} // namespace structure

#endif // !__STRUCTURE_H
