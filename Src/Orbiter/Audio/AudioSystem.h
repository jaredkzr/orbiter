// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sound in Orbiter: the audio engine, started with the application and
// stepped with the simulation. It applies the user's settings, fades the
// simulation's buses on pause and time acceleration, and in a render
// (ORBITER_AUDIO_RENDER=<file.wav>) mixes the sound offline, block by
// block with the time steps, into a WAV file.
//
// Where the sound goes:
//   - a graphics client: the default output device
//   - ORBITER_AUDIO_RENDER=<file.wav>: that file, no device (repeatable
//     with --fixedstep: one step of audio per time step)
//   - headless, offscreen (ORBITER_VK_OFFSCREEN), ORBITER_AUDIO=0, or
//     sound off in the settings: nowhere
//   - ORBITER_AUDIO=1: the device, even without a graphics client
//
// Tests: ORBITER_AUDIO_TEST_TONE=<vessel>[:<Hz>[:<level>]] puts a sine on
// a vessel (travel time, Doppler, the shock cone in a render);
// ORBITER_AUDIO_LOG=1 logs what is heard of each emitter, twice a second;
// ORBITER_AUDIO_REALISM=physical|cinematic overrides the setting.
// =======================================================================

#ifndef __AUDIOSYSTEM_H
#define __AUDIOSYSTEM_H

#include "AudioEngine.h"
#include "WavWriter.h"
#include "World.h"
#include "VesselSounds.h"
#include "Callouts.h"
#include "FlightState.h"
#include <cstdio>
#include "WarpDrive.h"
#include "Effects.h"
#include "Ambience.h"
#include "Radio.h"
#include "Music.h"
#include "Hrtf.h"
#include "Assets.h"
#include "OrbiterScene.h"
#include <chrono>
#include <map>
#include <vector>

class Orbiter;
class Vessel;
struct CFG_SOUNDPRM;
struct ma_sound;

namespace audio {

/// Time acceleration above which the simulation's sounds are faded out
constexpr double WARP_MUTE = 10.0;

class System {
public:
	explicit System (Orbiter *orbiter);
	~System ();

	/// Start the engine (after the graphics client is loaded)
	void Start (bool graphicsClient);
	/// Stop it (the render file is closed)
	void Stop ();

	/// Once per time step, after the step: settings, fades, offline mixing
	void Step ();
	/// Once per frame of the launchpad without a session (the music)
	void Idle ();

	/// The sound settings were changed (the settings screen): applied at
	/// once, and the device opened if sound was switched on
	void SettingsChanged ();

	/// A session ends: its sounds go
	void EndSession ();
	/// A body is deleted: its sounds go (World::Forget: every emitter and
	/// identity of it; each part of the sound finds its own gone)
	void Forget (const Body *body) { world.Forget (body); }
	/// A vessel's contact points over a step (to time t [s], of length h),
	/// on a surface; docking and undocking at a port (speed: closing, or
	/// separating [m/s]; mred: the stacks' reduced mass [kg]); a part of it
	/// breaking off (pos: the piece's centre, vessel frame; hit: what it was
	/// struck at [m/s]); a crash (destroyed in an impact at speed [m/s])
	void Contacts (const Vessel *v, const std::vector<ContactSample> &pts, SurfaceKind s, double t, double h)
	{ vessels.Contacts (v, pts, s, t, h); }
	void Docked (const Vessel *v, const V3 &port, bool dock, double speed = 0.0, double mred = 0.0)
	{
		vessels.Docked (v, port, dock, speed, mred);
		Event (v, dock ? Callouts::EV_DOCKED : Callouts::EV_UNDOCKED);
	}
	void PartBroke (const Vessel *v, const V3 &pos, double mass, double hit) { vessels.PartBroke (v, pos, mass, hit); }
	void Crash (const Vessel *v, double speed, double mass, double energy) { vessels.Crash (v, speed, mass, energy); }
	/// Something happened to a vessel the voice may call out
	void Event (const Vessel *v, Callouts::Event e);
	/// A vessel breaks up with propellant [kg] aboard: its blast
	void Explosion (const Vessel *v, double propellant);
	Effects &GetEffects () { return effects; }
	/// The animation given the gear's sound (-1: none)
	int GearAnim (const Vessel *v) const { return vessels.GearAnim (v); }
	Callouts &GetCallouts () { return callouts; }
	/// The flight of the vessel in focus (its phase, this step's events)
	const FlightState &GetFlight () const { return flight; }
	Radio &GetRadio () { return radio; }
	/// The voice's callouts are on (the settings; ORBITER_AUDIO_CALLOUTS)
	bool CalloutsOn () const;
	/// The music is on (the settings; a render: ORBITER_AUDIO_MUSIC=1)
	bool MusicOn () const;
	/// Binaural, for headphones (the settings; ORBITER_AUDIO_HRTF)
	bool HrtfOn () const;

