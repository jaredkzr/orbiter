// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ContactSynth.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace audio {

/// A recording decoded (mono, at the engine's rate)
struct ContactSample {
	std::vector<float> d;
};

namespace {
	constexpr float TWO_PI = 6.2831853f, PI = 3.14159265f;
	/// The output scale: rolling at 40 m/s under the full load is about 0.18
	/// RMS (gentle, 4 dB under the contacts' calibration)
	constexpr float OUT = 0.3f;
	/// A loop's seam: this long a crossfade [samples at 48 kHz]
	constexpr double SEAM = 2400.0;

	const char *SMP_NAME[] = {"clunk_heavy", "thump_door", "door_close", "bump", "latch_1", "latch_2", "latch_3", "lock", "squeal", "scrape", "motor"};

	// the recordings, shared by every synth (loaded once per rate)
	std::mutex bankMx;
	std::map<std::string, std::shared_ptr<const ContactSample>> bank;
	std::string sampleDir;

	std::shared_ptr<const ContactSample> LoadOne (const std::string &dir, const char *name, unsigned rate)
	{
		const std::string key = dir + "/" + name + "@" + std::to_string (rate);
		std::lock_guard<std::mutex> lock (bankMx);
		auto it = bank.find (key);
		if (it != bank.end ()) return it->second;
		std::shared_ptr<const ContactSample> res;
		ma_decoder_config cfg = ma_decoder_config_init (ma_format_f32, 1, rate);
		ma_decoder dec;
		const std::string path = dir + "/" + name + ".flac";
		if (ma_decoder_init_file (path.c_str (), &cfg, &dec) == MA_SUCCESS) {
			auto s = std::make_shared<ContactSample>();
			float buf[4096];
			for (;;) {
				ma_uint64 got = 0;
				ma_result r = ma_decoder_read_pcm_frames (&dec, buf, 4096, &got);
				if (got) s->d.insert (s->d.end (), buf, buf + got);
				if (r != MA_SUCCESS || got < 4096) break;
			}
			ma_decoder_uninit (&dec);
			if (s->d.size () > 2 * SEAM) res = s;
		}
		bank[key] = res;
		return res;
	}

	// the blows' bodies: a few damped low modes of the airframe (f [Hz],
	// decay [s], amplitude) and the contact pulse's duration [s] (a Hertz
	// contact: soft blows take longer and wake only the lowest modes)
	struct BodySpec { float d; int n; float f[4], tau[4], a[4]; };
	const BodySpec BODY_TOUCHDOWN = {0.012f, 4, {42.0f, 68.0f, 105.0f, 160.0f}, {0.22f, 0.14f, 0.09f, 0.05f}, {1.0f, 0.6f, 0.35f, 0.2f}};
	const BodySpec BODY_HULL = {0.018f, 4, {28.0f, 44.0f, 70.0f, 115.0f}, {0.28f, 0.2f, 0.12f, 0.07f}, {1.2f, 0.9f, 0.5f, 0.3f}};
	const BodySpec BODY_CLANG = {0.005f, 4, {60.0f, 95.0f, 150.0f, 240.0f}, {0.12f, 0.09f, 0.07f, 0.05f}, {0.7f, 0.5f, 0.4f, 0.3f}};
	const BodySpec BODY_DOCK = {0.014f, 4, {34.0f, 52.0f, 85.0f, 130.0f}, {0.18f, 0.13f, 0.09f, 0.06f}, {1.2f, 0.9f, 0.5f, 0.3f}};
	const BodySpec BODY_PUSH = {0.014f, 3, {34.0f, 52.0f, 85.0f, 0.0f}, {0.2f, 0.15f, 0.1f, 0.1f}, {0.6f, 0.45f, 0.25f, 0.0f}};
	const BodySpec BODY_GEARLOCK = {0.010f, 3, {40.0f, 66.0f, 110.0f, 0.0f}, {0.18f, 0.12f, 0.07f, 0.1f}, {0.8f, 0.5f, 0.3f, 0.0f}};
	const BodySpec BODY_LATCH = {0.003f, 3, {90.0f, 140.0f, 230.0f, 0.0f}, {0.06f, 0.05f, 0.04f, 0.1f}, {0.4f, 0.35f, 0.3f, 0.0f}};

