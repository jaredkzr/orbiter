// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Ambience.h"
#include "AmbienceSynth.h"
#include "AudioSystem.h"
#include "Ears.h"
#include "Medium.h"
#include "Camera.h"
#include "Vessel.h"
#include "Planet.h"
#include "Log.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern Camera *g_camera;
extern Vessel *g_focusobj;

namespace audio {

namespace {
	const char *CABIN_FILE = "XRSound/Default/Air Conditioning.wav";
	/// The recordings' RMS (measured), to set their levels in pascals
	constexpr double CABIN_RMS = 0.01396;    // (-37.1 dBFS)
	constexpr double EVENT_RMS = 0.06;       // (the cabin ambience's, about -24 dBFS)
	/// From a cockpit, the outside: as the world muffles the air's sound
	constexpr float INSIDE_GAIN = 0.1f, INSIDE_CUTOFF = 700.0f;
	/// Outdoors at rest: near the ground, not flying past
	constexpr double OUT_ALT = 2000.0, OUT_SPEED = 30.0;
}

Ambience::Ambience (System &s): sys (s)
{
}

Ambience::~Ambience ()
{
	Clear ();
}

bool Ambience::Init ()
{
	Engine &e = sys.GetEngine ();
	if (!e.Active ()) return false;
	node = std::make_unique<PathNode>();
	if (!node->Init (e.Ma (), 0.0f)) { node.reset (); return false; }
	ma_node_attach_output_bus (node->Node (), PathNode::AIR, e.Group (EXTERIOR), 0);
	synth = AmbienceSynth::Make (e, 77);
	if (!synth || !synth->Attach (node->Node ())) { synth.reset (); node->Uninit (); node.reset (); return false; }
	synth->Start ();
	return true;
}

void Ambience::Clear ()
{
	if (cabin) sys.StopSound (cabin), cabin = 0;
	if (cabinEvent) sys.StopSound (cabinEvent), cabinEvent = 0;
	synth.reset ();
	if (node) node->Uninit (), node.reset ();
	nextEvent = -1.0;
}

void Ambience::Update (double t, double dt)
{
	if (!g_camera || !sys.GetEngine ().Active ()) return;
	static const bool off = getenv ("ORBITER_AUDIO_AMBIENCE") && *getenv ("ORBITER_AUDIO_AMBIENCE") == '0'; // (tests)
	if (off) { if (cabin || node) Clear (); return; }
	const bool cockpit = g_camera->IsInternal () && g_focusobj && !g_focusobj->Destroyed ();

	// the cabin: its air handling while the camera is in the cockpit
	if (cockpit) {
		if (!cabin || !sys.Playing (cabin)) cabin = sys.PlayOn (INTERIOR, CABIN_FILE, CABIN_P / P_FULL / CABIN_RMS, true);
		// now and then a pump, a valve
		if (nextEvent < 0.0 || t < nextEvent - 60.0) nextEvent = t + 7.0 + 28.0 * Rand ();
		if (t >= nextEvent) {
			char f[64];
			snprintf (f, sizeof (f), "XRSound/Default/Cabin Ambience/amb%d.wav", (int)(Rand () * 13.0) % 13);
			if (cabinEvent) sys.StopSound (cabinEvent);
			cabinEvent = sys.PlayOn (INTERIOR, f, CABIN_EVENT_P / P_FULL / EVENT_RMS * (0.5 + 0.5 * Rand ()), false);
			nextEvent = t + 7.0 + 28.0 * Rand ();
		}
	} else {
		if (cabin) sys.StopSound (cabin), cabin = 0;
		if (cabinEvent) sys.StopSound (cabinEvent), cabinEvent = 0;
		nextEvent = -1.0;
	}

	// outdoors: the camera near the ground, at rest over it
	windP = seaP = 0.0;
	double U = 0.0, period = 8.0;
	const Planet *pl = g_camera->ProxyPlanet ();
	if (pl) {
		const Vector rel (g_camera->GPos () - pl->GPos ());
		const Vector loc (tmul (pl->GRot (), rel));
		const double r = loc.length ();
		const double lng = std::atan2 (loc.z, loc.x), lat = std::asin (loc.y / std::max (r, 1.0));
		const double alt = r - pl->Size () - oapiSurfaceElevation ((OBJHANDLE)pl, lng, lat); // (above the ground)
		// (the camera's speed over the ground: from its motion in the planet's frame)
		double vg = 0.0;
		if (lastT >= 0.0 && t > lastT) vg = (loc - lastLoc).length () / (t - lastT);
		lastLoc = loc, lastT = t;
		const double gp[3] = { g_camera->GPos ().x, g_camera->GPos ().y, g_camera->GPos ().z };
		const Air a = World::AirAt (pl, gp);
		if (a.Present () && alt < OUT_ALT && vg < OUT_SPEED) {
			VECTOR3 w = oapiGetModelWind ((OBJHANDLE)pl, lng, lat, r - pl->Size () + 10.0);
			U = std::sqrt (w.x * w.x + w.y * w.y + w.z * w.z);
			windP = AmbienceSynth::WIND_EAR * 0.5 * a.rho * U * U;
			// the sea, if there is water within some hundreds of metres
			SEAPARAM sp;
			if (oapiGetSeaParams ((OBJHANDLE)pl, &sp, sizeof (sp)) && alt < 300.0) {
				bool water = false;
				const double d = 300.0 / std::max (r, 1.0);
				for (int k = 0; k < 5 && !water; k++) {
					const double dl = k == 1 ? d : k == 2 ? -d : 0.0, db = k == 3 ? d : k == 4 ? -d : 0.0;
					water = oapiIsWater ((OBJHANDLE)pl, lng + dl / std::max (std::cos (lat), 0.1), lat + db);
				}
				if (water) {
					seaP = AmbienceSynth::SeaPressure (sp.wind) * std::min (1.0, 50.0 / std::max (alt, 50.0)); // (fading as the listener rises above it)
					period = sp.tp > 0.0 ? sp.tp : 8.0;
				}
			}
		}
	}
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
	static double logT = -1e9;
	if (log && std::fabs (t - logT) >= 5.0) {
		logT = t;
		printf ("Audio: ambience at %.1f s: wind %.1f m/s, %.4f Pa; sea %.4f Pa; cabin %s\n", t, U, windP, seaP, cockpit ? "on" : "off"), fflush (stdout);
	}
	if (windP > 0.0 || seaP > 0.0) {
		if (!node && !Init ()) return;
	}
	if (node) {
		synth->Set (windP, U, seaP, period);
		PathNode::Target tg;
		tg.gain = (float)(1.0 / P_FULL) * (cockpit ? INSIDE_GAIN : 1.0f);
		tg.cutoff = cockpit ? INSIDE_CUTOFF : 20000.0f;
		node->Set (PathNode::AIR, tg);
	}
}

} // namespace audio
