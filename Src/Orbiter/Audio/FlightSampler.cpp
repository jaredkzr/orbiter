// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "FlightSampler.h"
#include "AudioSystem.h"
#include "XrConfig.h"
#include "Vessel.h"
#include "Celbody.h"
#include <cmath>
#include <cstring>

namespace audio {

namespace {
	Vessel *Mutable (const Vessel *v) { return const_cast<Vessel*>(v); }

	/// The lowest of a vessel's touchdown points above the ground [m] (the
	/// gear's, or the hull's: its height, not its centre's)
	double LowestAltitude (VESSEL *v)
	{
		double alt = v->GetAltitude (ALTMODE_GROUND);
		VECTOR3 up;
		v->HorizonInvRot (_V(0, 1, 0), up);
		double low = 0.0;
		bool any = false;
		DWORD n = v->GetTouchdownPointCount ();
		for (DWORD i = 0; i < n; i++) {
			TOUCHDOWNVTX tv;
			if (!v->GetTouchdownPoint (tv, i)) continue;
			double h = dotp (tv.pos, up);
			if (!any || h < low) low = h, any = true;
		}
		return alt + (any ? low : 0.0);
	}

	/// The fraction left in a tank (< 0: none)
	double FuelLeft (VESSEL *v, PROPELLANT_HANDLE ph)
	{
		if (!ph) return -1.0;
		double mx = v->GetPropellantMaxMass (ph);
		return mx > 0.0 ? v->GetPropellantMass (ph) / mx : -1.0;
	}
}

double GearState (System &sys, const Vessel *v)
{
	if (!v) return -1.0;
	int a = sys.GearAnim (v);
	if (a < 0) a = XrConfig::For (v->ClassName () ? v->ClassName () : "").GearAnim ();
	if (a < 0) return -1.0;
	VESSEL *vi = Mutable (v)->GetModuleInterface ();
	return vi ? vi->GetAnimation ((UINT)a) : -1.0;
}

bool SampleFlight (System &sys, const Vessel *vessel, double simt, double simdt, FlightSample &s)
{
	s = FlightSample ();
	if (!vessel || vessel->Destroyed ()) return false;
	VESSEL *v = Mutable (vessel)->GetModuleInterface ();
	if (!v) return false;
	s.t = simt, s.dt = simdt;
	// height and motion
	s.alt = LowestAltitude (v);
	s.altitude = v->GetAltitude ();
	s.airspeed = v->GetAirspeed (), s.groundspeed = v->GetGroundspeed ();
	VECTOR3 as, hv;
	v->GetAirspeedVector (FRAME_HORIZON, as);
	s.vs = as.y;
	v->GetGroundspeedVector (FRAME_HORIZON, hv);
	s.hvUp = hv.y;
	s.atmP = v->GetAtmPressure ();
	s.mach = s.atmP > FlightState::ATM_P ? v->GetMachNumber () : 0.0;
	s.q = v->GetDynPressure ();
	s.contact = v->GroundContact () || (v->GetFlightStatus () & 1);
	// (a black hole has no ground: its horizon no terrain to warn of, nothing
	// to touch down on or lift off from - only far above it)
	if (OBJHANDLE ref = v->GetSurfaceRef ())
		if (oapiGetObjectType (ref) == OBJTP_BLACKHOLE) s.alt = s.altitude = 1e12, s.vs = s.hvUp = 0.0, s.contact = false;
	s.gear = GearState (sys, vessel);
	// engines
	s.main = v->GetThrusterGroupLevel (THGROUP_MAIN);
	s.hover = v->GetThrusterGroupLevel (THGROUP_HOVER);
	s.retro = v->GetThrusterGroupLevel (THGROUP_RETRO);
	s.hasMain = v->GetGroupThrusterCount (THGROUP_MAIN) > 0;
	if (s.hasMain) {
		VECTOR3 dir, up, av;
		v->GetThrusterDir (v->GetGroupThruster (THGROUP_MAIN, 0), dir);
		v->HorizonInvRot (_V(0, 1, 0), up);
		s.mainUp = dotp (dir, up) > 0.85;
		v->GetAngularVel (av);
		s.rollRate = std::fabs (dotp (av, dir));
	}
	// the orbit: the periapsis above the sensible atmosphere (14 scale heights, a millionth of the
	// ground's pressure - about 120 km on Earth - not its model's limit; airless: above the mountains)
	double pe = 0.0;
	if (OBJHANDLE ref = v->GetPeDist (pe)) {
		s.hasRef = true;
		const double R = oapiGetSize (ref);
		double top = 10e3;
		const ATMCONST *ac = oapiGetPlanetAtmConstants (ref);
		if (ac && ac->p0 > 0.0 && ac->rho0 > 0.0) {
			const double g = GGRAV * oapiGetMass (ref) / (R * R);
			top = std::min (ac->altlimit, 14.0 * ac->p0 / (ac->rho0 * g));
		}
		s.orbit = pe > R + top;
		ELEMENTS el;
		double mjd;
		if (v->GetElements (el, mjd)) s.escape = el.e >= 1.0;
		s.airless = !ac || ac->p0 <= 0.0;
	}
	if (const CelestialBody *er = vessel->ElRef ()) s.elRef = er, s.elRefName = er->Name ();
	if (OBJHANDLE sref = v->GetSurfaceRef ()) {
		char body[64] = "";
		oapiGetObjectName (sref, body, sizeof (body));
		s.surfaceName = body;
	}
	// docking: our first port to the nearest IDS signal tuned
	if (DOCKHANDLE hd = v->GetDockHandle (0)) {
		s.docked = v->DockingStatus (0) != 0;
		VECTOR3 pos, dir, rot, gpos;
		v->GetDockParams (hd, pos, dir, rot);
		v->Local2Global (pos, gpos);
		for (DWORD i = 0; i < 4 && !s.docked; i++) {
			NAVHANDLE hn = v->GetNavSource (i);
			if (!hn) continue;
			NAVDATA nd;
			oapiGetNavData (hn, &nd);
			if (nd.type != TRANSMITTER_IDS) continue;
			VECTOR3 np;
			oapiGetNavPos (hn, &np);
			double d = length (gpos - np);
			if (s.dockDist < 0.0 || d < s.dockDist) s.dockDist = d;
		}
	}
	// modes
	for (int i = 2; i <= 7; i++) if (v->GetNavmodeState (i)) { s.nav = i; break; }
	if (!s.nav && v->GetNavmodeState (NAVMODE_KILLROT)) s.nav = NAVMODE_KILLROT;
	s.rcs = v->GetAttitudeMode ();
	s.af = (int)std::min (v->GetADCtrlMode (), (DWORD)2);
	// fuel
	PROPELLANT_HANDLE mainTank = nullptr, rcsTank = nullptr;
	if (v->GetGroupThrusterCount (THGROUP_MAIN)) mainTank = v->GetThrusterResource (v->GetGroupThruster (THGROUP_MAIN, 0));
	for (int g = THGROUP_ATT_PITCHUP; g <= THGROUP_ATT_BACK && !rcsTank; g++)
		if (v->GetGroupThrusterCount ((THGROUP_TYPE)g)) rcsTank = v->GetThrusterResource (v->GetGroupThruster ((THGROUP_TYPE)g, 0));
	if (rcsTank == mainTank) rcsTank = nullptr; // (one tank: the main's warnings)
	s.mainFuel = FuelLeft (v, mainTank), s.rcsFuel = FuelLeft (v, rcsTank);
	s.mainExternal = mainTank != nullptr;
	for (DWORD i = 0; s.mainExternal && i < v->GetPropellantCount (); i++)
		if (v->GetPropellantHandleByIndex (i) == mainTank) s.mainExternal = false;
	// heat, load, light
	s.heat = vessel->HeatFraction ();
	s.load = vessel->LoadFraction ();
	s.dark = vessel->IlluminationFactor () < 0.5;
	const SurfParam *sp = vessel->GetSurfParam ();
	s.lowAir = sp && sp->is_in_atm && sp->alt < 100e3;
	return true;
}

} // namespace audio
