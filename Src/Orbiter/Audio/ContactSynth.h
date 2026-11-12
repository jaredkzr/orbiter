// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A vessel touching things: the ground under its wheels, its hull on the
// ground, other vessels.
//
// A hybrid: recordings (Sound/Contacts/*.flac, CC0, see Sound/CREDITS.md)
// for what recordings do best - the clacks of latches, the clunk of two
// structures meeting, a tyre's squeal, metal dragged - driven and layered
// by the simulation, under physical models for the rest:
//
//  - rolling: the tyres over the runway's texture - a dense train of soft
//    impacts (the aggregate, the tread) at the pace of the speed, each
//    ringing the tyre's and the strut's low modes, the slabs' joints (every
//    6 m) heavier ones; a low textured rumble, nothing above a few hundred
//    hertz, deeper and louder with the load
//  - skidding: the tyres' squeal (the recording, looped, its pitch and
//    level wavering as the rubber sticks and slips) or the hull scraping
//    (the recording, its pace with the speed, in grains) over a low rumble
//  - blows: each a modal body - a few damped low modes of the airframe
//    struck by a Hertz contact pulse (a few milliseconds: soft blows wake
//    only the lowest) - under a recording: a wheel's touchdown (the strut's
//    thump, the tyre's chirp), the hull striking the ground (a heavy slam
//    and a crunch), a vessel bumping another (a dull bump), docking (the
//    clunk, the latches closing a moment later), undocking (the latches,
//    the push-off), a door's latch, the gear locking (a heavy thunk, the
//    downlock)
//  - motors moving parts (animations): an electric motor (the recording,
//    looped, its pitch with the speed): servos high, doors' actuators
//    lower, the gear's hydraulics deep with a pump's whine and the fluid's
//    hiss
//
// Without the recordings (not found) every sound falls back to a model.
//
// Blows are counted: each new count (Strike) plays one, at its strength
// (0..1, a blow's closing speed against a hard landing's).
// =======================================================================

#ifndef __CONTACTSYNTH_H
#define __CONTACTSYNTH_H

#include "Synth.h"
#include <string>

namespace audio {

struct ContactSample;

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

	/// Where the recordings are (default Sound/Contacts, or
	/// ORBITER_CONTACT_SAMPLES); set before the first Make
	static void SetSampleDir (const std::string &dir);
	/// The recordings loaded (false: every sound is modelled)
	bool HasSamples () const { return hasSamples; }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit ContactSynth (uint32_t seed): rng (seed) { for (auto &m : pMotor) m.store (0.0f); }
	void LoadSamples (unsigned rate);

	std::atomic<float> pRollSpeed{0.0f}, pRollLoad{0.0f}, pSqueal{0.0f}, pScrape{0.0f}, pScrapeSpeed{0.0f};
	std::atomic<float> pMotor[NMOTOR];
	// blows: a small queue (simulation thread writes, audio thread reads)
	struct Pending { std::atomic<int> count{0}; std::atomic<float> strength{0.0f}; } pending[NBLOW];
	int played[NBLOW] = {};

	// the recordings (shared, immutable once loaded)
	enum Smp { S_CLUNK, S_THUMP, S_DOORCLOSE, S_BUMP, S_LATCH1, S_LATCH2, S_LATCH3, S_LOCK, S_SQUEAL, S_SCRAPE, S_MOTOR, NSMP };
	std::shared_ptr<const ContactSample> smp[NSMP];
	bool hasSamples = false;

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Smooth rollS, loadS, squealS, scrapeS, motorS[NMOTOR];

	/// A recording looped (crossfading at the seam)
	struct Loop {
		const ContactSample *s = nullptr;
		double pos = 0.0;
		float Next (float rate);
	};
	/// A recording played once: at a rate (gliding), a gain (decaying), a tone
	struct Shot {
		const ContactSample *s = nullptr;
		double pos = 0.0;
		float rate = 1.0f, glide = 1.0f, gain = 0.0f, decay = 1.0f, att = 1.0f, attK = 1.0f;
		dsp::OnePole tone;
		bool Alive () const;
		float Next ();
	};
	/// A damped resonator (two poles) rung by impulses
	struct Resonator {
		float a1 = 0.0f, a2 = 0.0f, g = 1.0f, y1 = 0.0f, y2 = 0.0f;
		/// at f [Hz], its ring decaying over tau [s]; an impulse of 1 rings it at about 1
		void Set (float f, float tau, float rate) {
			float r = std::exp (-1.0f / (tau * rate)), w = 6.2831853f * f / rate;
			a1 = 2.0f * r * std::cos (w), a2 = -r * r, g = std::sin (w);
		}
		inline float Process (float x) { float y = g * x + a1 * y1 + a2 * y2; y2 = y1, y1 = y; return y; }
	};
	/// A damped mode of a structure, struck by a contact pulse
	struct Mode { float f = 0.0f, tau = 0.1f, a = 0.0f; float ph = 0.0f; };
	struct Body {
		Mode m[4];
		float d = 0.01f;      // the pulse's duration [s]
		float gain = 0.0f, t = 1e9f;
		void Strike (const Mode *modes, int n, float pulse, float g);
		float Next (float dt, float dop);
	};
	struct Voice {
		int kind = -1;
		float s = 0.0f, t = 0.0f, delay = 0.0f;
		Shot shot[2];
		Body body;
		float crunch = 0.0f;  // (HULL: a burst of grinding)
		Loop crunchLoop;
		float knock = 0.0f;   // (no recording: a knock of noise)
		dsp::OnePole knockLp;
	};
	Voice voices[8];
	dsp::Biquad crunchLp;
	void Start (int kind, float s, float delay = 0.0f);
	void Begin (Voice &v);
	float Voice1 (Voice &v, float dop);

	// rolling
	dsp::Biquad rollLp1, rollLp2, rollMidBp;
	Resonator rollRes[3];
	float rollNext = 0.0f, jointPh = 0.0f, wheelPh = 0.0f;
	// squeal
	Loop squealLoop;
	dsp::OnePole squealLvlLp, squealPitchLp;
	float squealSaw = 0.0f;
	dsp::Biquad squealBp1, squealBp2;
	// scrape
	Loop scrapeLoop;
	dsp::OnePole scrapeGrain, scrapeRumbleLp;
	dsp::Biquad scrapeBp, scrapeHp;
	// motors
	Loop motorLoop[NMOTOR];
	float motorPh[NMOTOR][3] = {};
	dsp::OnePole motorWob[NMOTOR];
	dsp::Biquad hydraulicBp;
};

} // namespace audio

#endif // !__CONTACTSYNTH_H
