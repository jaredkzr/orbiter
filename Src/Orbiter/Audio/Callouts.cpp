// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Callouts.h"
#include "AudioSystem.h"
#include "XrConfig.h"
#include "XRSoundShim.h"
#include "Vessel.h"
#include "Orbiter.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern Orbiter *g_pOrbiter;

namespace audio {

namespace {
	constexpr double KNOTS100 = 100.0 * 0.514444;   // [m/s]
	constexpr double AIRBORNE_AFTER = 0.5;          // [s] off the ground: flying (not a bounce)
	constexpr double MIN_GAP = 1.0;                 // [s] between altitude or distance callouts
	constexpr double CLEARED_ALT = 1500.0;          // [m] "cleared to land"
	constexpr double BEEP_RANGE = 500.0;            // [m] the docking radar beeps within this
	constexpr double LOW_FUEL = 0.05;               // fuel low below 5 %
	constexpr double MAXQ_MIN = 5000.0;             // [Pa] a Max Q worth calling
	constexpr double ATM_P = 10.0;                  // [Pa] Mach callouts above this pressure

	/// XRSound's files, where its cfg doesn't name one (no XRSound.cfg)
	struct Default { const char *key, *file; };
	const Default defaults[] = {
		{"OneHundredKnots", "XRSound/Default/100 Knots.wav"},
		{"Liftoff", "XRSound/Default/Liftoff.wav"},
		{"Touchdown", "XRSound/Default/Touchdown.wav"},
		{"WheelStop", "XRSound/Default/Wheel Stop.wav"},
		{"WarningGearIsUp", "XRSound/Default/Warning Gear is Up.wav"},
		{"YouAreClearedToLand", "XRSound/Default/You Are Cleared to Land.wav"},
		{"DockingCallout", "XRSound/Default/Contact.wav"},
		{"UndockingCallout", "XRSound/Default/Undocking Confirmed.wav"},
		{"DockingRadarBeep", "XRSound/Default/Docking Radar Beep.wav"},
		{"AutopilotOn", "XRSound/Default/Autopilot On.wav"},
		{"AutopilotOff", "XRSound/Default/Autopilot Off.wav"},
		{"SubsonicCallout", "XRSound/Default/subsonic.wav"},
		{"RCSRotation", "XRSound/Default/Rotation.wav"},
		{"RCSTranslation", "XRSound/Default/Translation.wav"},
		{"RCSOff", "XRSound/Default/Off.wav"},
		{"AFPitch", "XRSound/Default/Pitch.wav"},
		{"AFOn", "XRSound/Default/On.wav"},
		{"AFOff", "XRSound/Default/Off.wav"},
		// (XRSound has these lines, not as default sounds: the cfgs may name
		// them all the same)
		{"WarningMainFuelLow", "XRSound/Default/Warning Main Fuel Low.wav"},
		{"WarningMainFuelDepleted", "XRSound/Default/Warning Main Fuel Depleted.wav"},
		{"WarningRCSFuelLow", "XRSound/Default/Warning RCS Fuel Low.wav"},
		{"WarningRCSFuelDepleted", "XRSound/Default/Warning RCS Fuel Depleted.wav"},
		{"WarningDamage", "XRSound/Default/Warning Ship Damage Detected.wav"},
		{"WarningHeatDamage", "XRSound/Default/Warning Heat Damage.wav"},
		// the new lines (Piper)
		{"Ignition", "Sound/Voice/Ignition.flac"},
		{"MaxQ", "Sound/Voice/Max Q.flac"},
		{"MECO", "Sound/Voice/MECO.flac"},
		{"Orbit", "Sound/Voice/Orbit.flac"},
		{"Splashdown", "Sound/Voice/Splashdown.flac"},
	};

