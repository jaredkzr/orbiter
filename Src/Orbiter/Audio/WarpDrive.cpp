// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "WarpDrive.h"
#include "DebugEnv.h"
#include "CrewLines.h"
#include "AudioSystem.h"
#include "World.h"
#include "RelTravel.h"
#include "Vessel.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace audio {

namespace {
	const char *ENGAGE = "Sound/Generated/warp_engage.flac", *DISENGAGE = "Sound/Generated/warp_disengage.flac";
	const char *ACCEL = "Sound/Generated/warp_accel.flac", *DECEL = "Sound/Generated/warp_decel.flac";
	const char *HUM = "Sound/Generated/warp_hum.flac";
	/// The drive's level in the cockpit (the files at -24 dBFS: about the cabin's events)
	constexpr double LEVEL = 0.35, HUM_LEVEL = 0.25;
	constexpr float HUM_FADE = 1.5f;   // [s] in and out

	bool Engaged (int ph)
	{
		return ph != reltravel::PH_IDLE && ph != reltravel::PH_ARRIVED && ph != reltravel::PH_FAILED && ph >= 0;
	}
	bool Burning (int ph)
	{
		return ph == reltravel::PH_BURN1 || ph == reltravel::PH_BURN2 || ph == reltravel::PH_HOLD;
	}
}

void WarpDrive::Play (int bus, const char *file, double level)
{
	if (System::FindSound (file).empty ()) return;   // (not made: silent)
	sys.PlayOn ((Bus)bus, file, level, false, false);
	const bool log = Dbg ().log > 0;
	if (log) printf ("Audio: warp drive %s\n", file), fflush (stdout);
}

void WarpDrive::Step (const Vessel *focus, bool cockpit, double dt)
{
	if (focus && focus->Destroyed ()) focus = nullptr;
	const reltravel::Controller *trip = focus ? focus->RelTripIf () : nullptr;
	const int ph = trip ? trip->GetPhase () : reltravel::PH_IDLE;
	const BodyRef fr = sys.GetWorld ().Ref (focus);
	if (fr != vessel) { vessel = fr; prevPhase = ph; }   // (a vessel taken over: no sound for its state)

	const bool logPhase = Dbg ().log > 0;
	if (logPhase && ph != prevPhase) printf ("Audio: drive phase %d -> %d%s\n", prevPhase, ph, fr != vessel ? " (another vessel)" : ""), fflush (stdout);

	// heard: in the cockpit; outside only with cinematic sound (physical: vacuum)
	const bool heard = cockpit || sys.GetWorld ().GetRealism () == CINEMATIC;
	const int bus = cockpit ? INTERIOR : EXTERIOR;
	// the crew's word on it (a beat after the sound; through the callouts: their setting, any view)
	if (ph != prevPhase && prevPhase >= 0) {
		const char *say = nullptr;
		if (!Engaged (prevPhase) && Engaged (ph)) say = "DriveEngaged";
		else if (ph == reltravel::PH_BURN1) say = "AccelerationBurn";
		else if (ph == reltravel::PH_COAST) say = "Coasting";
		else if (ph == reltravel::PH_BURN2) say = "BrakingBurn";
		else if (ph == reltravel::PH_ARRIVED) say = "Arrived";
		else if (ph == reltravel::PH_FAILED) say = "WarningNoTripFound";
		else if (ph == reltravel::PH_HOLD) say = "HoldingPosition";
		else if (ph == reltravel::PH_IDLE && Engaged (prevPhase)) say = "DriveDisengaged";
		if (say) line = say, lineIn = 1.2;
	}
	if (line && (lineIn -= dt) <= 0.0) {
		// (a line not made: silent)
		const CrewLine *c = CrewLines::Game ().Find (line);
		if (c && !System::FindSound (c->file).empty ()) sys.GetCallouts ().Crew (line);
		line = nullptr;
	}
	if (heard && ph != prevPhase && prevPhase >= 0) {
		if (!Engaged (prevPhase) && Engaged (ph)) Play (bus, ENGAGE, LEVEL);
		if (ph == reltravel::PH_BURN1) Play (bus, ACCEL, LEVEL);
		else if (ph == reltravel::PH_BURN2) Play (bus, DECEL, LEVEL);
		if (Engaged (prevPhase) && !Engaged (ph)) Play (bus, DISENGAGE, LEVEL);
	}
	prevPhase = ph;

	// the hum while the drive burns, faded in and out (a camera moved in or out: on its bus)
	if (hum && humBus != bus) { sys.StopSound (hum); hum = 0; }
	const float target = heard && Burning (ph) ? 1.0f : 0.0f;
	humLevel += std::clamp (target - humLevel, -(float)dt / HUM_FADE, (float)dt / HUM_FADE);
	if (humLevel > 0.0f && !hum && !System::FindSound (HUM).empty ()) hum = sys.PlayOn ((Bus)bus, HUM, HUM_LEVEL, true, false), humBus = bus;
	if (hum) {
		if (humLevel <= 0.0f) { sys.StopSound (hum); hum = 0; }
		else sys.SetGate (hum, humLevel);
	}
}

void WarpDrive::Clear ()
{
	if (hum) sys.StopSound (hum), hum = 0;
	humLevel = 0.0f;
	line = nullptr;
	vessel = BodyRef ();
	prevPhase = -1;
}

} // namespace audio
