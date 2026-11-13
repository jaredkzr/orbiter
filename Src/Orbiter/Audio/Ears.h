// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The ears: the simulation's sound (outside, cockpit) spans from a quiet
// cabin to a launch pad, 120 dB and more; the ears take it in as a
// compressor: below the threshold as it is, above it rising at 1/ratio,
// with a fast attack and a slow recovery (a launch dulls what follows for
// a while), and a peak limiter that looks 2 ms ahead (the gain comes down
// smoothly before a peak, no clipping). Physical levels come in scaled so
// that P_FULL [Pa] is full scale.
//
// Before all that, the listening level (adapt): ears adapt to where they
// are - a quiet cabin heard as clearly as a busy pad - and so does the mix
// (as games' "HDR audio" does): a slow gain raises a quiet scene towards a
// comfortable level (up to adaptMax), comes down within a fraction of a
// second when something loud starts (which still strikes through the
// compressor at full force), and recovers over seconds after it.
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
	bool adapt = false;       // the listening level (off: physical levels, P_FULL full scale)
	float adaptTarget = -28.0f; // [dBFS] the scene's level it raises a quiet one to
	float adaptMax = 45.0f;   // [dB] the most it raises
	float adaptDown = 0.15f;  // [s] coming down (something loud)
	float adaptUp = 2.0f;     // [s] going up again (as the scene's level, held, falls back: 4 dB/s)
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
	/// The listening level's gain now [dB] (>= 0)
	float AdaptGain () const;
	void SetAdapt (bool on);
	/// How hard the dynamics are squeezed (the settings' "Dynamic range"):
	/// 0 off (the true dynamics: only the peak limiter; no listening level),
	/// 1 light, 2 medium (the defaults), 3 strong (night, a low volume)
	enum Compression { COMP_OFF = 0, COMP_LIGHT = 1, COMP_MEDIUM = 2, COMP_STRONG = 3 };
	static Settings ForCompression (int level);
	/// Live: the compressor's and the listening level's settings of a level
	void SetCompression (int level);

	/// The gain computer's curve (tests): the gain [dB] for a level [dB]
	static float GainDb (float levelDb, const Settings &s);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace audio

#endif // !__AUDIOEARS_H
