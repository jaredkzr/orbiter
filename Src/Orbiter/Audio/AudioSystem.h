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
// =======================================================================

#ifndef __AUDIOSYSTEM_H
#define __AUDIOSYSTEM_H

#include "AudioEngine.h"
#include "WavWriter.h"
#include <vector>

class Orbiter;
struct CFG_SOUNDPRM;

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

	Engine &GetEngine () { return engine; }
	bool Rendering () const { return wav.IsOpen (); }

	/// The simulation buses' fade for a state of the simulation: 0 paused
	/// or above WARP_MUTE, else 1
	static float SimFadeFor (bool running, double warp);

private:
	void ApplySettings (const CFG_SOUNDPRM &s);
	void RenderStep (double dt);

	Orbiter *orbiter;
	Engine engine;
	WavWriter wav;
	std::vector<float> block;
	double renderTime = 0.0;        // seconds of sound asked for
	uint64_t flushed = 0;           // frames in the file at its last flush
	bool started = false;
	bool client = false;            // a graphics client (sound on the device)
	bool forcedOff = false;         // ORBITER_AUDIO=0
};

} // namespace audio

#endif // !__AUDIOSYSTEM_H
