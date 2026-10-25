// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Vessel: manoeuvre nodes and the burn autopilot
// =======================================================================

#include "Vessel.h"
#include "Celbody.h"
#include "Orbiter.h"
#include "Log.h"
#include "Astro.h"
#include "Util.h"
#include <algorithm>

extern Orbiter *g_pOrbiter;
extern TimeData td;

namespace {
	const double ALIGN_LEAD  = 90.0;     // [s] start of the alignment before the burn
	const double ALIGN_LIMIT = 2.0*RAD;  // attitude error at which the burn may start
	const double DV_DONE     = 0.02;     // [m/s] remaining velocity change at which the burn ends
}

// =======================================================================
// Nodes

DWORD Vessel::AddManeuverNode (const maneuver::Node &node)
{
	auto it = std::upper_bound (mnodes.begin(), mnodes.end(), node,
		[](const maneuver::Node &a, const maneuver::Node &b) { return a.mjd < b.mjd; });
	it = mnodes.insert (it, node);
	mnode_version++;
	return (DWORD)(it - mnodes.begin());
}

bool Vessel::SetManeuverNode (DWORD idx, const maneuver::Node &node)
{
	if (idx >= mnodes.size()) return false;
	if (idx == 0 && burn.phase != BurnState::NONE && mnodes[0].mjd != node.mjd)
		burn.phase = BurnState::NONE; // (the burn of the edited node restarts)
	mnodes.erase (mnodes.begin() + idx);
	AddManeuverNode (node);
	return true;
}

bool Vessel::DelManeuverNode (DWORD idx)
{
	if (idx >= mnodes.size()) return false;
	if (idx == 0) burn.phase = BurnState::NONE;
	mnodes.erase (mnodes.begin() + idx);
	mnode_version++;
	return true;
}

void Vessel::ClearManeuverNodes ()
{
	mnodes.clear();
	burn.phase = BurnState::NONE;
	mnode_version++;
}

bool Vessel::MainEngines (double &thrust, double &ve, Vector &dir) const
{
	const ThrustGroupSpec *tgs = GetThrusterGroup (THGROUP_MAIN);
	double F = 0.0, flow = 0.0;
	dir.Set (0, 0, 0);
	if (tgs) {
		for (const ThrustSpec *ts : tgs->ts) {
			if (!ts->tank || ts->tank->mass <= 0.0 || ts->isp0 <= 0.0) continue;
			F += ts->maxth0;
			flow += ts->maxth0 / (ts->isp0 * ts->tank->efficiency);
			dir += MakeVector (ts->dir) * ts->maxth0;
		}
	}
	if (F <= 0.0 || dir.length() <= 0.0) return false;
	thrust = dir.length();
	dir /= thrust;
	ve = F / flow;
	return true;
}

double Vessel::BurnDuration (double dv) const
{
	double thrust, ve;
	Vector dir;
	if (!MainEngines (thrust, ve, dir)) return -1.0;
	return maneuver::BurnTime (dv, thrust, mass, ve);
}

// =======================================================================
// Burn autopilot: turns the main engines' thrust direction into the
// node's velocity change shortly before the node, and burns until the
// velocity change is reached (centred on the node time). It uses
// temporary thruster levels (overrides), so the pilot's settings are
// unchanged.

