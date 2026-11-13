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
#include <deque>
#include <memory>
#include <string>
#include <vector>

class Vessel;

namespace audio {

class System;

/// A transmission: a recording through the radio's sound
class RadioSynth : public SynthSource {
public:
	/// buf: the voice (mono, the engine's rate); quindar: the ground keying;
	/// snr: signal to noise [dB]
	static std::unique_ptr<RadioSynth> Make (Engine &e, std::vector<float> &&buf, bool quindar, float snr, uint32_t seed);
	bool Done () const override { return done.load (std::memory_order_acquire); }
	/// Lost (a blackout): a burst of static, then nothing
	void Cut () { cut.store (true, std::memory_order_release); }
	double Length () const;

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	RadioSynth (std::vector<float> &&b, bool q, float snr, uint32_t seed): buf (std::move (b)), quindar (q), snrDb (snr), rng (seed) {}
	std::vector<float> buf;
	bool quindar;
	float snrDb;
	std::atomic<bool> done{false}, cut{false};
	// (audio thread)
	dsp::Rng rng;
	dsp::Biquad hp1, hp2, lp1, lp2, hissBp;
	bool setup = false;
	size_t pos = 0;           // samples into the whole (tones, voice, tail)
	float gain = 1.0f;        // the voice to the radio's loudness
	size_t cutAt = 0;
};

class Radio {
public:
	explicit Radio (System &s);
	~Radio ();

	enum Event { LIFTOFF, MECO, ORBIT, DOCKED, UNDOCKED, SPLASHDOWN, LANDED, NEVENT };
	/// Something the ground hears of (the focus vessel's): it replies
	void OnEvent (Event e);
	/// Each step
	void Update (double t);
	void Clear ();

	/// (tests) transmissions played so far; in a blackout
	int Played () const { return played; }
	bool Blackout () const { return blackout; }
	/// A transmission is being heard
	bool Transmitting () const { return Busy (); }
	/// The link's signal to noise [dB] at a distance from Earth [m]
	static double Snr (double d);
	static constexpr double REACT = 1.5; // [s] the ground's moment to answer

private:
	System &sys;
	std::unique_ptr<PathNode> node;
	struct Playing { std::unique_ptr<RadioSynth> s; };
	std::vector<Playing> playing;
	struct Pending { std::string file; double due; bool ground; };
	std::deque<Pending> replies;
	double nextChatter = -1.0, t = 0.0, blackoutSince = -1.0;
	bool blackout = false, entrySaid = false;
	int played = 0;
	std::vector<std::string> bag;  // chatter clips not played lately
	uint32_t rng = 0x3c6ef372u;
	double Rand () { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.0 / 16777216.0); }
	const Vessel *vessel = nullptr;
	double earthDist = 0.0;
	bool Init ();
	bool Transmit (const std::string &file, bool ground);
	bool Busy () const;
	void Reply (const char *file);
};

} // namespace audio

#endif // !__AUDIORADIO_H
