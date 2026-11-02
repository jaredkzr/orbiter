// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Implementation of class StarSpec

#include <fstream>
#include <stdio.h>
#include "Orbiter.h"
#include "Config.h"
#include "Star.h"
#include "Camera.h"
#include "Log.h"
#include "Psys.h"
#include <algorithm>
#include <cmath>
#include "StarTargets.h"

using namespace std;

extern Orbiter *g_pOrbiter;
extern PlanetarySystem *g_psys;
extern TimeData td;
extern Camera *g_camera;
extern char DBG_MSG[256];


Star::Star (double _mass, double _mean_radius)
: CelestialBody (_mass, _mean_radius)
{
	Setup ();
}

Star::Star (char *fname)
: CelestialBody (fname)
{
	ifstream ifs (g_pOrbiter->ConfigPath (fname));
	if (!ifs) return;
	bDynamicPosVel = false;
	// read star-specific parameters here
	char cbuf[256];
	if (GetItemString (ifs, "Catalogue", cbuf)) {
		catIndex = startargets::Get ().Find (trim_string (cbuf));
		if (catIndex < 0) LOGOUT_WARN ("Star %s: %s is not a named star of the catalogue; the star stays at the origin", Name(), cbuf);
		else {
			const startargets::Catalogue &c = startargets::Get ();
			teff = c.Stars ()[catIndex].teff;
			lum = c.Luminosity (catIndex);
		}
	}
	GetItemReal (ifs, "Teff", teff);
	GetItemReal (ifs, "Luminosity", lum);
	if (GetItemString (ifs, "Binary", cbuf)) binaryName = trim_string (cbuf);
	if (GetItemString (ifs, "Orbit", cbuf) &&
		sscanf (cbuf, "%lf%lf%lf%lf%lf%lf%lf", &orbP, &orbT, &orbE, &orbA, &orbI, &orbNode, &orbW) == 7)
		hasOrbit = orbP > 0.0;
	else if (GetItemString (ifs, "OrbitAU", cbuf) &&
		sscanf (cbuf, "%lf%lf%lf%lf%lf%lf%lf", &orbP, &orbT, &orbE, &orbA, &orbI, &orbNode, &orbW) == 7)
		hasOrbit = orbP > 0.0, orbitAU = true;
	if (GetItemString (ifs, "Primary", cbuf)) primaryName = trim_string (cbuf);
	if (GetItemString (ifs, "DustRing", cbuf) &&
		sscanf (cbuf, "%lf%lf%lf%lf%lf", &dust.radius, &dust.width, &dust.fraction, &dust.incl, &dust.node) != 5)
		dust = DustRing ();
	// a place of its own (a star or black hole of no catalogue), or orbiting
	// another: an entry of the catalogue (a target of interstellar travel)
	double place[3], uvw[3] = { 0, 0, 0 };
	bool placed = GetItemString (ifs, "Place", cbuf) && sscanf (cbuf, "%lf%lf%lf", place, place + 1, place + 2) == 3;
	if ((placed || (!primaryName.empty () && hasOrbit)) && catIndex < 0) {
		startargets::Star e;
		e.name = Name ();
		if (GetItemString (ifs, "Designation", cbuf)) e.designation = trim_string (cbuf);
		if (placed) {
			const double RAD = 3.14159265358979323846 / 180.0;
			double l = place[0] * RAD, b = place[1] * RAD, d = place[2];
			e.pos[0] = d * std::cos (b) * std::cos (l), e.pos[1] = d * std::cos (b) * std::sin (l), e.pos[2] = d * std::sin (b);
			if (GetItemString (ifs, "Velocity", cbuf)) sscanf (cbuf, "%lf%lf%lf", uvw, uvw + 1, uvw + 2);
			for (int k = 0; k < 3; k++) e.vel[k] = uvw[k];
		}
		e.teff = teff;
		e.mv = startargets::Catalogue::AbsMagV (lum, teff);
		catIndex = startargets::AddEntry (e);
	}
	InitDeviceObjects ();
	Setup ();
}

