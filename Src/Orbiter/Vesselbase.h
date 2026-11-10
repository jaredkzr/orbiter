// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#ifndef __VESSELBASE_H
#define __VESSELBASE_H

#include "Rigidbody.h"
#include "elevmgr.h"
#include "ContactSolver.h"
#include "CollisionHull.h"

namespace hydro { struct Model; struct Result; }

typedef enum {
	FLIGHTSTATUS_FREEFLIGHT,
	FLIGHTSTATUS_LANDED,
	FLIGHTSTATUS_TAXIING,
	FLIGHTSTATUS_DOCKED,
	FLIGHTSTATUS_CRASHED,
	FLIGHTSTATUS_UNDEFINED
} FlightStatus;

typedef struct { // touchdown vertex specifications
	Vector pos;          // touchdown point position (uncompressed) in vessel frame
	double stiffness;    // suspension stiffness
	double damping;      // suspension damping
	double compression;  // suspension compression factor
	double mu;           // isotropic/transversal friction coefficient
	double mu_lng;       // longitudinal friction coefficient (only used for first 3 points)
} TOUCHDOWN_VTX;

struct WindPrm;

struct SurfParam {//Surface-relative vessel state
	void Set (const StateVectors &s, const StateVectors &s_ref, const CelestialBody *ref,
		std::vector<ElevationTile> *etilecache=NULL, WindPrm *windprm=NULL);
	// Set surface parameters from object and reference state vectors

	static double ComputeAltitude(const StateVectors &s, const StateVectors &s_ref, const CelestialBody *ref,
		std::vector<ElevationTile> *etilecache=NULL);
	// recalculate altitude

	void SetLanded (double lng, double lat, double alt, double dir, const Vector &nml, const CelestialBody *ref);
	// Set surface parameters for a landed object

	const CelestialBody *ref; // body to which data refer
	Vector ploc;              // ship position in planet local cartesian coords
	Vector groundvel_glob;    // ground-relative velocity in global frame
	Vector groundvel_ship;    // ground-relative velocity in local vessel frame
	Vector airvel_glob;       // airspeed vector in global frame
	Vector airvel_ship;       // airspeed vector in local vessel frame
	double groundspd;         // ground-relative velocity magnitude
	double airspd;            // airspeed magnitude
	double vspd;              // vertical velocity (negative for descent)
	double lng, lat, rad;     // ship position in planet's local equatorial coords
	double slng, clng, slat, clat; // sines and cosines of lng and lat
	double alt;               // ship altitude over ground
	double alt0;              // ship altitude over mean radius
	double elev;              // ground elevation with respect to mean planet radius
	int elev_lvl;             // resolution level at which elevation was computed
	Vector surfnml;           // surface normal in local horizon frame (+y=up)
	double pitch, bank;       // vessel orientation w.r.t. horizon
	double dir;               // compass orientation
	bool is_in_atm;           // true if ship is within a planetary atmosphere
	double dynp;              // dynamic pressure: 1/2 rho * speed^2
	double atmp;              // atmospheric pressure [Pa]
	double atmrho;            // atmosphere density [kg/m^3]
	double atmT;              // atmospheric ambient temperature [K]
	double atmM;              // Mach number
	Matrix L2H;               // rotation matrix planet-local -> local horizon
};

struct WindPrm {           // per-vessel wind parameters
	double pert_t;            // time for wind perturbation vector
	Vector pert_v;            // wind perturbation vector
	Vector pert_v2;           // its fast part (gas giants: the chop)
};

// =======================================================================

class Planet;
class Base;

// =======================================================================
// Class VesselBase

class VesselBase: public RigidBody {
	friend class VesselCollider;
public:
	VesselBase ();
	VesselBase (double _mass, double _size, const Vector &_pmi);

	virtual void Update (bool force = false) = 0;
	// Update vessel state by propagating across current time step

	virtual void PostUpdate ();

	inline CelestialBody *ProxyBody() { return proxybody; }
	inline Planet *ProxyPlanet() { return proxyplanet; }
	inline const Planet *ProxyPlanet() const { return proxyplanet; }
	// closest celestial object (star, planet, moon)

	inline Base *ProxyBase () const { return proxybase; }
	// closest surface base

	virtual void RPlace (const Vector &rpos, const Vector &rvel) {}
	// Set vessel position and velocity in parent coords
	// If the vessel is part of a superstructure, the complete structure
	// is moved accordingly

