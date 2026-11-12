// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The ears: the simulation's sound (outside, cockpit) spans from a quiet
// cabin to a launch pad, 120 dB and more; the ears take it in as a
// compressor: below the threshold as it is, above it rising at 1/ratio,
// with a fast attack and a slow recovery (a launch dulls what follows for
// a while), and a soft limit on the peaks. Physical levels come in scaled
// so that P_FULL [Pa] is full scale.
// =======================================================================

#ifndef __AUDIOEARS_H
#define __AUDIOEARS_H

#include <memory>

struct ma_engine;

namespace audio {

/// Sound pressure [Pa] at full scale (before the ears): 130 dB SPL
constexpr double P_FULL = 63.2;

struct EarSettings {
	float threshold = -20.0f; // [dBFS] (110 dB SPL)
	float ratio = 3.0f;
	float attack = 0.02f;     // [s]
	float release = 1.5f;     // [s]
	float ceiling = 0.9f;     // peaks softly limited to this
};

class Ears {
public:
	using Settings = EarSettings;

	Ears ();
	~Ears ();
	bool Init (ma_engine *engine, const Settings &s = Settings ());
	void Uninit ();
	/// The node (input 0: the buses to hear through it; output to the endpoint)
	void *Node ();
	/// The gain reduction now [dB] (<= 0)
	float Reduction () const;

	/// The gain computer's curve (tests): the gain [dB] for a level [dB]
	static float GainDb (float levelDb, const Settings &s);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace audio

#endif // !__AUDIOEARS_H
