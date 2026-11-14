// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The radio: the vessel in focus and the ground (Mission Control, on Earth)
//
//  - chatter: air-to-ground traffic from the Shuttle and the ISS (XRSound's
//    "Altea STS" recordings, NASA's, public domain) now and then near Earth
//    (every 15-120 s, five times less often above 400 km; XRSound's [ATC]
//    settings: its cfgs may switch it off in the air, on the ground)
//  - replies: Mission Control answering the flight's events (liftoff,
//    MECO, orbit, entry, docking, splashdown, wheels stop: Piper's
//    en_US-norman, Sound/Voice/Ground), after the light's round trip and a
//    moment to react - seconds at the Moon, minutes at Mars
//  - a reentry's blackout: the plasma about the vessel stops radio (faster
//    than 5 km/s in air of some 300 Pa dynamic pressure and more); the
//    ground calls again when it is through ("we have you back")
//  - the radio's sound: Quindar tones as the ground keys its transmitter
//    (2525 Hz in, 2475 Hz out), a voice channel (300-3400 Hz), a little
//    clipping, static by the link's signal to noise (falling as 20 log10 of
//    the distance; ground dishes keep it above 10 dB), the squelch's tail
// =======================================================================

#ifndef __AUDIORADIO_H
#define __AUDIORADIO_H

#include "PathNode.h"
#include "Synth.h"
#include "World.h"
#include "FlightState.h"
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class Vessel;
class CelestialBody;

namespace audio {

class System;

/// The radio's sound: its band [Hz] (each side: stages of a Q 0.7 biquad), its clipping
/// (tanh of drive y, / drive), its static at best (S/N [dB]: the link's, no better)
struct RadioSound { float hp = 300.0f, lp = 3400.0f; int stages = 2; float drive = 2.0f, snrMax = 50.0f; };

/// A transmission: a recording through the radio's sound
class RadioSynth : public SynthSource {
public:
	using Sound = RadioSound;
	/// buf: the voice (mono, the engine's rate); quindar: the ground keying;
	/// snr: signal to noise [dB]
	/// (lufs: the voice's loudness, if known - Clip::lufs; else measured)
	static SourcePtr<RadioSynth> Make (Engine &e, std::vector<float> &&buf, bool quindar, float snr, uint32_t seed,
		const Sound &sound = Sound (), double lufs = NAN);
	bool Done () const override { return done.load (std::memory_order_acquire); }
	/// Lost (a blackout): a burst of static, then nothing
	void Cut () { cut.store (true, std::memory_order_release); }
	double Length () const;
	/// The link's signal to noise [dB] at a distance from Earth [m]
	static double Snr (double d);
	/// The radio's sound for a file (a CAPCOM's own: "_c2_" in its name; else the default)
	static Sound SoundFor (const std::string &file);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	RadioSynth (std::vector<float> &&b, bool q, float snr, uint32_t seed, const Sound &s)
		: buf (std::move (b)), quindar (q), snrDb (std::min (snr, s.snrMax)), rng (seed), sound (s) {}
	std::vector<float> buf;
	bool quindar;
	float snrDb;
	std::atomic<bool> done{false}, cut{false};
	// (audio thread)
	dsp::Rng rng;
	Sound sound;
	dsp::Biquad hp1, hp2, lp1, lp2, hissBp;
	// the link's quality (1: near Earth; 0: the worst, far out), from the S/N: a narrower band,
	// the signal fading, dropping out under the static
	float link = 1.0f, fade = 1.0f, fadeTarget = 1.0f, drop = 1.0f;
	size_t fadeNext = 0, dropEnd = 0;
	// (a low data rate's codec: the voice held for `hold` samples, quantized to `levels`)
	unsigned hold = 1, held = 0;
	float levels = 0.0f, heldY = 0.0f;
	float bandHp = 300.0f, bandLp = 3400.0f;   // (the band, narrowed far out)
	/// The link's state from the S/N: its quality, its band, its codec; and the gain that keeps a
	/// narrowed, fading voice as loud as near Earth
	void Link (unsigned rate);
	bool setup = false;
	size_t pos = 0;           // samples into the whole (tones, voice, tail)
	float gain = 1.0f;        // the voice to the radio's loudness
	size_t cutAt = 0;
};

class Radio {
public:
	explicit Radio (System &s);
	~Radio ();