void Vessel::UpdateBurn ()
{
	if (mnodes.empty() || !mnodes[0].autoburn || !cbody || fstatus != FLIGHTSTATUS_FREEFLIGHT) {
		burn.phase = BurnState::NONE;
		return;
	}
	const maneuver::Node &node = mnodes[0];
	double tnode = (node.mjd - td.MJD_ref) * 86400.0;

	double thrust, ve;
	Vector dir;
	if (!MainEngines (thrust, ve, dir)) { // no usable main engine
		burn.phase = BurnState::NONE;
		return;
	}

	if (burn.phase == BurnState::NONE) {
		// velocity change in the orbital frame at the node (two-body estimate)
		Vector r0 (GPos() - cbody->GPos()), v0 (GVel() - cbody->GVel()), r, v;
		double mu = Ggrav * cbody->Mass();
		if (!maneuver::Kepler (r0, v0, mu, tnode - td.SimT1, r, v)) return;
		Vector dvg = maneuver::ToGlobal (node.dv, r, v);
		double dv = dvg.length();
		if (dv < DV_DONE) { // nothing to do
			DelManeuverNode (0);
			return;
		}
		burn.dvg = dvg;
		burn.tburn = maneuver::BurnTime (dv, thrust, mass, ve);
		burn.tstart = tnode - 0.5 * burn.tburn;
		if (td.SimT1 < burn.tstart - ALIGN_LEAD) return; // not yet
		burn.phase = BurnState::ALIGN;
		burn.aligned = false;
		for (int i = NAVMODE_KILLROT; i <= NAVMODE_ANTINORMAL; i++)
			if (i != NAVMODE_HOLDALT) ClrNavMode (i);
		if (td.Warp() > 1.0) g_pOrbiter->SetWarpFactor (1.0);
		LOGOUT ("%s: burn of %.1f m/s, %.1f s from T%+.0f s", name.c_str(), dv, burn.tburn, burn.tstart - tnode);
	} else if (burn.phase == BurnState::BURN) {
		// subtract the velocity change of the last step's thrust
		Vector a (mul (GRot(), Thrust) / mass);
		burn.dvg -= a * td.SimDT;
	}

	// attitude: the thrust direction towards the remaining velocity change
	double dvrem = burn.dvg.length();
	Vector tgtv (tmul (GRot(), burn.dvg / std::max (dvrem, 1e-9))); // (vessel frame)
	// target in a frame with the thrust direction as z axis (the vessel frame
	// for thrust along +z), assuming the attitude axes of the vessel frame
	Vector e3 (dir), e2 (Vector (0, 1, 0) - dir * dir.y);
	if (e2.length() < 1e-3) e2 = Vector (0, 0, -1) - dir * (-dir.z);
	e2.unify();
	Vector e1 (crossp (e2, e3));
	Vector tgt (dotp (tgtv, e1), dotp (tgtv, e2), dotp (tgtv, e3));
	double err = acos (std::clamp (tgt.z, -1.0, 1.0));

	const double dvmax = 5.0*RAD;   // max angular velocity [rad/s]
	const double alpha0 = 20.0*RAD; // angle below which the rate is reduced
	const double f0 = dvmax / alpha0;
	double v1, dth;
	if ((v1 = f0 * asin (std::clamp (tgt.y, -1.0, 1.0))) >= 0.0) dth = (std::min (v1, dvmax) - s0->omega.x) / max_angular_moment[1];
	else                                                          dth = (std::max (v1,-dvmax) - s0->omega.x) / max_angular_moment[0];
	if (tgt.z < 0.0) dth = (dvmax - s0->omega.x) / max_angular_moment[1]; // (pointing away: turn)
	if ((dth *= td.iSimDT * pmi.x * mass) >= 0.0) SetThrusterGroupOverride (THGROUP_ATT_PITCHUP,   std::min ( dth, 0.5));
	else                                           SetThrusterGroupOverride (THGROUP_ATT_PITCHDOWN, std::min (-dth, 0.5));
	if ((v1 = -f0 * atan2 (tgt.x, tgt.z)) >= 0.0) dth = (std::min (v1, dvmax) - s0->omega.y) / max_angular_moment[3];
	else                                          dth = (std::max (v1,-dvmax) - s0->omega.y) / max_angular_moment[2];
	if ((dth *= td.iSimDT * pmi.y * mass) >= 0.0) SetThrusterGroupOverride (THGROUP_ATT_YAWLEFT,  std::min ( dth, 0.5));
	else                                           SetThrusterGroupOverride (THGROUP_ATT_YAWRIGHT, std::min (-dth, 0.5));
	dth = -s0->omega.z / max_angular_moment[s0->omega.z <= 0.0 ? 5 : 4] * td.iSimDT * pmi.z * mass; // no roll
	if (dth >= 0.0) SetThrusterGroupOverride (THGROUP_ATT_BANKRIGHT, std::min ( dth, 0.5));
	else            SetThrusterGroupOverride (THGROUP_ATT_BANKLEFT,  std::min (-dth, 0.5));

	burn.aligned = (err < ALIGN_LIMIT && s0->omega.length() < 0.5*RAD);

	if (burn.phase == BurnState::ALIGN) {
		if (td.SimT1 >= burn.tstart && burn.aligned) burn.phase = BurnState::BURN;
		else return;
	}

	// burn: full thrust, reduced in the last moments to stop at the target
	if (dotp (burn.dvg, mul (GRot(), dir)) <= 0.0 || dvrem < DV_DONE) {
		LOGOUT ("%s: burn complete (%.3f m/s remaining)", name.c_str(), dvrem);
		DelManeuverNode (0);
		return;
	}
	if (err > 10.0*RAD) return; // (off course: realign without thrust)
	double level = std::min (1.0, dvrem * mass / (thrust * std::max (td.SimDT, 0.05) * 2.0));
	SetThrusterGroupOverride (THGROUP_MAIN, level);
}