	/// The blows' internal kinds beyond the public ones
	enum { K_DLATCH1 = ContactSynth::NBLOW, K_DLATCH2, K_RELEASE, K_PUSH, K_DOWNLOCK };

	/// A raised-cosine pulse's spectrum at x = f d (its area 1)
	float RaisedCosine (float x)
	{
		const float px = PI * x;
		const float sinc = px < 1e-4f ? 1.0f : std::sin (px) / px;
		const float den = 1.0f - x * x;
		if (std::fabs (den) < 1e-3f) return 0.5f;
		return std::fabs (sinc / den);
	}
}

// ---- the recordings -----------------------------------------------------

void ContactSynth::SetSampleDir (const std::string &dir)
{
	std::lock_guard<std::mutex> lock (bankMx);
	sampleDir = dir;
}

void ContactSynth::LoadSamples (unsigned rate)
{
	std::string dir;
	{
		std::lock_guard<std::mutex> lock (bankMx);
		if (sampleDir.empty ()) {
			const char *env = getenv ("ORBITER_CONTACT_SAMPLES");
			sampleDir = env && *env ? env : "Sound/Contacts";
		}
		dir = sampleDir;
	}
	int n = 0;
	for (int k = 0; k < NSMP; k++) {
		smp[k] = LoadOne (dir, SMP_NAME[k], rate);
		if (smp[k]) n++;
	}
	hasSamples = n == NSMP;
	static bool warned = false;
	if (!hasSamples && !warned) {
		warned = true;
		fprintf (stderr, "Audio: contact recordings missing in %s (%d of %d): modelled instead\n", dir.c_str (), n, (int)NSMP);
	}
}

float ContactSynth::Loop::Next (float rate)
{
	if (!s) return 0.0f;
	const std::vector<float> &d = s->d;
	const size_t L = d.size ();
	const double X = std::min ((double)L * 0.25, SEAM);
	const double span = (double)(L - 1) - X;
	pos += rate;
	while (pos >= span) pos -= span;
	while (pos < 0.0) pos += span;
	auto rd = [&](double p) { size_t i = (size_t)p; float f = (float)(p - (double)i); return d[i] + f * (d[i + 1] - d[i]); };
	float y = rd (pos);
	if (pos < X) {
		// (the seam: fading in over the end of the previous pass)
		float w = (float)(pos / X);
		y = w * y + (1.0f - w) * rd (pos + span);
	}
	return y;
}

bool ContactSynth::Shot::Alive () const
{
	return s && gain > 1e-3f && pos < (double)(s->d.size () - 1);
}

float ContactSynth::Shot::Next ()
{
	const std::vector<float> &d = s->d;
	size_t i = (size_t)pos;
	float f = (float)(pos - (double)i);
	float y = d[i] + f * (d[i + 1] - d[i]);
	pos += rate;
	rate *= glide;
	if (att < 1.0f) att = std::min (1.0f, att + attK);
	float out = gain * att * tone.Lp (y);
	gain *= decay;
	return out;
}

void ContactSynth::Body::Strike (const Mode *modes, int n, float pulse, float g)
{
	for (int i = 0; i < 4; i++) {
		m[i] = i < n ? modes[i] : Mode ();
		m[i].ph = 0.0f;
		// (the pulse's spectrum at the mode: its share of the blow)
		m[i].a *= RaisedCosine (m[i].f * pulse);
	}
	d = pulse, gain = g, t = 0.0f;
}