	virtual void Refuel () {}
	// Re-fill all tanks

	const char *CurPropagatorStr (bool verbose = true) const
	{
		const VesselBase *super = GetSuperStructure();
		return (super ? super->CurPropagatorStr(verbose) : RigidBody::CurPropagatorStr(verbose));
	}

	const int CurPropagatorSubsamples () const
	{
		const VesselBase *super = GetSuperStructure();
		return (super ? super->CurPropagatorSubsamples() : RigidBody::CurPropagatorSubsamples());
	}

	inline double Elevation() const
	{ return sp.elev; }
	// Surface elevation with respect to mean planet radius below the current
	// vessel position.

	inline double Altitude() const
	{ return sp.alt; }

	/// Flying freely (not landed, not docked into another)
	inline bool InFreeFlight () const
	{ return fstatus == FLIGHTSTATUS_FREEFLIGHT; }

	inline bool GroundContact() const
	{ return bSurfaceContact; }

	/// Surface-relative state (0 if no body is near)
	inline const SurfParam *SurfaceParams () const
	{ return proxybody ? &sp : 0; }

protected:
	virtual void SetDefaultState ();
	// Reset all state parameters to default values

	virtual bool Activate (bool force = false);
	// Switch to active flight mode

	void UpdateSurfParams ();
	// update surface parameters

	virtual void InitLanded (Planet *planet, double lng, double lat, double dir,
		const Matrix *hrot=0, double cgelev=0.0, bool asComponent=false) {}
	// init vessel position on ground
	// Note: InitLanded must be called after the update phase (after EndStateUpdate)

	double LandedHeading (double lng, double lat) const;
	// Returns the heading of a vessel landed at lng/lat
	// using state vectors s0

	virtual void GetIntermediateMoments (Vector &acc, Vector &tau,
		const StateVectors &state, double tfrac, double dt);
	// Returns acceleration acc and torque tau, at time SimT0+tfrac*SimDT
	// and step size dt, given intermediate state in global frame

	virtual void UpdateProxies ();

	virtual void SetProxyplanet (Planet *p) { proxyplanet = p; }

	bool SurfaceProximity () const;
	// Returns true if vessel is withing 3 radii of planetary surface

	virtual bool CheckSurfaceContact () const = 0;
	// Returns true if any part of the vessel is in contact with a planet surface
	// Should be called only after update phase, and assumes that sp is up to date.

	virtual bool ThrustEngaged () const = 0;
	// Returns true if any thrusters are engaged at the current time step

	virtual bool IsComponent () const { return false; }
	// Returns true if *this is a component of a superstructure

	virtual const VesselBase *GetSuperStructure () const { return NULL; }

	void ResolveContacts (double h);
	// Resolve ground contact after the step was propagated (GroundContact.h):
	// impulses change the new velocities, and the new position follows them

	static constexpr double OBSTACLE_REACH = 500.0;
	// [m] a step moving the vessel farther than this over the ground (high
	// time acceleration, a fall at kilometres a second) leaves out the
	// obstacles on the ground (rocks, buildings), and its impact on the
	// terrain is found along its path (SurfaceImpact) rather than by contacts

	bool SurfaceImpact () override;
	// A long step that ends below the terrain: the impact, found along the
	// step's path over the turning planet; handled by Impacted (true)

	virtual void Impacted (const Vector &gpos, const Vector &gvel, double speed);
	// The impact of a long step at gpos (global; on the terrain) at speed
	// [m/s] relative to the ground (gvel: the ground's velocity there). By
	// default the vessel comes to rest there, and is landed after the step.

	bool landPending = false;
	void TakePendingLanding ();
	// (after the update phase: a vessel set down by an impact is landed,
	// before contacts over long steps could throw it about)

	contact::Body ContactBody () const;
	// The vessel as a body for the contact solver, after the step's propagation

	void ApplyContactVelocity (const contact::Body &b, double h, bool groundFrame);
	// Take on the velocities of a body resolved by the contact solver; the new
	// position and orientation follow them over the step h (groundFrame:
	// relative to the turning ground, close to a planet's surface)

	virtual void ContactStateChanged () {}
	/// (pre: the bodies' velocities before the contacts, if known)
	virtual void ContactsResolved (const std::vector<contact::Point> &pts, int index, double h, const std::vector<contact::Body> *pre = nullptr) {}
	// the contacts of the step were resolved (index: the body's in pts; the
	// impulses are in the points), e.g. for structural loads
	// The new state (s1) was changed after the update, by the contacts with
	// other vessels: update what depends on it

