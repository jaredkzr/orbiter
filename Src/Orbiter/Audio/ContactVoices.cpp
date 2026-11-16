// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ContactVoices.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	/// The body's modes over the lowest: a shell's, inharmonic (each vessel's
	/// a little off them: its seed)
	constexpr float RATIOS[ContactVoices::MODES] = {1.0f, 1.52f, 2.03f, 2.61f, 3.27f, 3.9f, 4.62f, 5.4f, 6.3f, 7.2f, 8.3f, 9.5f};
	constexpr float TAU_LOW = 0.6f, TAU_HIGH = 0.15f;   // [s] the lowest's ring, the highest's
	/// Outside (AIR): the recordings as they are; in the cabin (STRUCTURE): the
	/// recordings dulled (below 1.8 kHz: through the structure, still a blow).
	/// The body's modes off, both: rung by every blow, their lowest a note
	/// ringing on for seconds - heard as a gong, not a hull (by ear, 2026-11-15:
	/// the recordings carry their own body)
	constexpr float AIR_DIRECT = 1.0f, AIR_BODY = 0.0f;
	constexpr float STRUCT_DIRECT = 1.0f, STRUCT_BODY = 0.0f, STRUCT_CUTOFF = 1800.0f;
	/// The body's loudness: a blow's recording (full scale) rings it about
	/// as loud as itself at BODY_SCALE (Tests/Audio.ContactVoices.cpp)
	constexpr float BODY_SCALE = 0.025f;
	/// The beds in the cabin: the rolling a rumble, the brakes' groan
	constexpr float STRUCT_BED_CUTOFF[ContactVoices::BEDS] = {400.0f, 900.0f};
	/// The beds' easing [s]: up, down; their rate's
	constexpr float BED_UP = 0.1f, BED_DOWN = 0.15f, BED_RATE_T = 0.2f;
	/// A bed going to nothing is silent (its grains stop) this far under the
	/// level it fell from (-60 dB), or below BED_OFF
	constexpr float BED_CUT = 1e-3f, BED_OFF = 1e-9f;
}

float ContactVoices::BodyFreq (double size)
{
	// (60 Hz at 5 m, 25 Hz at 30 m: as size^-0.49)
	const double L = std::clamp (size, 2.0, 100.0);
	return (float)(60.0 * std::pow (5.0 / L, 0.4886));
}

ContactVoices::ContactVoices (Profile p, double size, uint32_t seed): profile (p)
{
	dsp::Rng rng (seed * 2654435761u + 1u);
	const float f0 = BodyFreq (size);
	for (int k = 0; k < MODES; k++) {
		modeF[k] = f0 * RATIOS[k] * (1.0f + 0.04f * rng.White ());
		modeTau[k] = TAU_LOW * std::pow (TAU_HIGH / TAU_LOW, (float)k / (MODES - 1));
		modeAmp[k] = (1.0f + 0.3f * rng.White ()) / std::sqrt (1.0f + k);
	}
	for (int b = 0; b < BEDS; b++) {
		bedGain[b].store (0.0f), bedRate[b].store (1.0f), bedNow[b].store (0.0f);
		beds[b].rng = dsp::Rng (seed * 7919u + 17u * (b + 1));
	}
}

