// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A black hole: a root body of a planetary system (like a star) with mass
// and spin, a Kerr spacetime (Kerr.h). Configured in the system file as
// BlackHole<n> = <name>, its file <name>.cfg giving Mass [kg] and Spin
// (a/M, 0 to 0.998; positive: turning like Earth, counterclockwise seen
// from its north pole, the axis set by Obliquity and LAN as a planet's).
// Optionally an accretion disc: AccretionRate (in units of the Eddington
// rate: Mdot = rate L_Edd / (eta c^2), eta = 1 - E_ISCO) and DiscOuter (its
// outer radius in masses, default 1000): a thin Novikov-Thorne disc from
// the ISCO out, in the spin's plane. DiscGlow (default 0; an artistic
// liberty): the disc's cool outer parts, too cool to glow, drawn lit by
// its hot inner parts and a donor star (1: clearly seen). Jets (default 0;
// an artistic liberty): relativistic jets along the spin axis, of that
// brightness, JetLength masses long (default 250).
// Its size is its horizon's radius. Vessels within its strong field
// (StrongRange) fly in its Kerr spacetime (RelFlight.cpp).
//
// The Kerr coordinates are right-handed, the spin along +z; Orbiter's
// frame is left-handed: a vector (x, y, z) is (x, z, y) right-handed.
// Lengths in metres, time as c t [m] (geometric units).
// =======================================================================

#ifndef __BLACKHOLE_H
#define __BLACKHOLE_H

#include "Star.h"
#include "Kerr.h"

class BlackHole: public Star {
public:
	/// Strong field: Kerr flight within this many masses (GM/c^2) of the
	/// hole (beyond, first post-Newtonian gravity is good to ~1e-8)
	static constexpr double KERR_RANGE = 1e4;
	static constexpr double MAX_SPIN = 0.998; // (Thorne's limit)

	BlackHole (char *fname);

	int Type() const { return OBJTP_BLACKHOLE; }

	/// the spacetime [m]: M = G m / c^2, a = spin M
	const kerr::Hole &Kerr () const { return hole; }
	double Spin () const { return hole.a / hole.M; }
	double StrongRange () const { return KERR_RANGE * hole.M; }

	/// a vector of the global frame relative to the hole into Kerr
	/// coordinates, and back
	void ToKerr (const Vector &v, double *k) const;
	Vector FromKerr (const double *k) const;
	/// move a body's state relative to the hole (global frame axes) over
	/// coordinate time dt, under a proper acceleration alpha (global axes,
	/// rest frame; zero: free) (false: not possible, unchanged)
	bool Drift (Vector &rpos, Vector &rvel, double dt, const Vector &alpha = Vector()) const;
	/// Local conditions of a body at rpos, rvel relative to the hole
	/// (global axes, SI)
	struct Local {
		double r = 0.0;          ///< spheroidal radius [m]
		double clockRate = 1.0;  ///< dtau/dt of a clock moving with the body (0: inside, not defined)
		double tidal[3][3] = {}; ///< tidal tensor [1/s^2] (global axes): acceleration relative to the body's centre per metre
		double tidalMax = 0.0;   ///< its largest eigenvalue's size [1/s^2]
		bool canHover = false;   ///< outside the ergosphere (static observers exist)
		Vector hover;            ///< proper acceleration to stay at rest there (global axes) [m/s^2]
		double speedStatic = 0.0;///< speed relative to the observer at rest there [m/s] (canHover)
		bool inside = false;     ///< inside the horizon
	};
	bool LocalState (const Vector &rpos, const Vector &rvel, Local &loc) const;
	/// proper acceleration (global axes) holding a body at rest at rpos
	/// (false within the ergosphere: impossible)
	bool HoverAccel (const Vector &rpos, Vector &alpha) const;
	/// coordinate acceleration (global axes) of a free body at rpos, rvel
	/// relative to the hole [m/s^2] (gravity, as the guidance takes it out)
	Vector FreeAccel (const Vector &rpos, const Vector &rvel) const;
	/// coordinate velocity (global axes) of the circular orbit through
	/// rpos, prograde, about the spin axis (exact in the equatorial plane);
	/// false within the innermost circular (photon) orbit
	bool CircularVelocity (const Vector &rpos, Vector &rvel) const;

	/// the accretion disc: rate (Eddington units, 0: none), its outer radius
	/// [m], the temperature scale T_K: T (r) = T_K (r_M^2 DiscFluxShape)^(1/4)
	/// (r in masses), its hottest [K], its luminosity [W]
	double DiscRate () const { return discRate; }
	double DiscOuter () const { return discOuter * hole.M; }
	double DiscTempScale () const { return discTK; }
	double DiscTmax () const { return discTmax; }
	double DiscLuminosity () const { return discL; }
	/// the cool outer disc drawn lit (0: not drawn; 1: clearly seen)
	double DiscGlow () const { return discGlow; }
	/// jets along the spin axis drawn (0: none; 1: clearly seen), their length [masses]
	double Jets () const { return jets; }
	double JetLength () const { return jetLength; }

	/// the spin axis (north pole) in the global frame; the Kerr x, y axes
	Vector SpinAxis () const { return axis; }
	Vector KerrX () const { return e1; }
	Vector KerrY () const { return e2; }

	void Update (bool force = false);

private:
	void SetFrame ();
	kerr::Hole hole;
	double discRate = 0.0, discOuter = 1000.0, discTK = 0.0, discTmax = 0.0, discL = 0.0, discGlow = 0.0;
	double jets = 0.0, jetLength = 250.0;
	Vector axis;     // spin axis (global)
	Vector e1, e2;   // the Kerr x and y axes (global): e1 x e2 = axis right-handed
};

#endif // !__BLACKHOLE_H
