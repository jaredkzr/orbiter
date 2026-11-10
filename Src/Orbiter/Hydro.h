// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The water's forces on a body (HullWater.h), and its motion through a step
// in the water.
//
// Forces (in a local frame about the body: x east, y up, z north, origin on
// the water's mean surface):
//   - buoyancy: rho g times each cell's submerged share (by its depth under
//     the swell's surface over the cell), at the cell;
//   - pressure: on each wetted panel moving into the water, the normal
//     pressure of a plate in a flow, 1/2 rho V^2 Cn(alpha), Cn = min (pi sin
//     alpha, CN_MAX) (alpha: between the relative velocity and the panel's
//     plane): drag head on, planing lift on a shallow bottom (the skip of a
//     shallow entry);
//   - slamming: a panel being wetted from below (the water entry) feels in
//     addition Wagner's pressure 1/2 rho w^2 pi cot(beta) (w: its downward
//     speed relative to the water; beta: the deadrise between it and the
//     surface), capped for a flat bottom at CS_MAX (the air cushion, the
//     spread over the patch);
//   - skin friction: ITTC-57 on the wetted panels;
//   - wave damping: the waterline's cells damp the heave (and so roll and
//     pitch) at ZETA of critical (the energy radiated as waves).
//
// The motion (Step): water is dense, its forces stiff: a body striking it
// at speed is stopped within a fraction of a frame. The step is therefore
// re-integrated in sub-steps short enough for the forces (the cells' and
// the panels' wetting, the buoyancy's stiffness), with the other forces of
// the step (gravity, thrust, air) as a constant acceleration, and each
// panel's impulse within a sub-step limited to what stops it relative to
// the water (no reversal: implicit in effect).
// Depends only on HullWater, ConvexGeometry and Vecmat.
// =======================================================================

#ifndef __HYDRO_H
#define __HYDRO_H

#include "HullWater.h"
#include <vector>

namespace hydro {

	const double CN_MAX = 1.0;
	const double CS_MAX = 3.0;
	const double ZETA = 0.15;
	const int MAX_SUBSTEPS = 64;
	extern int debugPanels; ///< (diagnostics: print this many strongest panels of the next Forces calls)

	/// The water about the body: a grid of the swell's surface (height above
	/// the mean level; the water's velocity at the surface), local frame
	struct Surface {
		double rho = 1025.0, nu = 1.19e-6, g = 9.81;
		double kp = 0.1;          ///< [1/m] the waves' typical wavenumber (the velocity's decay with depth)
		int n = 0;                ///< nodes per side (0: flat and still)
		double x0 = 0.0, z0 = 0.0, ds = 1.0; ///< grid origin (south-west node) and spacing [m]
		std::vector<double> h;    ///< n x n heights (row z, column x)
		std::vector<Vector> vel;  ///< n x n surface velocities
		double t0 = 0.0, dhdt = 0.0; ///< (unused: the grid is for the step's end)

		/// height and slope (dh/dx, dh/dz) at (x, z)
		double Height (double x, double z, double *sx = nullptr, double *sz = nullptr) const;
		/// the water's velocity at (x, z), depth d below the surface (>= 0)
		Vector Vel (double x, double z, double d) const;
	};

	/// A body in the local frame
	struct Body {
		double mass = 1.0;
		Vector pmi;       ///< principal moments of inertia per unit mass (body frame)
		Vector cg;        ///< centre of gravity (body frame; the model's origin is the body frame's)
		Vector x;         ///< the body frame's origin (local frame)
		Vector v;         ///< velocity of the centre of gravity (local frame, relative to the water's frame)
		Matrix R;         ///< body frame -> local frame
		Vector w;         ///< angular velocity (body frame)
	};

	/// What the water did over a step (or at an instant)
	struct Result {
		Vector F, T;               ///< mean force (local frame) and torque about the CG (body frame)
		double submerged = 0.0;    ///< [m^3] at the end
		double wetted = 0.0;       ///< [m^2] at the end
		Vector wetC;               ///< centre of the wetted panels (local frame), at the end
		double waterline = 0.0;    ///< [m^2] the waterplane's area at the end
		double slamMax = 0.0;      ///< [Pa] the highest slam pressure (on a panel)
		double pressureMax = 0.0;  ///< [Pa] the highest pressure (on a panel)
		double entrySpeed = 0.0;   ///< [m/s] the fastest downward speed of a panel being wetted
		double accMax = 0.0;       ///< [m/s^2] the water's largest acceleration of the CG within the step
		int substeps = 0;
		std::vector<Vector> partF; ///< mean force on each structural part (body frame), if parts > 0
		void Clear (int parts);
	};

	/// The water's force and torque on the body at its state (no step)
	void Forces (const Model &m, const Surface &s, const Body &b, Result &r, double dt = 0.0);

	/// The body over a step h: from its state at the start, with the other
	/// forces' acceleration aExt (local frame) and angular acceleration
	/// alphaExt (body frame), and the water's forces; b is replaced by the
	/// state at the end. r: the water's mean forces over the step
	void Step (const Model &m, const Surface &s, Body &b, const Vector &aExt, const Vector &alphaExt, double h, int parts, Result &r);

	/// Floating at rest on a flat, still sea: the body's origin y [m] above the
	/// surface, its pitch (about its x axis) and roll (about its z axis) [rad],
	/// body -> local R = Rx (pitch) Rz (roll) (heading 0), such that it
	/// displaces its mass and its centre of buoyancy lies under its centre of
	/// gravity. False if it does not float (heavier than its displacement).
	bool Equilibrium (const Model &m, double mass, const Vector &cg, double rho, double &y, double &pitch, double &roll);
	Matrix TrimMatrix (double pitch, double roll);

	/// Sub-steps the step needs (the buoyancy's stiffness, the speed over the cells)
	int Substeps (const Model &m, const Surface &s, const Body &b, double h, const Vector &aExt = Vector (0, 0, 0));

} // namespace hydro

#endif // !__HYDRO_H
