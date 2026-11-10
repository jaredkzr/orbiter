// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// What a vessel (or a piece of debris) shows the water, made from its
// collision hulls (CollisionHull.h) in its own frame:
//
//   - cells: the union of the hulls in a grid of cubes (at most MAX_CELLS,
//     the grid as fine as that allows). The hulls of a vessel overlap (a
//     mesh group's parts, groups inside each other): summing their volumes
//     would count the overlaps twice; the cells count them once. They give
//     the displaced volume, the centre of buoyancy and the waterplane.
//   - panels: the hulls' faces where they are the outside of the union
//     (not inside another hull), gathered into at most MAX_PANELS flat
//     patches (area, centre, normal). The water's pressure acts on them:
//     drag, planing lift, the slam of water entry.
//
// Each cell and panel belongs to a structural part (Structure.h), so the
// water's forces load the parts they act on.
// Depends only on ConvexGeometry and Vecmat.
// =======================================================================

#ifndef __HULLWATER_H
#define __HULLWATER_H

#include "ConvexGeometry.h"
#include <functional>
#include <vector>

namespace hydro {

	struct Cell {
		Vector c;      ///< centre of its filled share (body frame)
		double w;      ///< filled share of the cube (0-1: cut by the hulls' faces)
		int part;      ///< structural part (-1: none)
	};

	struct Panel {
		Vector c;      ///< centre (area-weighted)
		Vector n;      ///< outward unit normal (area-weighted mean)
		double area;   ///< [m^2]
		double size;   ///< [m] extent of the patch (for its wetting)
		int part;
	};

	struct Model {
		double h = 0.0;            ///< cell size [m]
		std::vector<Cell> cells;
		std::vector<Panel> panels;
		double volume = 0.0;       ///< [m^3] (cells)
		Vector centroid;           ///< of the volume (body frame)
		double radius = 0.0;       ///< bounding sphere about the body frame's origin [m]
		double length = 0.0;       ///< longest extent [m]
		double area = 0.0;         ///< panels' total area [m^2]
		bool symmetric = false;    ///< made mirror-symmetric about x = 0 (the hulls nearly were)
		bool Empty () const { return cells.empty(); }
	};

	const int MAX_CELLS = 4000;
	const int MAX_PANELS = 400;

	/// The model of a set of hulls (body frame); partAt (may be empty): the
	/// structural part at a point
	Model Build (const std::vector<convex::Polytope> &hulls, const std::function<int(const Vector&)> &partAt);

	/// A sphere of radius r about c (bodies without hulls)
	Model Sphere (const Vector &c, double r);

	/// The submerged volume [m^3] and its centroid (body frame) under the
	/// plane through p0 with upward unit normal up (both body frame); a
	/// cell is submerged by its depth over its size (linear across it)
	double Submerged (const Model &m, const Vector &p0, const Vector &up, Vector *centroid = nullptr);

} // namespace hydro

#endif // !__HULLWATER_H
