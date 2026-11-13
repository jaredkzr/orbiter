// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The audio engine: miniaudio's engine and node graph, with the buses
// everything plays through. On an output device (the audio thread pulls
// the mix), or offline: the caller pulls the mix block by block, tied to
// simulation time, for repeatable renders (tests, videos).
//
//   sources -> bus (sound group, user volume x simulation fade) -> master
//   (the outside and cockpit buses through the ears: a compressor, Ears.h)
//
// Buses: what is heard outside (through the air), in the cockpit (through
// the structure), voice callouts, the radio, music and the interface.
// The first four belong to the simulation: they fade out when it pauses
// or runs at high time acceleration (the fade); music and the interface
// play on.
//
// The mix (loudness, EBU R128, at the default volumes; measured in the
// Shuttle's ascent, Tests/Audio/Ascent.scn):
//   the world      about -20 LUFS loud (the ears' compressor), quiet
//                  scenes raised towards -28 dBFS (the listening level)
//   voice          speech brought to -18 LUFS (each file: Loudness.h),
//                  some 3 dB over the world - which ducks 9 dB under it
//   radio          voices to -20 LUFS before the channel; some 4 dB under
//                  the callouts - the world and the music duck 6 dB
//   music          -30 dBFS RMS (MusicSynth): 10 dB under a launch, about
//                  the level of a quiet scene
//   interface      clicks peaking at -29 dBFS
// =======================================================================

#ifndef __AUDIOENGINE_H
#define __AUDIOENGINE_H

#include <cstdint>
#include <memory>
#include <string>

struct ma_engine;
struct ma_sound;
typedef struct ma_sound ma_sound_group;

namespace audio {

enum Bus : int { EXTERIOR, INTERIOR, VOICE, RADIO, MUSIC, UI, NBUS };

const char *BusName (Bus b);

/// Buses that belong to the simulation (faded on pause and time acceleration)
inline bool SimBus (Bus b) { return b <= RADIO; }

/// A volume setting (0..1, as on a slider) as a gain: about 40 dB of range
/// over the slider, silent at 0
float VolumeToGain (float v);

class Engine {
public:
	enum Mode { OFF, DEVICE, OFFLINE };

	struct Options {
		Mode mode = DEVICE;
		unsigned sampleRate = 48000;
		unsigned channels = 2;
		bool ears = true;   ///< the outside and cockpit buses heard through the ears (Ears.h)
		bool adapt = false; ///< ... at an adapting listening level (Ears.h)
	};

	Engine ();
	~Engine ();
	Engine (const Engine&) = delete;
	Engine &operator= (const Engine&) = delete;

	/// Start the engine. A device that can't be opened leaves the engine
	/// OFF (returns false); everything else then does nothing.
	bool Init (const Options &opt);
	void Shutdown ();

	Mode GetMode () const { return mode; }
	bool Active () const { return mode != OFF; }
	unsigned SampleRate () const { return sampleRate; }
	unsigned Channels () const { return channels; }
	/// The output device's name (DEVICE mode)
	const std::string &DeviceName () const { return deviceName; }

	ma_engine *Ma () { return engine.get(); }
	ma_sound_group *Group (Bus b);

	/// Volumes (gains, linear)
	void SetMasterGain (float g);
	void SetBusGain (Bus b, float g);

	/// The voice's ducking: the other sounds of the simulation (outside,
	/// cockpit, radio, music) at this gain (1: none) while a callout speaks
	void SetDuck (float g);
	float Duck () const { return duck; }
	static bool Ducked (Bus b) { return b == EXTERIOR || b == INTERIOR || b == RADIO || b == MUSIC; }
	/// The radio's: the world and the music (not the radio) at this gain
	/// while a transmission is heard
	void SetRadioDuck (float g);
	float RadioDuck () const { return rduck; }
	static bool RadioDucked (Bus b) { return b == EXTERIOR || b == INTERIOR || b == MUSIC; }

	/// The simulation buses' fade (pause, time acceleration): towards
	/// `target` (0..1) over `seconds`. Repeated calls with the same target
	/// don't restart the fade.
	void SetSimFade (float target, double seconds);
	float SimFadeTarget () const { return simFade; }

	/// OFFLINE: mix the next `frames` frames into `out` (interleaved,
	/// Channels() per frame). Returns the frames written (`frames`, or 0
	/// when the engine isn't offline).
	uint64_t Render (float *out, uint64_t frames);

	/// The engine's clock: frames mixed since Init
	uint64_t Time () const;

	/// The ears' gain reduction now [dB] (0: none, or no ears)
	float EarsReduction () const;
	/// The listening level's gain now [dB]; on or off
	float EarsAdapt () const;
	void SetAdapt (bool on);
	/// The dynamics' compression (Ears::Compression)
	void SetCompression (int level);

	/// The mix's load: the audio thread's CPU time for a block over the
	/// block's length (0.05: 5 % of a core), smoothed over seconds; the
	/// worst block lately (it fades)
	float Load () const;
	float PeakLoad () const;

	struct Impl; // (its graph's parts; the load's accounting)

private:
	std::unique_ptr<ma_engine> engine;
	std::unique_ptr<Impl> impl;
	Mode mode = OFF;
	unsigned sampleRate = 48000, channels = 2;
	std::string deviceName;
	float simFade = 1.0f;
	float busGain[NBUS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	float duck = 1.0f, rduck = 1.0f;
};

} // namespace audio

#endif // !__AUDIOENGINE_H
