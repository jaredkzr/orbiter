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
#include "Effects.h"
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

	/// The sound settings were changed (the settings screen): applied at
	/// once, and the device opened if sound was switched on
	void SettingsChanged ();

	/// A session ends: its sounds go
	void EndSession ();
	/// A body is deleted: its sounds go
	void Forget (const Body *body);
	/// A vessel's contacts of a step; docking and undocking at a port
	void Contacts (const Vessel *v, const ContactStep &c) { vessels.Contacts (v, c); }
	void Docked (const Vessel *v, const V3 &port, bool dock)
	{
		vessels.Docked (v, port, dock);
		callouts.OnEvent (v, dock ? Callouts::EV_DOCKED : Callouts::EV_UNDOCKED);
	}
	/// Something happened to a vessel the voice may call out
	void Event (const Vessel *v, Callouts::Event e) { callouts.OnEvent (v, e); }
	/// A vessel breaks up with propellant [kg] aboard: its blast
	void Explosion (const Vessel *v, double propellant) { effects.Explosion (v, propellant); }
	Effects &GetEffects () { return effects; }
	/// The animation given the gear's sound (-1: none)
	int GearAnim (const Vessel *v) const { return vessels.GearAnim (v); }
	Callouts &GetCallouts () { return callouts; }
	/// The voice's callouts are on (the settings; ORBITER_AUDIO_CALLOUTS)
	bool CalloutsOn () const;

	// ---- the API (oapiAudio*, Lua's audio table) --------------------------
	/// Play a sound file on a vessel (at pos, vessel frame: heard as any of
	/// its sounds), or without one (null: the interface). The file: as given,
	/// else under Sound/. Returns a handle (0: not played).
	int Play (const Vessel *v, const std::string &file, const V3 &pos, double level, bool loop);
	/// Play a sound file on a bus, not in the world (the voice, the radio,
	/// music, the interface)
	int PlayOn (Bus bus, const std::string &file, double level, bool loop);
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
	/// else under Sound/ (empty: none)
	static std::string FindSound (const std::string &file);
	bool Tune (const Vessel *v, const std::string &key, double value) { return vessels.TuneVessel (v, key, value); }
	void Animation (const Vessel *v, unsigned anim, int kind, double level) { vessels.Animation (v, anim, kind, level); }

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
	Engine engine;
	World world{engine};
	VesselSounds vessels{engine, world};
	Callouts callouts{*this};
	Effects effects{engine, world};
	void Duck (double dt);
	WavWriter wav;
	std::vector<float> block;
	double renderTime = 0.0;        // seconds of sound asked for
	uint64_t flushed = 0;           // frames in the file at its last flush
	bool started = false;
	bool client = false;            // a graphics client (sound on the device)
	bool forcedOff = false;         // ORBITER_AUDIO=0
	Emitter *testTone = nullptr;    // ORBITER_AUDIO_TEST_TONE
	// sounds played through the API: on vessels (emitters), on the interface
	struct Played {
		Emitter *em = nullptr;              // on a vessel
		std::unique_ptr<ma_sound> flat;     // on a bus
		Bus bus = UI;
		bool loop = false, paused = false;
		double level = 1.0, gate = 1.0, pitch = 1.0;
		float pan = 0.0f;
	};
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
