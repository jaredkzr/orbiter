// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A vessel touching things: the ground under its wheels, its hull on the
// ground, other vessels.
//
//  - rolling: the tyres' rumble, deeper and louder with the load, its pace
//    (the tread's and the runway's joints) with the speed
//  - skidding: tyres squealing (a skid's slip) or the hull scraping and
//    grinding over the ground, as loud as the power the friction takes
//  - blows: a wheel's touchdown (the strut's thump, the tyre's chirp), the
//    hull striking the ground (a heavy thud and crunch), a vessel bumping
//    another (a metallic clang, its ring), docking (the clunk, the latches
//    closing a moment later) and undocking (the latches, the push-off), a
//    door's latch, the gear locking (a heavy thunk)
//  - motors moving parts (animations): servos humming, doors' actuators, the
//    gear's hydraulics (a low hum and the fluid's hiss), deep, their pitch
//    rising a little with their speed
//
// Blows are counted: each new count (Strike) plays one, at its strength
// (0..1, a blow's closing speed against a hard landing's).
// =======================================================================

#ifndef __CONTACTSYNTH_H
#define __CONTACTSYNTH_H

#include "Synth.h"

namespace audio {

class ContactSynth : public SynthSource {
public:
	enum Blow { TOUCHDOWN = 0, HULL = 1, CLANG = 2, DOCK = 3, UNDOCK = 4, LATCH = 5, GEARLOCK = 6, NBLOW = 7 };
	/// Motors moving parts: a servo, a door's actuator, the gear's hydraulics
	enum Motor { SERVO = 0, DOOR = 1, GEAR = 2, NMOTOR = 3 };

	struct Params {
		float rollSpeed = 0.0f;   // wheels rolling [m/s]
		float rollLoad = 0.0f;    // 0..1 (the vessel's weight on its wheels: 1)
		float squeal = 0.0f;      // tyres skidding 0..1
		float scrape = 0.0f;      // the hull sliding over the ground 0..1
		float scrapeSpeed = 0.0f; // [m/s]
		float motor[NMOTOR] = {}; // motors running, by kind 0..1 (their speed)
	};

	static std::unique_ptr<ContactSynth> Make (Engine &e, uint32_t seed);
	void Set (const Params &p);
	/// A blow of a kind at a strength (0..1)
	void Strike (Blow kind, float strength);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit ContactSynth (uint32_t seed): rng (seed) { for (auto &m : pMotor) m.store (0.0f); }
	std::atomic<float> pRollSpeed{0.0f}, pRollLoad{0.0f}, pSqueal{0.0f}, pScrape{0.0f}, pScrapeSpeed{0.0f};
	std::atomic<float> pMotor[NMOTOR];
	dsp::Smooth motorS[NMOTOR];
	float motorPh[NMOTOR][3] = {};
	dsp::Biquad hydraulicBp;
	dsp::OnePole motorWob[NMOTOR];  // (a motor's load wavering: its level and pitch)
	// blows: a small queue (simulation thread writes, audio thread reads)
	struct Pending { std::atomic<int> count{0}; std::atomic<float> strength{0.0f}; } pending[NBLOW];
	int played[NBLOW] = {};

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad rollLp, rollBp, squealBp, squealBp2, scrapeBp, scrapeHp, thudLp, chirpBp, crunchLp;
	dsp::OnePole rollAm, scrapeAm;
	dsp::Smooth rollS, loadS, squealS, scrapeS;
	float rollPh = 0.0f, wheelPh = 0.0f, jointT = 1.0f;
	float squealPh = 0.0f, squealWob = 0.0f;
	// a blow sounding: its kind, strength, time since it started
	struct Voice { int kind = -1; float s = 0.0f, t = 0.0f, ph = 0.0f, ph2 = 0.0f, ph3 = 0.0f; float delay = 0.0f; };
	Voice voices[6];
	void Start (int kind, float s, float delay = 0.0f);
	float Voice1 (Voice &v, float chirp, float crunch);
};

} // namespace audio

#endif // !__CONTACTSYNTH_H