float ContactSynth::Body::Next (float dt, float dop)
{
	if (gain <= 0.0f) return 0.0f;
	float y = 0.0f;
	bool alive = false;
	const float onset = 1.0f - std::exp (-t / (0.3f * d));
	for (auto &mo : m) {
		if (mo.a <= 0.0f) continue;
		const float env = std::exp (-t / mo.tau);
		if (env > 2e-3f) alive = true;
		mo.ph += TWO_PI * mo.f * dop * dt;
		if (mo.ph > TWO_PI) mo.ph -= TWO_PI;
		y += mo.a * env * std::sin (mo.ph);
	}
	t += dt;
	if (!alive) gain = 0.0f;
	return gain * onset * y;
}

// ---- the synth ------------------------------------------------------------

std::unique_ptr<ContactSynth> ContactSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<ContactSynth> s (new ContactSynth (seed));
	if (!s->Init (e)) return nullptr;
	s->LoadSamples (e.SampleRate ());
	s->squealLoop.s = s->smp[S_SQUEAL].get ();
	s->scrapeLoop.s = s->smp[S_SCRAPE].get ();
	for (int k = 0; k < NMOTOR; k++) {
		s->motorLoop[k].s = s->smp[S_MOTOR].get ();
		s->motorLoop[k].pos = s->smp[S_MOTOR] ? (double)(s->smp[S_MOTOR]->d.size () / 3 * k) : 0.0;
	}
	return s;
}

void ContactSynth::Set (const Params &p)
{
	pRollSpeed.store (std::max (0.0f, p.rollSpeed), std::memory_order_relaxed);
	pRollLoad.store (std::clamp (p.rollLoad, 0.0f, 2.0f), std::memory_order_relaxed);
	pSqueal.store (std::clamp (p.squeal, 0.0f, 1.0f), std::memory_order_relaxed);
	pScrape.store (std::clamp (p.scrape, 0.0f, 1.0f), std::memory_order_relaxed);
	pScrapeSpeed.store (std::max (0.0f, p.scrapeSpeed), std::memory_order_relaxed);
	for (int k = 0; k < NMOTOR; k++) pMotor[k].store (std::clamp (p.motor[k], 0.0f, 1.0f), std::memory_order_relaxed);
}

void ContactSynth::Strike (Blow kind, float strength)
{
	if (kind < 0 || kind >= NBLOW) return;
	pending[kind].strength.store (std::clamp (strength, 0.0f, 1.5f), std::memory_order_relaxed);
	pending[kind].count.fetch_add (1, std::memory_order_release);
}

void ContactSynth::Start (int kind, float s, float delay)
{
	// (into a free voice, or the quietest)
	Voice *v = nullptr;
	for (auto &x : voices) if (x.kind < 0) { v = &x; break; }
	if (!v) {
		v = &voices[0];
		for (auto &x : voices) if (x.s < v->s) v = &x;
	}
	*v = Voice ();
	v->kind = kind, v->s = s, v->delay = delay;
}