	/// A control used: the interface's (the launchpad, the menus), a
	/// cockpit's switch or knob, an MFD's button. Its click plays at the
	/// frame's end - one a frame (a button's before a switch's, a cockpit's
	/// before the interface's): recordings (Sound/Clicks), the cockpit's in
	/// its cabin at a switch's loudness (CLICK_P), paused on the interface bus
	enum ClickKind { CLICK_UI, CLICK_SWITCH, CLICK_BUTTON };
	void Click (ClickKind k) { if (k > pendingClick) pendingClick = k; }
	static constexpr double CLICK_P = 0.1;  ///< a cockpit control's click at the ear, peak [Pa] (74 dB)
	static constexpr double UI_CLICK = 0.07;///< the interface's, peak (-23 dBFS before its bus)
	Music &GetMusic () { return music; }

	// ---- the API (oapiAudio*, Lua's audio table) --------------------------
	/// Play a sound file on a vessel (at pos, vessel frame: heard as any of
	/// its sounds), or without one (null: the interface). The file: as given,
	/// else under Sound/. Returns a handle (0: not played).
	int Play (const Vessel *v, const std::string &file, const V3 &pos, double level, bool loop);
	/// Play a sound file on a bus, not in the world (the voice, the radio,
	/// music, the interface). Speech (the voice's, the radio's buses; not
	/// asked otherwise) is brought to SPEECH_LUFS first: recordings of all
	/// levels alike
	int PlayOn (Bus bus, const std::string &file, double level, bool loop, bool speech = true);
	static constexpr double SPEECH_LUFS = -24.0;   // (was -18: the callouts some 10 dB over a landing - by ear far too loud)
	bool StopSound (int handle);
	/// Its level (< 0: as it is) and pitch (<= 0: as it is)
	bool SetSound (int handle, double level, double pitch);
	bool Playing (int handle) const;
	bool Pause (int handle, bool pause);
	bool Paused (int handle) const;
	/// Left (-1) to right (1): sounds on a bus (false: on a vessel)
	bool SetPan (int handle, float pan);
	float GetPan (int handle) const;      ///< -100: no such sound
	double GetPitch (int handle) const;   ///< 0: no such sound
	/// To a time in the file [s]; where it is [s] (< 0: no such sound)
	bool Seek (int handle, double s);
	double Cursor (int handle) const;
	/// A gate on its level (0..1): where it may be heard (the XRSound shim:
	/// the cockpit only, the focus vessel only)
	bool SetGate (int handle, double g);
	/// A callout is speaking (the voice bus)
	bool Speaking () const;
	/// A sound file's path: as given (backslashes as slashes, any case),
	/// else under Sound/ (empty: none). (XRSound's default lines in
	/// Orbiter's voice: xr::Voice, where a line is played)
	static std::string FindSound (const std::string &file);
	bool Tune (const Vessel *v, const std::string &key, double value) { return vessels.TuneVessel (v, key, value); }
	/// One of a vessel's own sounds (VesselSounds::Sound) on or off
	bool DefaultSound (const Vessel *v, int sound, bool on) { return vessels.Enable (v, sound, on); }
	void Animation (const Vessel *v, unsigned anim, int kind, double level) { vessels.Animation (v, anim, kind, level); }
	/// A moment a vessel's code tells of (oapiAudioEvent): its sound, line and reply (false: no such event)
	bool VesselEvent (const Vessel *v, const std::string &event);

