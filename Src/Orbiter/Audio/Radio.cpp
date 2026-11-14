// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Radio.h"
#include "DebugEnv.h"
#include "Loudness.h"
#include "Assets.h"
#include "AudioSystem.h"
#include "XrConfig.h"
#include "Ears.h"
#include "Vessel.h"
#include "Planet.h"
#include "Psys.h"
#include "Camera.h"
#include "Log.h"
#include "TimeData.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern Vessel *g_focusobj;
extern TimeData td;
extern PlanetarySystem *g_psys;

namespace audio {

namespace {
	constexpr double C_LIGHT = 299792458.0;
	/// A blackout: faster than this in air of this dynamic pressure
	constexpr double BLACKOUT_V = 5000.0, BLACKOUT_Q = 300.0;
	/// Entry interface: 120 km, coming down faster than 6 km/s (Earth's; each body's from its atmosphere)
	constexpr double ENTRY_ALT = 120e3, ENTRY_V = 6000.0;

	/// Entry and blackout by the body: the interface at its sensible atmosphere's top (14 scale
	/// heights, as the callouts' orbit), faster than 0.8 of the circular speed there; the blackout
	/// faster than 0.6 of the circular speed at the ground (Earth: 118 km, 6.3 km/s; 4.7 km/s)
	struct Limits { bool atm = true; double top = ENTRY_ALT, vEntry = ENTRY_V, vBlackout = BLACKOUT_V; };
	Limits LimitsFor (const CelestialBody *ref)
	{
		Limits L;
		if (!ref) return L;
		const ATMCONST *ac = oapiGetPlanetAtmConstants ((OBJHANDLE)ref);
		if (!ac || ac->p0 <= 0.0 || ac->rho0 <= 0.0) { L.atm = false; return L; }
		const double R = ref->Size (), GM = GGRAV * ref->Mass ();
		L.top = std::min (ac->altlimit, 14.0 * ac->p0 / (ac->rho0 * GM / (R * R)));
		L.vEntry = 0.8 * std::sqrt (GM / (R + L.top));
		L.vBlackout = 0.6 * std::sqrt (GM / R);
		return L;
	}
	const char *CHATTER_DIR = "XRSound/Default/Altea STS";
	const char *AOS_CLIP = "XRSound/Default/Altea STS/we have you back on air-to-ground_two.wav"; // (no AOS takes: NASA's)
	/// Each event's file name in Sound/Voice/Ground (its takes <name>_<n>.flac)
	const char *EVENT_NAME[Radio::NEVENT] = { "Liftoff", "MECO", "Orbit", "Docked", "Undocked", "Splashdown", "Landed",
		"MaxQ", "Ignition", "Deorbit", "Surface", "Capture", "Escape", "Descent", "Approach", "BoosterSep", "TankSep", "DeepSpace", "Reentry", "AOS", "LOS", "Lost" };

