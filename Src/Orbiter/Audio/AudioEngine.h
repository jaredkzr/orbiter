// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The audio engine: miniaudio's engine and node graph, with the buses
// everything plays through. On an output device (the audio thread pulls
// the mix), or offline: the caller pulls the mix block by block, tied to
// simulation time, for repeatable renders (tests, videos).
//
//   sources -> bus (sound group, user volume x simulation fade) -> master
//
// Buses: what is heard outside (through the air), in the cockpit (through
// the structure), voice callouts, the radio, music and the interface.
// The first four belong to the simulation: they fade out when it pauses
// or runs at high time acceleration (the fade); music and the interface
// play on.
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

private:
	struct Impl;
	std::unique_ptr<ma_engine> engine;
	std::unique_ptr<Impl> impl;
	Mode mode = OFF;
	unsigned sampleRate = 48000, channels = 2;
	std::string deviceName;
	float simFade = 1.0f;
};

} // namespace audio

#endif // !__AUDIOENGINE_H