Star::~Star ()
{
	DestroyDeviceObjects ();
}

void Star::Setup ()
{
	CelestialBody::Setup ();
	ExternPosition(); // try to init from module
}

void Star::Update (bool force)
{
	if (bDynamicPosVel || module && module->bEphemeris())
		CelestialBody::Update (force);
	else if (catIndex >= 0) {
		// (a star of the catalogue: its own law; turning)
		AnchoredState (td.SimT1, s1->pos, s1->vel);
		UpdatePrecession ();
		UpdateRotation ();
	} else {
		// otherwise keep the star at rest at the origin of the global
		// coordinate system: its state carried over to the step's end, its
		// rotation turned (the new state's: else a zero matrix after the
		// states swap)
		s1->pos = s0->pos, s1->vel = s0->vel;
		UpdatePrecession ();
		UpdateRotation ();
	}
}

Vector Star::Pos2Barycentre (Vector &pos)
{
	// by definition, the barycentre of stars (root objects) is the
	// barycentre of the system: the global frame's origin unless it moved
	return g_psys ? -g_psys->Origin () : Vector();
}

void Star::LinkPrimary (const PlanetarySystem *psys)
{
	if (primaryName.empty () || !psys) return;
	const CelestialBody *b = psys->GetGravObj (primaryName.c_str (), true);
	orbPrimary = dynamic_cast<const Star*> (b);
	if (!orbPrimary || orbPrimary->catIndex < 0) {
		LOGOUT_WARN ("Star %s: its primary %s is not a star of the catalogue; the star stays at the origin", Name (), primaryName.c_str ());
		orbPrimary = nullptr;
		return;
	}
	// (its catalogue entry: at the primary's place)
	if (catIndex >= 0) {
		startargets::Star e = startargets::Get ().Stars ()[catIndex];
		e.follow = orbPrimary->catIndex;
		startargets::AddEntry (e);
	}
}

const Star *Star::Binary () const
{
	if (!binary && !binaryName.empty () && g_psys) {
		const CelestialBody *b = g_psys->GetGravObj (binaryName.c_str (), true);
		if (b && (b->Type () == OBJTP_STAR) && ((const Star*)b)->catIndex >= 0) binary = (const Star*)b;
	}
	return binary;
}

Vector Star::RelativeOrbit (double t, const Vector &dir, double dpc) const
{
	// Thiele-Innes: offsets north (x) and east (y) on the sky ["], and
	// along the line of sight (z, away from the Sun), of the secondary
	const double PI = 3.14159265358979323846, AU = 1.495978707e11;
	double yr = (td.MJD_ref + t / 86400.0 - 51544.5) / 365.25 + 2000.0;
	double M = 2.0 * PI * (yr - orbT) / orbP, E = M;
	// (Kepler's equation by Newton's method: the S-stars' eccentricities near 1)
	M = std::remainder (M, 2.0 * PI);
	E = orbE > 0.8 ? (M < 0.0 ? -PI : PI) : M;
	for (int k = 0; k < 50; k++) {
		double dE = (E - orbE * std::sin (E) - M) / (1.0 - orbE * std::cos (E));
		E -= dE;
		if (std::fabs (dE) < 1e-15) break;
	}
	double X = std::cos (E) - orbE, Y = std::sqrt (1.0 - orbE * orbE) * std::sin (E);
	Vector P, Q;
	SkyOrbitAxes (dir, orbI, orbNode, orbW, P, Q);
	return (P * X + Q * Y) * (orbitAU ? orbA * AU : orbA * dpc * AU);
}

