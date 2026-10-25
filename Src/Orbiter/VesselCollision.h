// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Collisions between vessels. Before the vessels are updated, pairs that
// may touch within the step are found (bounding spheres); those vessels
// leave their ground contact to this pass. After the update, the contacts
// between their collision hulls (CollisionHull.h) and with the ground are
// resolved together (ContactSolver.h). Vessels at rest on the ground
// (landed) are fixed obstacles, and become dynamic when hit. Contacts are
// not made close to free docking ports near each other, so that vessels
// can dock.
// =======================================================================

#ifndef __VESSELCOLLISION_H
#define __VESSELCOLLISION_H

#include "Vesselbase.h"
#include <vector>

class PlanetarySystem;

class VesselCollider {
public:
	/// Before the vessels' update: find the pairs that may touch in the step h
	/// (bodies: all vessels and composites; components are left out)
	void Begin (const std::vector<VesselBase*> &bodies, double h);

	/// After the vessels' update: resolve their contacts over the step h
	void Resolve (double h);

private:
	struct Pair { VesselBase *a, *b; };
	std::vector<Pair> pairs;
	std::vector<VesselBase*> dynamic; // bodies of the pairs that move
};

#endif // !__VESSELCOLLISION_H