SourcePtr<ContactVoices> ContactVoices::Make (Engine &e, Profile p, double size, uint32_t seed)
{
	SourcePtr<ContactVoices> s (new ContactVoices (p, size, seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

OneShot ContactVoices::Shot (int kind, const Clip *clip, float gain, float rate, float cutoff, float send, float len)
{
	OneShot s;
	s.kind = kind, s.ref = clip;
	s.v[0] = gain, s.v[1] = rate, s.v[2] = cutoff, s.v[3] = send, s.v[4] = len;
	return s;
}

bool ContactVoices::SetBedTakes (int bed, const Clip *a, float ga, const Clip *b, float gb)
{
	if (bed < 0 || bed >= BEDS || !a) return false;
	return bedTakes.Push ({bed, {a, b}, {ga, gb}});
}

void ContactVoices::SetBed (int bed, float gain, float rate)
{
	if (bed < 0 || bed >= BEDS) return;
	bedGain[bed].store (std::max (gain, 0.0f), std::memory_order_relaxed);
	bedRate[bed].store (std::clamp (rate, 0.25f, 4.0f), std::memory_order_relaxed);
}

bool ContactVoices::Fire (const OneShot &s, float delay)
{
	if (s.kind < IMPACT || s.kind > CHIRP || !s.ref) return true;   // (not one of ours: taken, nothing played)
	return starts.Push ({(const Clip*)s.ref, s.v[0], s.v[1], s.v[2], s.v[3], std::max (delay, 0.0f), std::max (s.v[4], 0.0f)});
}

void ContactVoices::SetModes ()
{
	for (int k = 0; k < MODES; k++) {
		if (modeF[k] >= 0.45f * rate) { modeAmp[k] = 0.0f; continue; }
		modes[k].Set (modeF[k], modeTau[k], rate);
		// (a mode's peak gain grows with its ring, tau rate / 2: rung by a
		// blow's spread of frequencies, each alike - as their decays' roots)
		modeIn[k] = BODY_SCALE * modeAmp[k] / std::sqrt (modeTau[k] * rate * 0.5f) * 30.0f;
	}
	dull.SetCutoff (STRUCT_CUTOFF, rate);
	xfade = std::clamp ((unsigned)(GRAIN_XFADE * rate), 16u, (unsigned)XF_TAB);
	for (int i = 0; i <= XF_TAB; i++) xfTab[i] = std::sin (0.5f * 3.14159265f * (float)i / XF_TAB);
	for (int b = 0; b < BEDS; b++)
		for (auto &f : beds[b].lp) f.SetCutoff (STRUCT_BED_CUTOFF[b], rate);
	modesSet = true;
}

void ContactVoices::Begin (const Start &s)
{
	if (!s.clip || s.clip->frames < 2 || s.clip->channels != 1) return;
	// a free voice, else the quietest's (its gain, what is left of it)
	Voice *v = nullptr;
	float least = 1e30f;
	for (auto &x : voices) {
		if (!x.clip) { v = &x; break; }
		const float left = x.gain * (1.0f - (float)(x.pos / (double)x.clip->frames));
		if (left < least) least = left, v = &x;
	}
	*v = Voice ();
	v->clip = s.clip;
	v->gain = s.gain;
	v->rate = s.rate * (float)s.clip->rate / rate;
	v->send = std::clamp (s.send, 0.0f, 1.0f);
	v->wait = (unsigned)(s.delay * rate);
	v->len = s.len > 0.0f ? std::max (1u, (unsigned)(s.len * rate)) : 0u;
	v->lp.SetCutoff (std::max (s.cutoff, 20.0f), rate);
}

void ContactVoices::NewGrain (BedState &b, Grain &g, float rateNow)
{
	// a random take, a random stretch of it that the grain (and its fade out) fits in
	const int k = b.n > 1 && b.rng.Uniform () < 0.5f ? 1 : 0;
	const Clip *c = b.take[k];
	g.clip = c, g.g = b.tg[k];
	unsigned len = (unsigned)((GRAIN_MIN + (GRAIN_MAX - GRAIN_MIN) * b.rng.Uniform ()) * rate);
	// (the most it may read, at up to 1.5 times this rate - the rate eases)
	const double step = 1.5 * std::max (rateNow, 0.25f) * (double)c->rate / rate;
	const double room = (double)c->frames - 2.0;
	if ((len + xfade) * step > room) len = (unsigned)std::max (room / step - xfade, (double)xfade);
	const double span = room - (len + xfade) * step;
	g.pos = span > 0.0 ? span * b.rng.Uniform () : 0.0;
	g.len = g.left = len + xfade;
}

float ContactVoices::BedSample (BedState &b, float dop)
{
	// the grain playing, and the next fading in over its last xfade samples
	float y = 0.0f;
	for (int j = 0; j < 2; j++) {
		Grain &g = b.gr[j];
		if (!g.clip || !g.left) continue;
		float w = 1.0f;
		const unsigned done = g.len - g.left;
		if (done < xfade && g.len > xfade && j != b.cur) w = xfTab[done * XF_TAB / xfade];               // (fading in)
		else if (g.left <= xfade && j == b.cur) w = xfTab[(g.left - 1) * XF_TAB / xfade];                 // (fading out)
		const float *d = g.clip->pcm.data ();
		const size_t i = (size_t)g.pos;
		if (i + 1 < g.clip->frames) {
			const float f = (float)(g.pos - (double)i);
			y += w * g.g * (d[i] + f * (d[i + 1] - d[i]));
		}
		g.pos += b.rate * dop * (double)g.clip->rate / rate;
		g.left--;
	}
	Grain &cur = b.gr[b.cur];
	if (cur.left == xfade) NewGrain (b, b.gr[1 - b.cur], b.rate);     // (the next begins as this one fades)
	if (!cur.left) b.cur = 1 - b.cur;
	return y;
}

void ContactVoices::Render (float *out, unsigned n, float dop)
{
	if (!modesSet) SetModes ();
	for (Start s; starts.Pop (s); ) Begin (s);
	for (BedTakes bt; bedTakes.Pop (bt); ) {
		BedState &b = beds[bt.bed];
		b.n = 0;
		for (int k = 0; k < 2; k++)
			if (bt.take[k] && bt.take[k]->channels == 1 && bt.take[k]->frames > 64) b.take[b.n] = bt.take[k], b.tg[b.n] = bt.g[k], b.n++;
		b.gr[0].clip = b.gr[1].clip = nullptr, b.gr[0].left = b.gr[1].left = 0;
	}
	// the beds' easing, per sample (one-pole); their targets this block
	float bTgt[BEDS], bRate[BEDS];
	bool bedOn = false;
	for (int k = 0; k < BEDS; k++) {
		bTgt[k] = beds[k].n ? bedGain[k].load (std::memory_order_relaxed) : 0.0f;
		bRate[k] = bedRate[k].load (std::memory_order_relaxed);
		if (bTgt[k] > 0.0f || beds[k].gain > 0.0f) bedOn = true;
	}
	const float aUp = 1.0f - std::exp (-1.0f / (BED_UP * rate)), aDown = 1.0f - std::exp (-1.0f / (BED_DOWN * rate));
	const float aRate = 1.0f - std::exp (-1.0f / (BED_RATE_T * rate));
	const float direct = profile == AIR ? AIR_DIRECT : STRUCT_DIRECT;
	const float body = profile == AIR ? AIR_BODY : STRUCT_BODY;
	const bool dulled = profile == STRUCTURE;
	int on = 0;
	for (auto &v : voices) if (v.clip) on++;
	for (unsigned i = 0; i < n; i++) {
		float y = 0.0f, x = 0.0f;
		if (on) {
			for (auto &v : voices) {
				if (!v.clip) continue;
				if (v.wait) { v.wait--; continue; }
				const float *d = v.clip->pcm.data ();
				const size_t j = (size_t)v.pos;
				const float f = (float)(v.pos - (double)j);
				float s = v.gain * v.lp.Lp (d[j] + f * (d[j + 1] - d[j]));
				if (v.len) {   // (played only so long: faded over its last 40 %)
					const unsigned fadeAt = v.len - v.len * 2 / 5;
					if (v.played >= fadeAt) s *= (float)(v.len - v.played) / (float)(v.len - fadeAt);
					v.played++;
				}
				y += s, x += s * v.send;
				v.pos += v.rate * dop;
				if (v.pos >= (double)(v.clip->frames - 1) || (v.len && v.played >= v.len)) v.clip = nullptr, on--;
			}
		}
		float b = 0.0f;
		if (body > 0.0f) for (int k = 0; k < MODES; k++) b += modes[k].Process (x * modeIn[k]);
		float bed = 0.0f;
		if (bedOn) {
			for (int k = 0; k < BEDS; k++) {
				BedState &bs = beds[k];
				bs.gain += (bTgt[k] > bs.gain ? aUp : aDown) * (bTgt[k] - bs.gain);
				bs.rate += aRate * (bRate[k] - bs.rate);
				if (bTgt[k] > 0.0f) bs.held = std::max (bs.gain, bTgt[k]);
				if (bTgt[k] <= 0.0f && bs.gain <= std::max (BED_OFF, BED_CUT * bs.held)) {   // (silent: its grains stop, from a fresh one next time)
					bs.gain = 0.0f, bs.gr[0].left = bs.gr[1].left = 0, bs.gr[0].clip = bs.gr[1].clip = nullptr;
					continue;
				}
				if (!bs.gr[bs.cur].clip || !bs.gr[bs.cur].left) { NewGrain (bs, bs.gr[bs.cur], bs.rate); bs.gr[bs.cur].len = bs.gr[bs.cur].left; }
				float v = bs.gain * BedSample (bs, dop);
				if (dulled) v = bs.lp[1].Lp (bs.lp[0].Lp (v));
				bed += v;
			}
		}
		out[i] = direct * (dulled ? dull.Lp (y) : y) + body * b + bed;
	}
	for (int k = 0; k < BEDS; k++) bedNow[k].store (beds[k].gain, std::memory_order_relaxed);
	playing.store (on, std::memory_order_relaxed);
}

} // namespace audio