	virtual void CollectHulls (const StateVectors &s, std::vector<collision::HullRef> &hulls) const {}
	// Collision hulls of the vessel (or the components of a composite) at state s

	virtual void CollectFreePorts (const StateVectors &s, std::vector<collision::Port> &ports) const {}
	// Free docking ports at state s (vessel-vessel contacts are not made near
	// two ports lined up with each other, so that the vessels can dock)

	virtual void WakeFromContact () {}

	virtual bool InPlayback () const { return false; }

	virtual double CollisionRadius () const { return -1.0; }
	// Bounding radius of the collision hulls about the centre of gravity
	// (< 0: no hulls)
	// state from a flight recording (not changed by contacts)
	// Hit by another vessel while landed: become dynamic in the next step

	// The water (Sea.h, Hydro.h; VesselWater.cpp)
	void HydroStep (double h) override;
	struct WaterState {
		bool near = false;        ///< within reach of the water in the last step
		bool wet = false;         ///< touching it
		double submerged = 0.0;   ///< [m^3] at the end of the step
		double volume = 0.0;      ///< [m^3] the model's (its displacement fully under)
		double wetted = 0.0;      ///< [m^2]
		double waterline = 0.0;   ///< [m^2]
		double entrySpeed = 0.0;  ///< [m/s] of the last water entry (the fastest panel speed)
		double slamMax = 0.0;     ///< [Pa] of the last entry
		double accMax = 0.0;      ///< [m/s^2] the water's highest acceleration in the last entry
		double tWet = -1e10;      ///< when it last became wet [s simulation time]
		int substeps = 0;
		Vector F;                 ///< the water's mean force in the last step (global frame)
	};
	const WaterState &Water () const { return water; }
	/// the water's model of the body (built on demand; null: none)
	virtual const hydro::Model *WaterModel () const { return nullptr; }
	/// centre of gravity (body frame)
	virtual Vector WaterCG () const { return Vector (0, 0, 0); }
	/// structural parts the water's forces are given to (0: none)
	virtual int WaterParts () const { return 0; }
	/// the water's forces of a step (r.partF per part), e.g. for structural loads
	virtual void WaterLoads (const hydro::Result &r, double h) {}
	WaterState water;

	bool contact_deferred = false; // ground contacts are resolved with the contacts with other vessels in this step
	const VesselBase *contact_vessel = nullptr; // the vessel it last touched (pressed against)
	double contact_vessel_t = -1e10;            // ... when [s simulation time]
	/// Whether it touches another vessel (in the last moment)
	bool Touching (const void *other) const;

	virtual void CollectGroundContacts (const StateVectors &s, const StateVectors &sb, int body, double h,
		std::vector<contact::Point> &pts) const {}
	// Touchdown points in contact, or about to be within the step h, with the
	// ground, for the body with state sb (index body in the solver) that this
	// vessel is (part of); s: this vessel's state. States at the start of the
	// step; vectors relative to the body's centre in the global frame's axes.

	virtual TOUCHDOWN_VTX *HullvtxFirst () = 0;
	virtual TOUCHDOWN_VTX *HullvtxNext () = 0;
	// Hull vertex iterator

	// Ground contact: force and moment of the last step (body frame), and
	// the points of contact
	Vector contact_force, contact_moment;
	std::vector<contact::Point> contact_pts;

	// Status flags
	FlightStatus fstatus;
	CelestialBody *proxybody;    // closest 'massive' object (star, planet, moon)
	Planet *proxyplanet;         // closest 'landable' object (planet or moon)
	Base *proxybase;             // closest surface base
	bool bSurfaceContact;        // signal vessel is in contact with planet surface

	SurfParam sp;      // ship parameters concerning planet surface
	Matrix land_rot;   // rotates ship's local into planet's local coords so that grot = grot(planet) * land_rot

	mutable std::vector<ElevationTile> etile;
	mutable WindPrm windp;

	struct LANDING_TEST {        // parameters for testing LANDED status eligibility
		bool testing;
		double testt;
		Vector surfpos;
		Quaternion surfrot;
	} LandingTest;

private:
	void CheckLanded ();
	// Switch to flight status "LANDED" if criteria are satisfied

	double proxyT;               // next check for proxybody
};

#endif // !__VESSELBASE_H