	enum Event { LIFTOFF, MECO, ORBIT, DOCKED, UNDOCKED, SPLASHDOWN, LANDED,
		MAXQ, IGNITION, DEORBIT, SURFACE,   // (the callouts' too)
		CAPTURE, ESCAPE,                    // (into orbit about a body, onto an escape path: by a burn)
		DESCENT, APPROACH,                  // (a powered descent on an airless world; a rendezvous inside 1 km)
		BOOSTER_SEP, TANK_SEP,              // (a vessel's own: oapiAudioEvent)
		DEEPSPACE,                          // (leaving Earth's neighbourhood: the replies will lag)
		ENTRY, AOS, LOS, LOST,              // (the radio's own: entry interface, the signal back after a blackout, lost; the vehicle lost near the ground)
		NEVENT };
	/// Something the ground hears of (the focus vessel's): it replies, with one of
	/// its takes (Sound/Voice/Ground/<Event>_<n>.flac; not the last one again)
	void OnEvent (Event e);
	/// The flight's events of a step (FlightState): those the ground answers
	void OnFlight (const std::vector<FlightEvent> &events);
	/// Each step: the simulation's time (the light's travel, the plasma) and the clock of what is
	/// heard (the ground's pauses, the chatter's: held in a pause, not hurried by time acceleration)
	void Update (double t, double playT);
	void Clear ();

	/// (tests) transmissions played so far; in a blackout
	int Played () const { return played; }
	bool Blackout () const { return blackout; }
	/// A transmission is being heard
	bool Transmitting () const { return Busy (); }
	/// The link's signal to noise [dB] at a distance from Earth [m]
	static double Snr (double d);
	/// The radio's sound for a file (a CAPCOM's own: "_c2_" in its name; else the default)
	static RadioSynth::Sound SoundFor (const std::string &file);
	static constexpr double REACT = 1.5; // [s] the ground's moment to answer (the chatter, AOS)
	/// [s] the ground's pause before a reply, once the crew's callout is over: about half a
	/// second, varied (0.25 to 1.3 s; lognormal: a conversation's turn-taking)
	double ReplyGap ();

private:
	System &sys;
	std::unique_ptr<PathNode> node;
	struct Playing { SourcePtr<RadioSynth> s; };
	std::vector<Playing> playing;
	/// (due: when it reaches the vessel - the simulation's time; ready: the ground's pause over - the
	/// clock of what is heard)
	struct Pending { std::string file; double due; bool ground; double gap; double ready; };
	std::deque<Pending> replies;
	double nextChatter = -1.0, t = 0.0, pt = 0.0, blackoutSince = -1.0;
	bool blackout = false, entrySaid = false;
	int played = 0;
	std::vector<std::string> bag;  // chatter clips not played lately
	uint32_t rng = 0x3c6ef372u;
	double Rand () { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.0 / 16777216.0); }
	const Vessel *vessel = nullptr;
	BodyRef vref;          // (its identity)
	double earthDist = 0.0;
	int lastTake[NEVENT];                  // (the take played last, per event: -1 none)
	double losDue = -1.0, losReady = 0.0;  // (the ground's "lost your signal": when - sim, heard)
	double quietSince = 0.0;               // (the last transmission over: when)
	bool wasBusy = false;
	std::string Take (Event e);
	/// The CAPCOM on shift ("c1", "c2"): 8-hour shifts by the simulation's clock, alternating
	/// (ORBITER_AUDIO_CAPCOM=1 or 2: that one)
	static const char *OnShift ();
	/// The takes that name Houston (from Sound/Voice/Ground/lines.txt: their words), not said
	/// beyond the Earth and the Moon
	bool SaysHouston (const std::string &file);
	std::vector<std::string> houston;
	bool houstonRead = false;
	const CelestialBody *lastRef = nullptr;   // (where the vessel was, for a reply after it's gone: LOS)
	double lastAlt = 1e9;                     // (and how high: lost near the ground, a different reply)
	bool Init ();
	bool Transmit (const std::string &file, bool ground);
	void Prefetch (const std::string &file);   // (decoded in the background before its turn)
	bool Busy () const;
	void Reply (Event e);
};

} // namespace audio

#endif // !__AUDIORADIO_H
