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
}

SourcePtr<ContactVoices> ContactVoices::Make (Engine &e, Profile p, double size, uint32_t seed)
{
	SourcePtr<ContactVoices> s (new ContactVoices (p, size, seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

OneShot ContactVoices::Shot (int kind, const Clip *clip, float gain, float rate, float cutoff, float send)
{
	OneShot s;
	s.kind = kind, s.ref = clip;
	s.v[0] = gain, s.v[1] = rate, s.v[2] = cutoff, s.v[3] = send;
	return s;
}

bool ContactVoices::Fire (const OneShot &s, float delay)
{
	if (s.kind < IMPACT || s.kind > RATTLE || !s.ref) return true;   // (not one of ours: taken, nothing played)
	return starts.Push ({(const Clip*)s.ref, s.v[0], s.v[1], s.v[2], s.v[3], std::max (delay, 0.0f)});
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
	v->lp.SetCutoff (std::max (s.cutoff, 20.0f), rate);
}

void ContactVoices::Render (float *out, unsigned n, float dop)
{
	if (!modesSet) SetModes ();
	for (Start s; starts.Pop (s); ) Begin (s);
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
				const float s = v.gain * v.lp.Lp (d[j] + f * (d[j + 1] - d[j]));
				y += s, x += s * v.send;
				v.pos += v.rate * dop;
				if (v.pos >= (double)(v.clip->frames - 1)) v.clip = nullptr, on--;
			}
		}
		float b = 0.0f;
		if (body > 0.0f) for (int k = 0; k < MODES; k++) b += modes[k].Process (x * modeIn[k]);
		out[i] = direct * (dulled ? dull.Lp (y) : y) + body * b;
	}
	playing.store (on, std::memory_order_relaxed);
}

} // namespace audio