	const int heights[] = { 5000, 4000, 3000, 2000, 1000, 900, 800, 700, 600, 500, 400, 300, 200, 100,
		75, 50, 40, 30, 20, 15, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };

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

std::string Callouts::Line (const char *key) const
{
	std::string own;
	if (xr::LineFor (vessel, key, own)) return own; // (the vessel's code's: XRSound)
	if (cfg) {
		std::string lk (key);
		for (auto &c : lk) c = (char)std::tolower ((unsigned char)c);
		auto it = cfg->files.find (lk);
		if (it != cfg->files.end ()) return cfg->File (key); // (named: that, or none)
	}
	for (auto &d : defaults) if (!strcmp (d.key, key)) return d.file;
	return std::string ();
}

std::string Callouts::GroupLine (const char *group, const std::string &base) const
{
	std::string g (group);
	for (auto &c : g) c = (char)std::tolower ((unsigned char)c);
	std::string dir;
	if (xr::LineFor (vessel, group, dir)) { if (dir.empty ()) return std::string (); }
	else {
		dir = cfg ? cfg->Group (group) : std::string ();
		if (dir.empty () && cfg && cfg->groups.count (g)) return std::string (); // (switched off)
	}
	if (dir.empty ()) dir = "XRSound/Default";
	for (const char *ext : { ".wav", ".ogg", ".flac", ".mp3" }) {
		std::string f = dir + "/" + base + ext;
		if (!System::FindSound (f).empty ()) return f;
	}
	return std::string ();
}

double Callouts::GearState (const Vessel *v) const
{
	if (!v) return -1.0;
	int a = sys.GearAnim (v);
	if (a < 0) a = XrConfig::For (v->ClassName () ? v->ClassName () : "").GearAnim ();
	if (a < 0) return -1.0;
	VESSEL *vi = Mutable (v)->GetModuleInterface ();
	return vi ? vi->GetAnimation ((UINT)a) : -1.0;
}

void Callouts::Reset ()
{
	if (speaking) sys.StopSound (speaking);
	if (beep) sys.StopSound (beep);
	speaking = beep = 0;
	queue.clear ();
	st = State ();
	vessel = nullptr;
	cfg = nullptr;
}

bool Callouts::Say (const std::string &f, Prio prio, double maxAge)
{
	if (f.empty () || muted) return false;
	std::string file (f);
	std::replace (file.begin (), file.end (), '\\', '/');
	// (the same line waiting already: once)
	for (auto &q : queue) if (q.file == file) { q.until = t + maxAge; return true; }
	Queued q{file, prio, t + maxAge};
	// warnings go before the others waiting
	auto it = queue.begin ();
	while (it != queue.end () && it->prio >= prio) ++it;
	queue.insert (it, q);
	return true;
}

void Callouts::Speak (double simt)
{
	if (speaking && !sys.Playing (speaking)) speaking = 0;
	while (!queue.empty () && queue.front ().until < simt) queue.pop_front (); // (too late now)
	if (speaking || queue.empty ()) return;
	Queued q = queue.front ();
	queue.pop_front ();
	speaking = sys.PlayOn (VOICE, q.file, 1.0, false);
	if (!speaking) return;
	said++;
	Note (q.file, simt);
}

void Callouts::Note (const std::string &f, double simt)
{
	std::string file (f);
	std::replace (file.begin (), file.end (), '\\', '/');
	recent.push_front (file);
	if (recent.size () > 64) recent.pop_back ();
	LOGOUT ("Audio: callout %s at %.2f", file.c_str (), simt);
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
	if (log) printf ("Audio: callout %s at %.2f\n", file.c_str (), simt), fflush (stdout);
}

void Callouts::Radio (int e)
{
	sys.GetRadio ().OnEvent ((audio::Radio::Event)e);
}

void Callouts::OnEvent (const Vessel *v, Event e)
{
	if (!v || v != vessel) return;
	switch (e) {
	case EV_DOCKED:      Say (Line ("DockingCallout")); st.undockAt = -1.0; Radio (audio::Radio::DOCKED); break;
	case EV_UNDOCKED:    st.undockAt = t + 0.667; Radio (audio::Radio::UNDOCKED); break; // ("undocking confirmed" a moment later)
	case EV_SPLASHDOWN:  Say (Line ("Splashdown"), WARNING, 4.0); st.ascent = false; Radio (audio::Radio::SPLASHDOWN); break;
	case EV_PART_LOST:   Say (Line ("WarningDamage"), WARNING, 4.0); break;
	case EV_HEAT_DAMAGE: Say (Line ("WarningHeatDamage"), WARNING, 4.0); break;
	}
}

void Callouts::Step (const Vessel *focus, double simt, double simdt, double warp)
{
	if (focus != vessel) {
		Reset ();
		vessel = focus;
		cfg = focus ? &XrConfig::For (focus->ClassName () ? focus->ClassName () : "") : nullptr;
	}
	t = simt;
	if (!vessel || vessel->Destroyed ()) { queue.clear (); return; }
	VESSEL *v = Mutable (vessel)->GetModuleInterface ();
	if (!v) return;
	// (time acceleration: nothing called, the state kept up)
	const bool quiet = warp > 10.0;
	if (quiet) queue.clear ();

	if (!quiet || st.first) {
		Flight (v, simt);
		Docking (v, simt);
		Modes (v);
		Ascent (v, simt);
		Fuel (v);
	}
	if (quiet) { st.first = true; return; }
	st.first = false;
	if (st.undockAt >= 0.0 && simt >= st.undockAt) { Say (Line ("UndockingCallout")); st.undockAt = -1.0; }
	Speak (simt);
}

void Callouts::Flight (VESSEL *v, double simt)
{
	// (after XRSound's TakeoffAndLandingCallouts, AltitudeCallouts, MachCallouts)
	const double alt = LowestAltitude (v);
	const double airspeed = v->GetAirspeed (), ground = v->GetGroundspeed ();
	VECTOR3 as;
	v->GetAirspeedVector (FRAME_HORIZON, as);
	const double vs = as.y; // (up: positive)
	// on the ground: touching it, landed; or (not flying yet) standing
	// still, as a rocket held on its pad
	// (a vessel "flying" standing still, its engines off, for a second: held -
	// on a pad, a ground it doesn't touch: on the ground, quietly; hovering
	// takes thrust)
	const bool held = ground < 0.1 && std::fabs (vs) < 0.05;
	if (held && st.flying && v->GetThrusterGroupLevel (THGROUP_MAIN) <= 0.0 && v->GetThrusterGroupLevel (THGROUP_HOVER) <= 0.0
		&& v->GetThrusterGroupLevel (THGROUP_RETRO) <= 0.0) {
		if (st.heldSince < 0.0) st.heldSince = simt;
		else if (simt - st.heldSince >= 1.0) st.flying = false, st.rolledOut = true, st.airborneAt = 0.0;
	} else st.heldSince = -1.0;
	const bool contact = v->GroundContact () || (v->GetFlightStatus () & 1) || alt <= 0.0 || (held && !st.flying);
	const double gear = GearState (vessel);   // (< 0: not known)
	const bool gearDown = gear >= 0.999;
	const bool atm = v->GetAtmPressure () > ATM_P;
	const double mach = atm ? v->GetMachNumber () : 0.0;

	if (st.first) {
		st.prevAlt = alt, st.prevVs = vs, st.prevAirspeed = airspeed, st.prevMach = mach;
		st.flying = !contact;
		st.rolledOut = !st.flying && ground < 0.5;
		st.airborneAt = st.flying ? simt - AIRBORNE_AFTER : 0.0;
		return;
	}

	if (contact) {
		st.airborneAt = 0.0;
		if (st.flying) {
			// touchdown (a hard one: the crunch says it, no voice)
			double sink = std::max (-vs, -st.prevVs);
			st.flying = false;
			st.rolledOut = false;
			st.ascent = false;
			if (sink < 4.0 && (gear < 0.0 || gearDown)) Say (Line ("Touchdown"));
		} else {
			// rolling: "100 knots" through it either way; at a stop, "wheel stop"
			if (st.prevAirspeed >= 0.0 && ((airspeed >= KNOTS100) != (st.prevAirspeed >= KNOTS100)) && (gear < 0.0 || gearDown))
				Say (Line ("OneHundredKnots"), NORMAL, 1.0);
			if (!st.rolledOut && ground < 0.2) {
				st.rolledOut = true;
				if (gearDown) Say (Line ("WheelStop"));
				if (st.flewHigh) Radio (audio::Radio::LANDED); // (home from a flight to space)
				st.flewHigh = false;
			}
		}
	} else if (!st.flying) {
		// off the ground: flying after a moment (not a bounce)
		if (st.airborneAt == 0.0) st.airborneAt = simt;
		else if (simt - st.airborneAt >= AIRBORNE_AFTER) {
			st.flying = true;
			st.rolledOut = true;
			Say (Line ("Liftoff"));
			Radio (audio::Radio::LIFTOFF);
			st.ascent = true, st.maxqSaid = st.mecoSaid = st.orbitSaid = false, st.qMax = 0.0, st.mainOffAt = -1.0;
		}
	} else {
		// descending: the gear, clearance, the height above the ground
		const double gearAlt = cfg ? cfg->Number ("WarningGearIsUpAltitude", 275.0) : 275.0;
		if (gear >= 0.0 && !gearDown && alt < gearAlt && alt < st.prevAlt && vs <= -0.25 && alt > 0.0 && simt >= st.nextGearWarn) {
			if (Say (Line ("WarningGearIsUp"), WARNING, 2.5)) st.nextGearWarn = simt + 4.0;
		} else if (simt >= st.nextAlt && vs < 0.0) {
			if (atm && gear >= 0.0 && st.prevAlt > CLEARED_ALT && alt <= CLEARED_ALT && vs > -150.0) {
				if (Say (Line ("YouAreClearedToLand"))) st.nextAlt = simt + MIN_GAP;
			} else if (alt <= heights[0]) {
				for (int h : heights) {
					if (st.prevAlt > h && alt <= h) {
						if (Say (GroupLine ("AltitudeCalloutsGroup", std::to_string (h)), NORMAL, 0.8)) st.nextAlt = simt + MIN_GAP;
						break;
					}
				}
			}
		}
	}

	// Mach: through Mach 1 up, subsonic down, each whole number either way
	if (atm && st.prevMach > 0.0 && mach > 0.0 && !contact) {
		if (st.prevMach >= 1.0 && mach < 1.0) Say (Line ("SubsonicCallout"), NORMAL, 1.5);
		else if (st.prevMach < 1.0 && mach >= 1.0) Say (GroupLine ("MachCalloutsGroup", "Mach 1"), NORMAL, 1.5);
		else if (st.prevMach < 27.0 && mach >= 27.0) Say (GroupLine ("MachCalloutsGroup", "Mach 27 Plus"), NORMAL, 1.5);
		else {
			for (int m = 2; m < 27; m++) {
				if ((st.prevMach < m && mach >= m) || (st.prevMach > m && mach <= m)) {
					Say (GroupLine ("MachCalloutsGroup", "Mach " + std::to_string (m)), NORMAL, 1.5);
					break;
				}
			}
		}
	}

	st.prevAlt = alt, st.prevVs = vs, st.prevAirspeed = airspeed, st.prevMach = mach;
}

void Callouts::Docking (VESSEL *v, double simt)
{
	// (after XRSound's DockingCallouts and DockingRadar: the distance from
	// our first port to the nearest IDS signal tuned)
	double dist = -1.0;
	bool docked = false;
	if (DOCKHANDLE hd = v->GetDockHandle (0)) {
		docked = v->DockingStatus (0) != 0;
		VECTOR3 pos, dir, rot, gpos;
		v->GetDockParams (hd, pos, dir, rot);
		v->Local2Global (pos, gpos);
		for (DWORD i = 0; i < 4 && !docked; i++) {
			NAVHANDLE hn = v->GetNavSource (i);
			if (!hn) continue;
			NAVDATA nd;
			oapiGetNavData (hn, &nd);
			if (nd.type != TRANSMITTER_IDS) continue;
			VECTOR3 np;
			oapiGetNavPos (hn, &np);
			double d = length (gpos - np);
			if (dist < 0.0 || d < dist) dist = d;
		}
	}
	if (st.first || docked) { st.prevDist = docked ? -1.0 : dist; if (beep) sys.StopSound (beep), beep = 0; return; }
	if (dist >= 0.0 && st.prevDist >= 0.0 && simt >= st.nextDist && dist <= heights[0]) {
		for (int h : heights) {
			if (st.prevDist > h && dist <= h) {
				if (Say (GroupLine ("DockingDistanceCalloutsGroup", std::to_string (h)), NORMAL, 0.8)) st.nextDist = simt + MIN_GAP;
				break;
			}
		}
	}
	// the radar's beep: faster as the port nears (every 4 s at 500 m, 0.5 s close)
	if (dist > 0.0 && dist <= BEEP_RANGE && !muted) {
		if (simt >= st.nextBeep) {
			if (beep) sys.StopSound (beep);
			std::string f = Line ("DockingRadarBeep");
			beep = f.empty () ? 0 : sys.PlayOn (VOICE, f, 0.4, false, false); // (a beep, not speech)
			st.nextBeep = simt + 0.5 + 3.5 * (dist / BEEP_RANGE);
		}
	} else {
		if (beep) sys.StopSound (beep), beep = 0;
		st.nextBeep = 0.0;
	}
	st.prevDist = dist;
}

void Callouts::Modes (VESSEL *v)
{
	int nav = 0;
	for (int i = 2; i <= 7; i++) if (v->GetNavmodeState (i)) { nav = i; break; }
	const int rcs = v->GetAttitudeMode ();
	const int af = (int)std::min (v->GetADCtrlMode (), (DWORD)2);
	if (!st.first) {
		if ((nav != 0) != (st.prevNav != 0) || (nav && nav != st.prevNav))
			Say (Line (nav ? "AutopilotOn" : "AutopilotOff"), NORMAL, 1.0);
		if (rcs != st.prevRcs)
			Say (Line (rcs == 1 ? "RCSRotation" : rcs == 2 ? "RCSTranslation" : "RCSOff"), NORMAL, 1.0);
		if (af != st.prevAf)
			Say (Line (af == 0 ? "AFOff" : af == 1 ? "AFPitch" : "AFOn"), NORMAL, 1.0);
	}
	st.prevNav = nav, st.prevRcs = rcs, st.prevAf = af;
}

void Callouts::Ascent (VESSEL *v, double simt)
{
	const double main = v->GetThrusterGroupLevel (THGROUP_MAIN);
	const bool contact = !st.flying; // (Flight: on the ground, or held on a pad)
	if (st.first) { st.prevMain = main; return; }

	// ignition: the main engines lit on the ground, pointing up (a rocket on
	// its pad, not a plane on its runway)
	if (contact && st.prevMain <= 0.0 && main > 0.0 && v->GetGroupThrusterCount (THGROUP_MAIN) > 0) {
		VECTOR3 dir, up;
		v->GetThrusterDir (v->GetGroupThruster (THGROUP_MAIN, 0), dir);
		v->HorizonInvRot (_V(0, 1, 0), up);
		if (dotp (dir, up) > 0.85) Say (Line ("Ignition"), NORMAL, 1.5);
	}

	if (st.ascent) {
		// Max Q: the dynamic pressure past its peak (5 % down from it), in a
		// powered climb (not a plane slowing down)
		VECTOR3 as;
		v->GetAirspeedVector (FRAME_HORIZON, as);
		const bool climbing = main > 0.0 && as.y > 0.0;
		const double q = v->GetDynPressure ();
		if (!climbing) st.qMax = 0.0;
		else if (q > st.qMax) st.qMax = q;
		if (!st.maxqSaid && climbing && st.qMax > MAXQ_MIN && q < 0.95 * st.qMax) { Say (Line ("MaxQ"), NORMAL, 3.0); st.maxqSaid = true; }
		// the periapsis above the atmosphere (or the ground): in orbit
		double pe = 0.0;
		OBJHANDLE ref = v->GetPeDist (pe);
		bool orbit = false;
		if (ref) {
			// (above the sensible atmosphere: 14 scale heights, a millionth of
			// the ground's pressure - about 120 km on Earth - not its model's
			// limit, the thermosphere's top; airless: above the mountains)
			const double R = oapiGetSize (ref);
			double top = 10e3;
			const ATMCONST *ac = oapiGetPlanetAtmConstants (ref);
			if (ac && ac->p0 > 0.0 && ac->rho0 > 0.0) {
				const double g = GGRAV * oapiGetMass (ref) / (R * R);
				top = std::min (ac->altlimit, 14.0 * ac->p0 / (ac->rho0 * g));
			}
			orbit = pe > R + top;
		}
		// MECO: the main engines off (half a second) on the way up: off the
		// ground, above 10 km or out of the atmosphere
		if (main > 0.0) st.mainOffAt = -1.0;
		else if (st.prevMain > 0.0) st.mainOffAt = simt;
		if (!st.mecoSaid && st.mainOffAt >= 0.0 && simt - st.mainOffAt >= 0.5 && (v->GetAltitude () > 10e3 || v->GetAtmPressure () < ATM_P)) {
			Say (Line ("MECO"), NORMAL, 3.0);
			st.mecoSaid = true;
			Radio (audio::Radio::MECO);
		}
		if (!st.orbitSaid && orbit) {
			Say (Line ("Orbit"), NORMAL, 4.0);
			st.orbitSaid = true;
			Radio (audio::Radio::ORBIT);
			st.flewHigh = true;
			st.ascent = false;
		}
	}
	st.prevMain = main;
}

void Callouts::Fuel (VESSEL *v)
{
	PROPELLANT_HANDLE mainTank = nullptr, rcsTank = nullptr;
	if (v->GetGroupThrusterCount (THGROUP_MAIN)) mainTank = v->GetThrusterResource (v->GetGroupThruster (THGROUP_MAIN, 0));
	for (int g = THGROUP_ATT_PITCHUP; g <= THGROUP_ATT_BACK && !rcsTank; g++)
		if (v->GetGroupThrusterCount ((THGROUP_TYPE)g)) rcsTank = v->GetThrusterResource (v->GetGroupThruster ((THGROUP_TYPE)g, 0));
	if (rcsTank == mainTank) rcsTank = nullptr; // (one tank: the main's warnings)
	const double fm = FuelLeft (v, mainTank), fr = FuelLeft (v, rcsTank);
	if (!st.first) {
		if (fm >= 0.0 && st.prevMainFuel >= 0.0) {
			if (st.prevMainFuel > 0.0 && fm <= 0.0) Say (Line ("WarningMainFuelDepleted"), WARNING, 4.0);
			else if (st.prevMainFuel >= LOW_FUEL && fm < LOW_FUEL) Say (Line ("WarningMainFuelLow"), WARNING, 4.0);
		}
		if (fr >= 0.0 && st.prevRcsFuel >= 0.0) {
			if (st.prevRcsFuel > 0.0 && fr <= 0.0) Say (Line ("WarningRCSFuelDepleted"), WARNING, 4.0);
			else if (st.prevRcsFuel >= LOW_FUEL && fr < LOW_FUEL) Say (Line ("WarningRCSFuelLow"), WARNING, 4.0);
		}
	}
	st.prevMainFuel = fm, st.prevRcsFuel = fr;
}

} // namespace audio
