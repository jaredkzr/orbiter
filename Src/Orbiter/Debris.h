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
#include "TimeData.h"
#include <string>
#include <vector>

class Mesh;
extern TimeData td;

class Debris: public VesselBase {
	friend class Replay; // (its states shown in the instant replay)
public:
	/// The triangles of one group of a mesh of the vessel (debris frame)
	struct Group {
		const Mesh *mesh;          ///< the vessel's mesh it came from
		DWORD group;               ///< the group in that mesh
		std::vector<NTVERTEX> vtx; ///< three vertices for each triangle
		std::vector<DWORD> tris;   ///< the triangles' indices in the group (scenarios)
		Vector ofs;                ///< the mesh's offset in the debris frame
		std::vector<float> edge;   ///< for each triangle: at the fracture (1) or not (0)
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
	double CreatedT () const { return created_t; } ///< [s] simulation time it broke off
	DWORD Serial () const { return serial; }        ///< (unique: handles may be reused)

	/// Propellant burning in it [kg] (released when its vessel broke up),
	/// burning for burnTime [s]
	void Ignite (double fuel, double burnTime);
	double Fuel () const { return fuel; }
	double BurnLeft () const;                        ///< [s] (0: not burning)
	/// Its temperature [K] when it broke off (hot parts, burned off)
	void SetTemp (double T) { temp0 = T, tempT = td.SimT1; }
	double Temp () const;                            ///< [K] cooling down
	/// Ground contacts of the last step: the fastest sliding [m/s] and the
	/// velocity change of the impact [m/s]
	double Scrape () const { return scrape; }
	double Impact () const { return impact; }
	bool Hidden () const { return hidden; }        ///< not shown (instant replay: not broken off yet)
	/// The parts of its vessel it is made of (bit i: part i, i < 64; 0: not known)
	uint64_t Parts () const { return parts; }
	void SetParts (uint64_t mask) { parts = mask; }
	void SetHidden (bool h) { hidden = h; }

	void Update (bool force = false) override;
	void PostUpdate () override;
	void UpdateBodyForces ();
	// atmospheric drag, from the state at the start of the step

	/// Adds a velocity change (e.g. the push of the joint that broke)
	void AddVelocity (const Vector &dv);

	/// Writes the piece to a scenario (BEGIN_DEBRIS section): its state,
	/// the triangles of the meshes it was made of (by the meshes' names) and
	/// its hulls. Pieces of meshes without a name (made by a module) are
	/// not written.
	void Write (std::ostream &os) const;
	/// Reads a piece from a scenario (after its PIECE line, up to END);
	/// null if it cannot be made
	static Debris *Read (std::istream &is, const std::string &name);

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
	void ContactsResolved (const std::vector<contact::Point> &pts, int index, double h) override;
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
	double restTime = 0.0;     // [s] how long it has been still on the ground
	double proxyCheckT = 0.0;  // next check for the proxy bodies
	double created_t = 0.0;    // [s] simulation time it broke off
	DWORD serial;
	double fuel = 0.0, burnEnd = 0.0;
	double temp0 = 0.0, tempT = 0.0;
	double scrape = 0.0, impact = 0.0;
	bool hidden = false;
	uint64_t parts = 0;
};

#endif // !__DEBRIS_H
