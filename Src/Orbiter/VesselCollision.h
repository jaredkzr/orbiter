// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Collisions between vessels, and with the structures of surface bases.
// Before the vessels are updated, pairs that may touch within the step are
// found (bounding spheres); those vessels leave their ground contact to
// this pass. After the update, the contacts
// between their collision hulls (CollisionHull.h) and with the ground are
// resolved together (ContactSolver.h). Vessels at rest on the ground
// (landed) are fixed obstacles, and become dynamic when hit; base
// structures (Base::CollisionHulls) are fixed. A vessel that starts inside
// a structure (placed there) passes through that part until it is clear. Contacts are
// not made close to free docking ports near each other, so that vessels
// can dock.
// =======================================================================

#ifndef __VESSELCOLLISION_H
#define __VESSELCOLLISION_H

#include "Vesselbase.h"
#include <set>
#include <vector>

class PlanetarySystem;
class Base;

class VesselCollider {
public:
	/// Before the vessels' update: find the pairs that may touch in the step h
	/// (bodies: all vessels and composites; components are left out)
	void Begin (const std::vector<VesselBase*> &bodies, double h);

	/// After the vessels' update: resolve their contacts over the step h
	void Resolve (double h);

	/// A payload was released from a vessel (the bodies they are part of):
	/// they do not collide with each other until they are apart (a payload
	/// nested in its mounting, whose convex parts may enclose it). A piece
	/// broken off (piece true) that has not come apart from its vessel once
	/// they have settled (a vessel resting on a piece under it) is pushed
	/// out of it, slowly.
	void Released (VesselBase *parent, VesselBase *payload, bool piece = false);

	/// A vessel is deleted
	void Forget (VesselBase *v);

private:
	struct Pair { VesselBase *a, *b; };
	std::vector<Pair> pairs;
	struct BasePair { VesselBase *v; const Base *base; };
	std::vector<BasePair> basePairs;
	// (vessel, structure part) overlapping when first met: passed through
	// until apart
	std::set<std::pair<const VesselBase*, const void*>> inside;
	std::vector<VesselBase*> dynamic; // bodies of the pairs that move
	struct Release {
		VesselBase *a, *b;
		bool clear = false; // apart: collide from now on
		bool piece = false; // (a piece broken off its vessel)
		double t = 0.0;     // (simulation time released)
		bool settled = false; // (a piece: settled against its vessel, pushed out of it)
	};
	std::vector<Release> releases;
};

#endif // !__VESSELCOLLISION_H
