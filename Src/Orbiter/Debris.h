// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Debris: pieces broken off vessels (Structure.h). Rigid bodies of their
// own, not vessels: they fall under gravity and atmospheric drag, collide
// with the ground (the vertices of their hulls), with vessels, base
// structures and each other, and come to rest on the ground. They are kept
// in their own list of the planetary system (not among the vessels).
//
// A piece keeps the triangles it was made of (frame: its centre of
// gravity, axes those of the vessel when it broke off), for the graphics
// client, and a convex hull for each structural part in it.
// =======================================================================

#ifndef __DEBRIS_H
#define __DEBRIS_H

#include "Vesselbase.h"
#include <string>
#include <vector>

class Mesh;

class Debris: public VesselBase {
public:
	/// The triangles of one group of a mesh of the vessel (debris frame)
	struct Group {
		const Mesh *mesh;          ///< the vessel's mesh it came from
		DWORD group;               ///< the group in that mesh
		std::vector<NTVERTEX> vtx; ///< three vertices for each triangle
	};

	/// A piece of mass m, centre of gravity at cg in the vessel's frame,
	/// given the vessel's state s (global frame). groups: the triangles,
	/// still in the vessel's frame; parts: the vertices of each structural
	/// part in it (vessel frame), for its hulls.
	Debris (const std::string &name, const std::string &source, double m, const Vector &cg,
		const StateVectors &s, std::vector<Group> &&groups, const std::vector<std::vector<Vector>> &parts);
	~Debris ();

	int Type () const override { return OBJTP_DEBRIS; }
	const std::string &Source () const { return source; } ///< name of the vessel it broke off
	const std::vector<Group> &Groups () const { return groups; }
	const std::vector<collision::Hull> &Hulls () const { return hulls; }
	bool AtRest () const { return fstatus == FLIGHTSTATUS_LANDED; }

	void Update (bool force = false) override;
	void PostUpdate () override;
	void UpdateBodyForces ();
	// atmospheric drag, from the state at the start of the step

	/// Adds a velocity change (e.g. the push of the joint that broke)
	void AddVelocity (const Vector &dv);

protected:
	void GetIntermediateMoments (Vector &acc, Vector &tau, const StateVectors &state, double tfrac, double dt) override;
	void InitLanded (Planet *planet, double lng, double lat, double dir, const Matrix *hrot = 0, double cgelev = 0.0, bool asComponent = false) override;
	bool CheckSurfaceContact () const override { return touching; }
	bool ThrustEngaged () const override { return false; }
	void CollectHulls (const StateVectors &s, std::vector<collision::HullRef> &hl) const override;
	double CollisionRadius () const override { return hullBound; }
	void CollectGroundContacts (const StateVectors &s, const StateVectors &sb, int body, double h,
		std::vector<contact::Point> &pts) const override;
	void WakeFromContact () override { wake = true; }
	void ContactStateChanged () override;
	TOUCHDOWN_VTX *HullvtxFirst () override { return nullptr; }
	TOUCHDOWN_VTX *HullvtxNext () override { return nullptr; }

private:
	void SetRestState ();      // the state of a piece at rest on the turning ground
	std::string source;
	std::vector<Group> groups;
	std::vector<collision::Hull> hulls;
	double hullBound = 0.0;    // bounding radius of the hulls
	double area = 0.0;         // [m^2] surface area of its triangles
	Vector Fdrag, Mdrag;       // drag force (global) and torque (body frame) of the step
	mutable bool touching = false;
	bool wake = false;
	double restTime = 0.0;
	double proxyCheckT = 0.0;  // next check for the proxy bodies     // [s] how long it has been still on the ground
};

#endif // !__DEBRIS_H