	Engine &GetEngine () { return engine; }
	World &GetWorld () { return world; }
	bool Rendering () const { return wav.IsOpen (); }

	/// The simulation buses' fade for a state of the simulation: 0 paused
	/// or above WARP_MUTE, else 1
	static float SimFadeFor (bool running, double warp);

private:
	void ApplySettings (const CFG_SOUNDPRM &s);
	void RenderStep (double dt);
	void TestTone ();
	void Boom (const World::Boom &b);

	Orbiter *orbiter;
	OrbiterScene scene;             // (the world's input: Orbiter's globals read here)
	Engine engine;
	World world{engine};
	VesselSounds vessels{engine, world};
	FlightState flight;             // (the focus vessel's flight: the voice, the radio, the music follow it)
	FILE *flightTrace = nullptr;    // (ORBITER_AUDIO_FLIGHT_TRACE)
	Callouts callouts{*this};
	Effects effects{engine, world};
	Ambience ambience{*this};
	Radio radio{*this};
	WarpDrive warpDrive{*this};
	Music music{*this};
	Hrtf hrtf;                      // (loaded when first asked for)
	int pendingClick = -1;          // (a ClickKind)
	bool switchUp = true;           // (switches flipped: one way, then the other)
	uint32_t clickRng = 0x1b873593u;
	void PlayClick ();
	void PrefetchClicks ();
	void TraceFlight (const FlightSample *s);
	bool hrtfTried = false;
	std::chrono::steady_clock::time_point idleT;
	void Duck (double dt);
	WavWriter wav;
	std::vector<float> block;
	double renderTime = 0.0;        // seconds of sound asked for
	double playT = 0.0;             // the clock of what is heard [s]: real time while the simulation's sounds are
	uint64_t flushed = 0;           // frames in the file at its last flush
	bool started = false;
	bool client = false;            // a graphics client (sound on the device)
	bool forcedOff = false;         // ORBITER_AUDIO=0
	EmitterId testTone;             // ORBITER_AUDIO_TEST_TONE
	// sounds played through the API: on vessels (emitters), on the buses
	struct Played {
		std::string path;                   // its file (resolved)
		EmitterDesc desc;                   // on a vessel (desc.body): its emitter's
		BodyRef vessel;                     // (that vessel: gone, the sound goes)
		EmitterId em;                       // ... when playing
		std::unique_ptr<ClipSound> flat;    // on a bus, when playing
		/// waiting for its file (decoded in the background): what's asked
		/// of it meanwhile kept, applied as it starts
		bool pending = false;
		bool speech = false;                // (brought to SPEECH_LUFS)
		double seek = -1.0;
		Bus bus = UI;
		bool loop = false, paused = false;
		double level = 1.0, gate = 1.0, pitch = 1.0;
		float pan = 0.0f;
	};
	int Launch (Played &&p);
	bool StartPending (Played &p);          // (false: it can't play)
	void Drop (Played &p);
	std::map<int, Played> played;
	Played *Find (int h);
	const Played *Find (int h) const;
	void ApplyLevel (Played &p);
	int nextHandle = 1;
	void UpdatePlayed ();
	double logT = -1.0;             // ORBITER_AUDIO_LOG: last line
	bool wasReplay = false;         // (the instant replay was showing last step)
};

} // namespace audio

#endif // !__AUDIOSYSTEM_H