void ContactSynth::Begin (Voice &v)
{
	// the voice's recording(s) and body, by kind, at its strength: a soft
	// blow is quieter and duller
	const float s = std::max (v.s, 0.0f), s1 = std::min (s, 1.0f);
	const float tone = 900.0f + 7000.0f * s1;
	auto vary = [&](float r) { return r * (0.96f + 0.08f * rng.Uniform ()); }; // (no two alike)
	auto shot = [&](Shot &sh, Smp k, float gain, float r, float toneHz, float decayTau = 0.0f, float glideTo = 1.0f, float glideOver = 0.0f, float attack = 0.0f) {
		sh.s = smp[k].get ();
		sh.pos = 0.0, sh.gain = gain, sh.rate = r;
		sh.decay = decayTau > 0.0f ? std::exp (-1.0f / (decayTau * rate)) : 1.0f;
		sh.glide = glideOver > 0.0f ? std::exp (std::log (glideTo) / (glideOver * rate)) : 1.0f;
		sh.att = attack > 0.0f ? 0.0f : 1.0f;
		sh.attK = attack > 0.0f ? 1.0f / (attack * rate) : 1.0f;
		sh.tone.SetCutoff (toneHz, rate);
		sh.tone.y = 0.0f;
	};
	auto body = [&](const BodySpec &b, float g) {
		Mode m[4];
		for (int i = 0; i < b.n; i++) m[i].f = b.f[i], m[i].tau = b.tau[i], m[i].a = b.a[i];
		v.body.Strike (m, b.n, b.d * (0.8f + 0.6f * (1.0f - s1)), g);
	};
	switch (v.kind) {
	case TOUCHDOWN:
		// the strut's thump under the body, and the tyres' chirp as they spin
		// up: nearly as loud for a gentle touchdown as a hard one (it is the
		// speed that makes it), its pitch dropping as the slip does
		body (BODY_TOUCHDOWN, 0.7f * std::pow (s1, 0.7f));
		shot (v.shot[0], S_THUMP, 1.6f * s1, vary (0.85f), tone);
		shot (v.shot[1], S_SQUEAL, 6.0f * std::max (0.6f, std::min (s, 1.0f)), vary (0.7f), 9000.0f, 0.2f, 0.72f, 0.5f, 0.012f);
		v.knock = 0.0f;
		break;
	case HULL:
		// a heavy slam and the structure's rattle, a crunch of grinding
		body (BODY_HULL, 1.2f * std::pow (s1, 0.7f));
		shot (v.shot[0], S_CLUNK, 6.0f * s1, vary (0.9f), tone);
		v.crunch = 3.0f * s1;
		v.crunchLoop.s = smp[S_SCRAPE].get ();
		v.crunchLoop.pos = v.crunchLoop.s ? rng.Uniform () * (double)(v.crunchLoop.s->d.size () / 2) : 0.0;
		break;
	case CLANG:
		// a vessel bumping another: a dull bump
		body (BODY_CLANG, 0.8f * s1);
		shot (v.shot[0], S_BUMP, 4.0f * s1, vary (0.95f), tone);
		break;
	case DOCK:
		// two structures meeting: a heavy, dull clunk and the structure
		// settling
		body (BODY_DOCK, 1.8f * s1);
		shot (v.shot[0], S_CLUNK, 9.0f * s1, vary (1.0f), tone);
		break;
	case K_PUSH:
		// undocking's push-off: a lighter thud
		body (BODY_PUSH, 0.7f * s1);
		shot (v.shot[0], S_THUMP, 2.5f * s1, vary (0.9f), tone);
		break;
	case GEARLOCK:
		// the gear locking: a heavy thunk
		body (BODY_GEARLOCK, 0.9f * s1);
		shot (v.shot[0], S_DOORCLOSE, 3.5f * s1, vary (0.8f), tone);
		break;
	case K_DOWNLOCK:
		// the downlock: a short, hard clack
		body (BODY_LATCH, 0.5f * s1);
		shot (v.shot[0], S_LOCK, 2.0f * s1, vary (1.0f), tone);
		break;
	case LATCH:
		// a door's latch: its thump and click
		body (BODY_LATCH, 0.6f * s1);
		shot (v.shot[0], S_DOORCLOSE, 3.0f * s1, vary (1.05f), tone);
		break;
	case K_DLATCH1:
		// a docking latch engaging: a hard, mid-heavy clack with a body
		body (BODY_LATCH, 1.0f * s1);
		shot (v.shot[0], S_LATCH1, 4.5f * s1, vary (1.0f), tone);
		break;
	case K_DLATCH2:
		body (BODY_LATCH, 1.0f * s1);
		shot (v.shot[0], S_LATCH2, 4.5f * s1, vary (1.0f), tone);
		break;
	case K_RELEASE:
		// a latch letting go
		body (BODY_LATCH, 0.8f * s1);
		shot (v.shot[0], S_LATCH3, 4.5f * s1, vary (1.0f), tone);
		break;
	default: v.kind = -1; return;
	}
	// (no recording: a knock of noise stands in for it)
	if (!v.shot[0].s) v.knock = 2.0f * s1;
	v.knockLp.SetCutoff (tone, rate);
}