	/// A sound file decoded: mono, at a rate (Assets: remembered)
	ClipRef Decode (const std::string &file, unsigned rate)
	{
		const std::string path = System::FindSound (file);
		return path.empty () ? nullptr : Assets::Get ().Load (path, rate, 1);
	}
}

// ---- the radio --------------------------------------------------------------

Radio::Radio (System &s): sys (s)
{
	std::fill (std::begin (lastTake), std::end (lastTake), -1);
}

Radio::~Radio ()
{
	Clear ();
}

double Radio::Snr (double d)
{
	return RadioSynth::Snr (d);
}

RadioSynth::Sound Radio::SoundFor (const std::string &file)
{
	return RadioSynth::SoundFor (file);
}

bool Radio::Init ()
{
	Engine &e = sys.GetEngine ();
	if (!e.Active ()) return false;
	node = std::make_unique<PathNode>();
	if (!node->Init (e.Ma (), 0.0f)) { node.reset (); return false; }
	ma_node_attach_output_bus (node->Node (), PathNode::AIR, e.Group (RADIO), 0);
	PathNode::Target tg;
	tg.gain = 1.0f;
	node->Set (PathNode::AIR, tg);
	return true;
}

void Radio::Clear ()
{
	playing.clear ();
	if (node) node->Uninit (), node.reset ();
	replies.clear ();
	nextChatter = -1.0, blackoutSince = -1.0, losDue = -1.0, losReady = 0.0, quietSince = 0.0;
	lastRef = nullptr;
	wasBusy = false;
	blackout = entrySaid = false;
	vessel = nullptr;
	vref = BodyRef ();
	std::fill (std::begin (lastTake), std::end (lastTake), -1);
}

bool Radio::Busy () const
{
	for (auto &p : playing) if (!p.s->Done ()) return true;
	return false;
}

void Radio::Prefetch (const std::string &file)
{
	const std::string path = System::FindSound (file);
	if (!path.empty () && sys.GetEngine ().Active ()) Assets::Get ().Prefetch (path, sys.GetEngine ().SampleRate (), 1);
}

bool Radio::Transmit (const std::string &file, bool ground)
{
	if (!node && !Init ()) return false;
	Engine &e = sys.GetEngine ();
	ClipRef clip = Decode (file, e.SampleRate ());
	if (!clip) { LOGOUT ("Audio: radio: no file %s", file.c_str ()); return false; }
	auto s = RadioSynth::Make (e, std::vector<float> (clip->pcm), ground, (float)Snr (earthDist), (uint32_t)(Rand () * 1e9), SoundFor (file), clip->lufs);
	if (!s || !s->Attach (node->Node ())) return false;
	s->Start ();
	playing.push_back ({std::move (s)});
	played++;
	LOGOUT ("Audio: radio: %s (S/N %.0f dB)", file.c_str (), Snr (earthDist));
	const bool log = Dbg ().log > 0;
	if (log) printf ("Audio: radio at %.2f s: %s\n", t, file.c_str ()), fflush (stdout);
	return true;
}

double Radio::ReplyGap ()
{
	// (lognormal, median 0.55 s, sigma 0.45: Box-Muller from two uniforms)
	const double u1 = std::max (1e-9, Rand ()), u2 = Rand ();
	const double n = std::sqrt (-2.0 * std::log (u1)) * std::cos (6.283185307179586 * u2);
	return std::clamp (0.55 * std::exp (0.45 * n), 0.25, 1.3);
}

const char *Radio::OnShift ()
{
	if (Dbg ().capcom) return Dbg ().capcom == 1 ? "c1" : "c2";   // (ORBITER_AUDIO_CAPCOM)
	// (three shifts a day from 06:00 UTC, the CAPCOMs taking turns)
	const long shift = (long)std::floor ((td.MJD0 * 24.0 - 6.0) / 8.0);
	return (shift & 1) ? "c2" : "c1";
}

bool Radio::SaysHouston (const std::string &file)
{
	if (!houstonRead) {
		houstonRead = true;
		const std::string path = System::FindSound ("Sound/Voice/Ground/lines.txt");
		if (FILE *f = path.empty () ? nullptr : fopen (path.c_str (), "r")) {
			char line[512];
			while (fgets (line, sizeof (line), f)) {
				const char *bar = strchr (line, '|');
				if (line[0] != '#' && bar && strstr (bar, "Houston")) houston.emplace_back (line, bar - line);
			}
			fclose (f);
		}
	}
	for (const auto &h : houston)
		if (file.size () >= h.size () + 5 && file.compare (file.size () - h.size () - 5, h.size () + 5, h + ".flac") == 0) return true;
	return false;
}

std::string Radio::Take (Event e)
{
	// the event's takes by the CAPCOM on shift: <name>_<c1|c2>_<n> (else the other's; else
	// <name>_<n>, <name>.flac); one at random, not the last one again; AOS: NASA's clip if none
	const std::string dir = std::string ("Sound/Voice/Ground/") + EVENT_NAME[e];
	const char *shift = OnShift ();
	std::vector<std::string> takes;
	// (beyond the Earth and the Moon, no "Houston": a Mars base isn't talked to as from Texas)
	const CelestialBody *ref = vessel ? vessel->ElRef () : lastRef;
	const bool home = !ref || !strcmp (ref->Name (), "Earth") || !strcmp (ref->Name (), "Moon");
	for (const std::string &tag : { std::string ("_") + shift + "_", std::string (strcmp (shift, "c1") ? "_c1_" : "_c2_"), std::string ("_") }) {
		for (int n = 1; n <= 9; n++) {
			const std::string f = dir + tag + std::to_string (n) + ".flac";
			if (!System::FindSound (f).empty () && (home || !SaysHouston (f))) takes.push_back (f);
		}
		if (!takes.empty ()) break;
	}
	if (takes.empty ()) return e == AOS ? AOS_CLIP : dir + ".flac";
	int k = (int)((size_t)(Rand () * takes.size ()) % takes.size ());
	if (takes.size () > 1 && k == lastTake[e]) k = (k + 1) % (int)takes.size ();
	lastTake[e] = k;
	return takes[k];
}

void Radio::Reply (Event e)
{
	// (heard after the light's round trip to Earth, and a pause once the crew's callout is over)
	const double rtt = 2.0 * earthDist / C_LIGHT, gap = ReplyGap ();
	const std::string f = Take (e);
	replies.push_back ({f, t + rtt, true, gap, pt + gap});
	Prefetch (replies.back ().file);
}

void Radio::OnEvent (Event e)
{
	if (e < 0 || e >= NEVENT || !vessel) return;
	Reply (e);
}

void Radio::OnFlight (const std::vector<FlightEvent> &events)
{
	using E = FlightEvent;
	for (const FlightEvent &e : events) {
		switch (e.kind) {
		case E::SURFACE:   OnEvent (SURFACE); break;
		case E::LANDED:    OnEvent (LANDED); break;
		case E::LIFTOFF:   if (e.value > 0.5) OnEvent (LIFTOFF); break;   // (a rocket's, not a plane's)
		case E::APPROACH:  OnEvent (APPROACH); break;
		case E::IGNITION:  OnEvent (IGNITION); break;
		case E::DEORBIT:   OnEvent (DEORBIT); break;
		case E::CAPTURE:   OnEvent (CAPTURE); break;
		case E::ESCAPE:    OnEvent (ESCAPE); break;
		case E::DESCENT:   OnEvent (DESCENT); break;
		case E::MAXQ:      OnEvent (MAXQ); break;
		case E::MECO:      OnEvent (MECO); break;
		case E::ORBIT:     OnEvent (ORBIT); break;
		case E::DEEPSPACE: OnEvent (DEEPSPACE); break;
		default: break;
		}
	}
}

void Radio::Update (double tnow, double playNow)
{
	t = tnow, pt = playNow;
	// (the transmissions over: gone)
	playing.erase (std::remove_if (playing.begin (), playing.end (), [](const Playing &p) { return p.s->Done (); }), playing.end ());
	const bool busy = Busy ();
	if (wasBusy && !busy) quietSince = pt;
	wasBusy = busy;
	// the vessel in focus destroyed: the ground loses its signal (a moment of silence, then says so)
	if (vessel && vref == sys.GetWorld ().Known (g_focusobj) && g_focusobj->Destroyed () && !blackout) {
		for (auto &p : playing) p.s->Cut ();
		losDue = t + 2.0 * earthDist / C_LIGHT, losReady = pt + 2.5 + Rand ();
	}
	const Vessel *v = g_focusobj && !g_focusobj->Destroyed () ? g_focusobj : nullptr;
	const BodyRef vr = sys.GetWorld ().Ref (v);
	if (vr != vref) { replies.clear (); nextChatter = -1.0; entrySaid = false; blackout = false; vessel = v; vref = vr; }
	if (v) {
		losDue = -1.0, lastRef = v->ElRef ();
		const SurfParam *s = v->GetSurfParam ();
		lastAlt = s ? s->alt : 1e9;
	}
	else if (losDue >= 0.0 && t >= losDue && pt >= losReady && !Busy ()) {
		losDue = -1.0;
		Transmit (Take (lastAlt < 10e3 ? LOST : LOS), true);   // (near the ground: the vehicle lost, not the signal)
	}
	const CelestialBody *earth = g_psys ? g_psys->GetGravObj ("Earth", true) : nullptr;
	if (!v || !earth) return;
	earthDist = std::max (0.0, (v->GPos () - earth->GPos ()).length () - earth->Size ());

	// the blackout: the plasma about a vessel entering fast
	const SurfParam *sp = v->GetSurfParam ();
	const double U = sp ? sp->airspd : 0.0, q = sp ? sp->dynp : 0.0, alt = sp ? sp->alt : 1e9;
	const double vs = sp ? sp->vspd : 0.0;
	const Limits L = LimitsFor (sp ? sp->ref : nullptr);
	const bool bo = sp && sp->is_in_atm && L.atm && U > L.vBlackout && q > BLACKOUT_Q;
	if (bo && !blackout) {
		blackout = true, blackoutSince = t;
		for (auto &p : playing) p.s->Cut ();
		LOGOUT ("Audio: radio blackout at %.1f s (%.0f m/s, %.0f Pa)", t, U, q);
		sys.GetCallouts ().Crew ("CommBlackout");
		const bool log = Dbg ().log > 0;
		if (log) printf ("Audio: radio blackout at %.2f s\n", t), fflush (stdout);
	} else if (!bo && blackout) {
		blackout = false;
		// (through it: the ground has us back)
		if (t - blackoutSince > 5.0) {
			std::string f = Take (AOS);
			const double react = REACT + 2.0 * Rand ();
			replies.push_back ({f, t + 2.0 * earthDist / C_LIGHT, f != AOS_CLIP, 0.0, pt + react});
			Prefetch (f);
		}
		LOGOUT ("Audio: radio blackout over at %.1f s", t);
	}
	// entry interface: coming down through 120 km fast (once per descent)
	if (sp && L.atm && alt < L.top && alt > 0.8 * L.top && U > L.vEntry && vs < 0.0 && !entrySaid) {
		entrySaid = true;
		sys.GetCallouts ().Crew ("EntryInterface");
		Reply (ENTRY);
	}
	if (alt > 2.0 * L.top) entrySaid = false;
	if (blackout) return;

	// the ground's replies first (dropped if a minute late: a blackout); a reply waits for the
	// crew's callout to be over, then its pause
	while (!replies.empty () && replies.front ().due < t - 60.0) replies.pop_front ();
	if (sys.Speaking ())
		for (auto &r : replies) if (r.ground && r.gap > 0.0) r.ready = std::max (r.ready, pt + r.gap);
	// (and after a transmission, the same pause: two replies due together aren't back to back)
	if (!replies.empty () && replies.front ().due <= t && replies.front ().ready <= pt && !Busy () && pt - quietSince >= replies.front ().gap) {
		Pending r = replies.front ();
		replies.pop_front ();
		Transmit (r.file, r.ground);
		return;
	}

	// the chatter: near Earth, now and then, as XRSound's [ATC] settings allow
	const XrConfig &cfg = XrConfig::For (v->ClassName () ? v->ClassName () : "");
	auto atc = [&cfg](const char *k, double def) {
		auto it = cfg.atc.find (k);
		return it == cfg.atc.end () || it->second.empty () ? def : atof (it->second.c_str ());
	};
	const bool landed = v->GroundContact ();
	const bool inAtm = sp && sp->is_in_atm && q > 1.0;
	const bool reentry = inAtm && U > 1500.0 && vs < 0.0 && alt > 20e3;
	bool allowed = earthDist < 2e9 && (!landed || atc ("allowwhilelanded", 1) != 0.0) && (!inAtm || atc ("allowinatmosphere", 1) != 0.0)
		&& (!reentry || atc ("allowduringreentry", 0) != 0.0);
	if (nextChatter < 0.0) nextChatter = pt + atc ("mindelay", 15.0) + Rand () * (atc ("maxdelay", 120.0) - atc ("mindelay", 15.0));
	if (pt >= nextChatter) {
		const double far = earthDist > atc ("delayplanetdistance", 400.0) * 1e3 ? atc ("delayplanetmultiplier", 5.0) : 1.0;
		nextChatter = pt + far * (atc ("mindelay", 15.0) + Rand () * (atc ("maxdelay", 120.0) - atc ("mindelay", 15.0)));
		if (allowed && !Busy () && replies.empty ()) {
			if (bag.empty ()) {
				// (a shuffled bag: each clip once before any again; the AOS call
				// kept for a blackout's end)
				std::error_code ec;
				std::string dir = System::FindSound (CHATTER_DIR);
				if (!dir.empty ())
					for (auto &de : std::filesystem::directory_iterator (dir, ec))
						if (de.path ().extension () == ".wav" && de.path ().filename ().string ().find ("we have you back") == std::string::npos)
							bag.push_back (std::string (CHATTER_DIR) + "/" + de.path ().filename ().string ());
				for (size_t i = bag.size (); i > 1; i--) std::swap (bag[i - 1], bag[(size_t)(Rand () * i) % i]);
			}
			if (!bag.empty ()) {
				std::string f = bag.back ();
				bag.pop_back ();
				Transmit (f, false);
				if (!bag.empty ()) Prefetch (bag.back ()); // (the next: decoded by its turn)
			}
		}
	}
}

} // namespace audio