void SkyOrbitAxes (const Vector &dir, double i, double node, double w, Vector &P, Vector &Q)
{
	const double RAD = 3.14159265358979323846 / 180.0;
	double ci = std::cos (i * RAD), si = std::sin (i * RAD), cO = std::cos (node * RAD), sO = std::sin (node * RAD);
	double cw = std::cos (w * RAD), sw = std::sin (w * RAD);
	double A = cw * cO - sw * sO * ci, B = cw * sO + sw * cO * ci, F = -sw * cO - cw * sO * ci, G = -sw * sO + cw * cO * ci;
	double C = sw * si, H = cw * si;
	// (the sky's axes at the star, right-handed ecliptic: north (n), east (e),
	// away from the Sun (r); the celestial pole at obliquity 23.4393;
	// Orbiter's frame is (x, z, y) of it)
	const double eps = 23.4392911 * RAD;
	Vector r (dir.x, dir.z, dir.y), pole (0.0, std::sin (eps), std::cos (eps));
	Vector e (crossp (pole, r));
	e.unify ();
	Vector n (crossp (r, e));
	Vector p (n * A + e * B + r * C), q (n * F + e * G + r * H);
	P = Vector (p.x, p.z, p.y), Q = Vector (q.x, q.z, q.y);
}

Vector Star::DustAxis () const
{
	// (the normal of an orbit in the ring's plane, seen from the Sun)
	Vector helio (GPos () + (g_psys ? g_psys->Origin () : Vector ()));
	Vector dir (helio.length () > 0.0 ? helio.unit () : Vector (1, 0, 0));
	Vector P, Q;
	SkyOrbitAxes (dir, dust.incl, dust.node, 0.0, P, Q);
	return crossp (P, Q).unit ();
}

bool Star::AnchoredState (double t, Vector &x, Vector &v) const
{
	// orbiting another: on the orbit about it (its relative velocity from the
	// orbit over a short interval: a ten-thousandth of its period)
	if (orbPrimary && hasOrbit) {
		if (!orbPrimary->AnchoredState (t, x, v)) x = orbPrimary->GPos (), v = orbPrimary->GVel ();
		Vector helio (x + (g_psys ? g_psys->Origin () : Vector ()));
		double dpc = helio.length () / 3.0856775814913673e16;
		Vector dir (helio.unit ());
		double h = std::min (3600.0, orbP * 365.25 * 86400.0 * 1e-4);
		Vector r0 (RelativeOrbit (t - h, dir, dpc)), r1 (RelativeOrbit (t + h, dir, dpc));
		x += RelativeOrbit (t, dir, dpc), v += (r1 - r0) / (2.0 * h);
		return true;
	}
	if (catIndex < 0) return false;
	const startargets::Catalogue &c = startargets::Get ();
	c.State (catIndex, t, x, v);
	const Star *b = Binary ();
	if (!b) return true;
	// a binary: the barycentre of the two catalogue states, each star offset
	// from it along the pair's orbit
	Vector xb, vb;
	c.State (b->catIndex, t, xb, vb);
	double m = Mass (), mb = b->Mass (), mt = std::max (m + mb, 1.0);
	Vector X ((x * m + xb * mb) / mt), V ((v * m + vb * mb) / mt);
	Vector helio (X + (g_psys ? g_psys->Origin () : Vector ()));
	double dpc = helio.length () / 3.0856775814913673e16;
	Vector dir (helio.unit ());
	const Star *prim = hasOrbit ? this : b->hasOrbit ? b : nullptr;
	if (!prim) { x = X, v = V; return true; }
	const double h = 3600.0;
	Vector r0 (prim->RelativeOrbit (t - h, dir, dpc)), r1 (prim->RelativeOrbit (t + h, dir, dpc));
	Vector r (prim->RelativeOrbit (t, dir, dpc)), dr ((r1 - r0) / (2.0 * h));
	// (each moves by the other's share of the mass: the primary against the
	// secondary's relative motion, the secondary with it)
	double f = prim == this ? -mb / mt : mb / mt;
	x = X + r * f, v = V + dr * f;
	return true;
}

Vector4 Star::GetLightColor ()
{
	return {1,1,1,1};
}