float ContactSynth::Voice1 (Voice &v, float dop)
{
	// one sample of a blow; ends the voice when it has died away
	const float dt = 1.0f / rate;
	if (v.t == 0.0f) Begin (v);
	if (v.kind < 0) return 0.0f;
	float y = v.body.Next (dt, dop);
	bool alive = v.body.gain > 0.0f;
	for (auto &sh : v.shot) {
		if (!sh.Alive ()) continue;
		y += sh.Next ();
		alive = true;
	}
	if (v.knock > 1e-3f) {
		// (a burst of noise, a few milliseconds)
		y += v.knock * v.knockLp.Lp (rng.White ()) * 3.0f;
		v.knock *= std::exp (-dt / 0.006f);
		alive = true;
	}
	if (v.crunch > 1e-3f) {
		// (HULL: grinding, in grains, dying over a quarter second)
		float g = v.crunchLoop.s ? v.crunchLoop.Next (1.1f * dop) : crunchLp.Process (rng.White ()) * 3.0f;
		y += v.crunch * g * std::max (0.0f, 1.0f + 5.0f * scrapeGrain.y);
		v.crunch *= std::exp (-dt / 0.22f);
		alive = true;
	}
	v.t += dt;
	if (!alive) v.kind = -1;
	return y;
}

void ContactSynth::Render (float *out, unsigned n, float dop)
{
	// new blows (docking: the clunk, then two latches; undocking: the latches,
	// then the push-off; the gear locking: the thunk, then the downlock)
	for (int k = 0; k < NBLOW; k++) {
		int c = pending[k].count.load (std::memory_order_acquire);
		if (c == played[k]) continue;
		played[k] = c;
		float s = pending[k].strength.load (std::memory_order_relaxed);
		if (k == DOCK) { Start (DOCK, s); Start (K_DLATCH1, 1.0f, 0.45f); Start (K_DLATCH2, 1.0f, 0.62f); }
		else if (k == UNDOCK) { Start (K_RELEASE, 1.0f); Start (K_DLATCH2, 0.9f, 0.15f); Start (K_PUSH, 0.9f * s, 0.4f); }
		else if (k == GEARLOCK) { Start (GEARLOCK, s); Start (K_DOWNLOCK, 0.8f * s, 0.12f); }
		else Start (k, s);
	}

	const float speed = rollS.Step (pRollSpeed.load (std::memory_order_relaxed), dsp::Smooth::K (0.1f, n, rate));
	const float load = loadS.Step (pRollLoad.load (std::memory_order_relaxed), dsp::Smooth::K (0.1f, n, rate));
	// (a knee under the squeal and the scrape: a vessel at rest creeping a
	// hair, the solver's residue, makes no sound)
	const float squeal = std::max (0.0f, squealS.Step (pSqueal.load (std::memory_order_relaxed), dsp::Smooth::K (0.03f, n, rate)) - 0.01f) / 0.99f;
	const float scrape = std::max (0.0f, scrapeS.Step (pScrape.load (std::memory_order_relaxed), dsp::Smooth::K (0.05f, n, rate)) - 0.01f) / 0.99f;
	const float sspeed = pScrapeSpeed.load (std::memory_order_relaxed);
	const float dt = 1.0f / rate;

	// rolling: the runway's texture under the tyres - a dense train of soft
	// impacts (the aggregate, the tread: every half metre or so, random)
	// each ringing the tyre's hop and the strut's low modes, the slabs'
	// joints (every 6 m) heavier ones; under it a low rumble (its band
	// widening a little with the speed); all of it swelling with the wheels'
	// turn (a tyre isn't quite round: 2.5 m around)
	const float rollFc = std::clamp (70.0f + 3.5f * speed, 80.0f, 330.0f) * dop;
	rollLp1.LowPass (rollFc, 0.55f, rate);
	rollLp2.LowPass (rollFc, 0.55f, rate);
	rollMidBp.BandPass (480.0f * dop, 1.2f, rate);
	const float rollAmp = std::sqrt (std::max (load, 0.0f)) * std::pow (std::min (speed / 40.0f, 1.5f), 1.3f);
	const float impactRate = std::max (speed / 0.5f, 1.0f) * dop, jointRate = speed / 6.0f * dop, wheelRate = speed / 2.5f * dop;
	static const float ROLL_F[3] = {36.0f, 82.0f, 150.0f}, ROLL_TAU[3] = {0.05f, 0.025f, 0.012f}, ROLL_A[3] = {1.0f, 0.55f, 0.25f};
	for (int i = 0; i < 3; i++) rollRes[i].Set (ROLL_F[i] * dop, ROLL_TAU[i], rate);
	// squeal: the recording looped, its pitch wandering a few per cent and
	// its level a third as the rubber sticks and slips (a model without it:
	// a stick-slip sawtooth ringing the tyre's modes)
	squealPitchLp.SetCutoff (6.0f, rate);
	squealLvlLp.SetCutoff (14.0f, rate);
	squealBp1.BandPass (1050.0f * dop, 6.0f, rate);
	squealBp2.BandPass (2150.0f * dop, 8.0f, rate);
	// scrape: metal dragged (the recording looped, its pace with the speed)
	// in grains, over the hull's low rumble (a model without it: mid noise
	// in grains)
	const float scrapeRate = std::clamp (0.55f + sspeed / 50.0f, 0.55f, 1.3f) * dop;
	scrapeGrain.SetCutoff (25.0f, rate);
	scrapeRumbleLp.SetCutoff (110.0f * dop, rate);
	scrapeBp.BandPass (std::clamp (250.0f + 6.0f * sspeed, 250.0f, 900.0f) * dop, 1.0f, rate);
	scrapeHp.HighPass (120.0f * dop, 0.7f, rate);
	crunchLp.LowPass (900.0f * dop, 0.7f, rate);
	// motors: the recording looped at a pitch by kind (a servo high, a door's
	// actuator lower, the gear's hydraulics deep, with a pump's whine and the
	// fluid's hiss), up 35 % at full speed, wavering with the load
	static const float MOTOR_RATE[NMOTOR] = {1.35f, 1.0f, 0.5f}, MOTOR_GAIN[NMOTOR] = {0.9f, 1.05f, 2.4f}, MOTOR_F0[NMOTOR] = {180.0f, 140.0f, 90.0f};
	float mspeed[NMOTOR], mrate[NMOTOR];
	bool motors = false;
	for (int k = 0; k < NMOTOR; k++) {
		mspeed[k] = motorS[k].Step (pMotor[k].load (std::memory_order_relaxed), dsp::Smooth::K (0.08f, n, rate));
		mrate[k] = MOTOR_RATE[k] * (0.85f + 0.35f * mspeed[k]) * dop;
		motorWob[k].SetCutoff (3.0f, rate);
		if (mspeed[k] > 1e-3f) motors = true;
	}
	hydraulicBp.BandPass (450.0f * dop, 1.5f, rate);

	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		float s = 0.0f;
		if (rollAmp > 1e-4f) {
			float impulse = 0.0f;
			rollNext -= 1.0f;
			if (rollNext <= 0.0f) {
				// (the next impact: a random interval, a random size, mostly small)
				rollNext = -std::log (std::max (rng.Uniform (), 1e-6f)) * rate / impactRate;
				float u = rng.Uniform ();
				impulse = u * u * (0.4f + 0.6f * rng.Uniform ());
			}
			jointPh += jointRate / rate;
			if (jointPh >= 1.0f) jointPh -= 1.0f, impulse += 2.0f;
			wheelPh += wheelRate / rate;
			if (wheelPh >= 1.0f) wheelPh -= 1.0f;
			const float turn = 1.0f + 0.2f * std::sin (TWO_PI * wheelPh);
			float ring = 0.0f;
			for (int k = 0; k < 3; k++) ring += ROLL_A[k] * rollRes[k].Process (impulse);
			const float texture = rollLp2.Process (rollLp1.Process (p));
			s += rollAmp * turn * (0.75f * texture + 0.9f * ring + 0.06f * rollMidBp.Process (impulse));
		}
		if (squeal > 1e-4f) {
			const float pitch = std::clamp (25.0f * squealPitchLp.Lp (w), -1.0f, 1.0f);
			const float lvl = 1.0f + 0.35f * std::clamp (15.0f * squealLvlLp.Lp (rng.White ()), -1.0f, 1.0f);
			float y;
			if (squealLoop.s) y = 4.0f * squealLoop.Next (0.75f * dop * (1.0f + 0.04f * pitch));
			else {
				// (stick-slip: a sawtooth at the squeal's pitch, the tyre's modes rung by it)
				squealSaw += 1100.0f * dop * (1.0f + 0.04f * pitch) / rate;
				if (squealSaw >= 1.0f) squealSaw -= 1.0f;
				const float saw = 2.0f * squealSaw - 1.0f;
				y = 0.5f * squealBp1.Process (saw) + 0.25f * squealBp2.Process (saw) + 0.15f * saw;
			}
			s += squeal * lvl * y;
		}
		if (scrape > 1e-4f) {
			const float grains = std::max (0.0f, 1.0f + 5.0f * scrapeGrain.Lp (rng.White ()));
			float y;
			if (scrapeLoop.s) y = 5.0f * scrapeLoop.Next (scrapeRate);
			else y = 2.0f * scrapeBp.Process (w);
			s += scrape * (grains * y + 4.0f * scrapeHp.Process (scrapeRumbleLp.Lp (p)));
		} else scrapeGrain.Lp (rng.White ());
		if (motors) {
			for (int k = 0; k < NMOTOR; k++) {
				if (mspeed[k] <= 1e-3f) continue;
				// (its load wavering: a few per cent in pitch, a quarter in level)
				const float wob = std::clamp (60.0f * motorWob[k].Lp (rng.White ()), -1.0f, 1.0f);
				float y;
				if (motorLoop[k].s) y = motorLoop[k].Next (mrate[k] * (1.0f + 0.03f * wob));
				else {
					y = 0.0f;
					for (int h = 0; h < 3; h++) {
						motorPh[k][h] += TWO_PI * MOTOR_F0[k] * (0.85f + 0.35f * mspeed[k]) * dop * (1.0f + 0.03f * wob) * (h + 1) / rate;
						if (motorPh[k][h] > TWO_PI) motorPh[k][h] -= TWO_PI;
						y += 0.3f * std::sin (motorPh[k][h]) / (1.0f + 1.5f * h);
					}
				}
				if (k == GEAR) {
					// (the hydraulics: the pump's whine under the motor, the fluid's hiss)
					motorPh[k][0] += TWO_PI * MOTOR_F0[k] * (0.85f + 0.35f * mspeed[k]) * dop * (1.0f + 0.03f * wob) / rate;
					if (motorPh[k][0] > TWO_PI) motorPh[k][0] -= TWO_PI;
					y += 0.12f * std::sin (motorPh[k][0]) + 0.3f * hydraulicBp.Process (p);
				}
				s += MOTOR_GAIN[k] * mspeed[k] * (1.0f + 0.25f * wob) * y;
			}
		}
		for (auto &v : voices) {
			if (v.kind < 0) continue;
			if (v.delay > 0.0f) { v.delay -= dt; continue; }
			s += Voice1 (v, dop);
		}
		out[i] = OUT * s;
	}
}

} // namespace audio
