// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// A transmission's sound: the radio (quindar tones, the band, static, clipping), the link
// degrading far from Earth. Apart from Radio.cpp so the synth tests can build it alone.

#include "Radio.h"
#include "Loudness.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
	/// Quindar tones: 2525 Hz as the ground keys its transmitter, 2475 Hz as
	/// it lets go (250 ms each)
	constexpr float QUINDAR_IN = 2525.0f, QUINDAR_OUT = 2475.0f, QUINDAR_LEN = 0.25f, QUINDAR_GAP = 0.08f;
	/// The squelch's tail (the carrier gone: noise for a moment)
	constexpr float SQUELCH_LEN = 0.12f;
	/// The radio's output: the voice's RMS on the radio bus (callouts' level)
	constexpr float RADIO_RMS = 0.08f;
	/// Each transmission's voice brought to this loudness first (BS.1770:
	/// recordings of all levels alike; about RADIO_RMS for speech)
	constexpr double RADIO_LUFS = -20.0;
	/// After the channel's clipping (its peaks at half scale): up to this
	/// (the radio about the callouts' loudness, a little under)
	constexpr float RADIO_OUT = 1.7f;
	/// capcom2's radio (chosen by ear, "more" of the variants: "lets add more"): narrower, harder, more static
	const RadioSynth::Sound CAPCOM2_RADIO = { 400.0f, 3000.0f, 2, 3.0f, 40.0f };
}

// ---- a transmission -------------------------------------------------------

std::unique_ptr<RadioSynth> RadioSynth::Make (Engine &e, std::vector<float> &&buf, bool quindar, float snr, uint32_t seed, const Sound &sound)
{
	std::unique_ptr<RadioSynth> s (new RadioSynth (std::move (buf), quindar, snr, seed, sound));
	if (!s->Init (e)) return nullptr;
	const double L = Loudness (s->buf.data (), s->buf.size (), 1, e.SampleRate ());
	s->gain = std::isfinite (L) ? (float)std::pow (10.0, std::clamp (RADIO_LUFS - L, -20.0, 20.0) / 20.0) : 1.0f;
	s->Link (e.SampleRate ());
	return s;
}

void RadioSynth::Link (unsigned r)
{
	// far from Earth the link degrades, from 40 dB S/N to the Moon's (about 28 dB) and no further (by
	// ear: L2, Mars sound as the Moon). A deep-space link trades data rate for S/N: the voice comes
	// narrower and coarser (a codec), fading and dropping out now and then, but clean of static
	const float moon = std::clamp (((float)Snr (3.844e8) - 10.0f) / 30.0f, 0.0f, 1.0f);
	link = std::clamp ((snrDb - 10.0f) / 30.0f, moon, 1.0f);
	snrDb = std::max (snrDb, (float)Snr (3.844e8));
	bandHp = sound.hp + 250.0f * (1.0f - link), bandLp = sound.lp * (0.4f + 0.6f * link);
	static const char *codec = getenv ("ORBITER_RADIO_CODEC");     // (listening tests: 0 off)
	if (link < 1.0f && !(codec && *codec == '0')) {
		hold = std::max (1u, (unsigned)(r / (5.0f * bandLp)));         // (a sample rate of 5 x the band)
		levels = std::pow (2.0f, 10.0f + 5.0f * link);                  // (15 bits near, about 13 at the Moon; by ear)
	}
	if (link >= 1.0f || buf.empty ()) return;
	// as loud as near Earth: the loudness the narrower band takes away (the voice through each
	// band, measured), and the fading's on average, made up
	auto through = [&](float hp, float lp) {
		dsp::Biquad a, b, c, d;
		a.HighPass (hp, 0.7f, (float)r), b.HighPass (hp, 0.7f, (float)r), c.LowPass (lp, 0.7f, (float)r), d.LowPass (lp, 0.7f, (float)r);
		std::vector<float> y (buf.size ());
		for (size_t i = 0; i < buf.size (); i++) {
			float v = a.Process (buf[i]);
			if (sound.stages > 1) v = b.Process (v);
			v = c.Process (v);
			if (sound.stages > 1) v = d.Process (v);
			y[i] = v;
		}
		return Loudness (y.data (), y.size (), 1, r);
	};
	const double near = through (sound.hp, sound.lp), far = through (bandHp, bandLp);
	const float bad = 1.0f - link;
	const float fadeMean = 1.0f - 0.5f * 0.35f * bad;                 // (the fading's average)
	const float dropped = 0.6f * bad * bad * 0.145f;                   // (the dropouts' share of the time)
	if (std::isfinite (near) && std::isfinite (far))
		gain *= (float)std::pow (10.0, std::clamp (near - far, 0.0, 12.0) / 20.0) / fadeMean / std::sqrt (1.0f - dropped);
}

double RadioSynth::Length () const
{
	const double tones = quindar ? 2.0 * (QUINDAR_LEN + QUINDAR_GAP) : 0.0;
	return tones + buf.size () / (double)rate + SQUELCH_LEN;
}

void RadioSynth::Render (float *out, unsigned n, float dop)
{
	if (!setup) {
		setup = true;
		hp1.HighPass (bandHp, 0.7f, rate), hp2.HighPass (bandHp, 0.7f, rate);
		lp1.LowPass (bandLp, 0.7f, rate), lp2.LowPass (bandLp, 0.7f, rate);
		hissBp.BandPass (1800.0f, 0.5f, rate);
	}
	const float bad = 1.0f - link;
	const float fadeDepth = 0.35f * bad, dropRate = 0.6f * bad * bad; // [per second]
	const float fadeK = 1.0f - std::exp (-1.0f / (0.12f * rate)), dropK = 1.0f - std::exp (-1.0f / (0.008f * rate));
	const size_t nTone = quindar ? (size_t)((QUINDAR_LEN + QUINDAR_GAP) * rate) : 0;
	const size_t nVoice = buf.size (), nTail = (size_t)(SQUELCH_LEN * rate);
	const size_t total = 2 * nTone + nVoice + nTail;
	const float hiss = std::pow (10.0f, -std::max (snrDb, 30.0f) / 20.0f) * 2.0f; // (static against the voice, band-limited)
	if (cut.load (std::memory_order_acquire) && !cutAt) cutAt = pos + (size_t)(0.15f * rate);
	for (unsigned i = 0; i < n; i++, pos++) {
		if (pos >= total || (cutAt && pos >= cutAt)) { out[i] = 0.0f; continue; }
		float x = 0.0f, tone = 0.0f, carrier = 1.0f;
		const float tt = (float)pos / rate;
		if (pos < nTone) {
			if (tt < QUINDAR_LEN) tone = 0.5f * std::sin (TWO_PI * QUINDAR_IN * tt);
		} else if (pos < nTone + nVoice) {
			x = buf[pos - nTone] * gain;
		} else if (pos < 2 * nTone + nVoice) {
			const float to = (float)(pos - nTone - nVoice) / rate - QUINDAR_GAP;
			if (to >= 0.0f && to < QUINDAR_LEN) tone = 0.5f * std::sin (TWO_PI * QUINDAR_OUT * to);
		} else carrier = 0.0f; // (the squelch's tail)
		// (a degraded link: the signal fades slowly, and drops out now and then under the static)
		if (bad > 0.0f) {
			if (pos >= fadeNext) { fadeTarget = 1.0f - fadeDepth * rng.Uniform (); fadeNext = pos + (size_t)((0.15f + 0.35f * rng.Uniform ()) * rate); }
			fade += fadeK * (fadeTarget - fade);
			if (pos >= dropEnd && rng.Uniform () < dropRate / rate) dropEnd = pos + (size_t)((0.04f + 0.21f * rng.Uniform ()) * rate);
			drop += dropK * ((pos < dropEnd ? 0.05f : 1.0f) - drop);
			x *= fade * drop, tone *= fade * drop;
		}
		// (lost: a burst of static, then the squelch shuts)
		float noise = hiss * RADIO_RMS;
		if (cutAt || carrier == 0.0f) noise = 0.6f * RADIO_RMS;
		const float hs = noise * 3.0f * hissBp.Process (rng.White ());
		float y = hp1.Process (x + hs);
		if (sound.stages > 1) y = hp2.Process (y);
		y = lp1.Process (y);
		if (sound.stages > 1) y = lp2.Process (y);
		if (hold > 1 || levels > 0.0f) {                               // (the low data rate's codec)
			const float fs = 4.0f * RADIO_RMS;                          // (the codec's full scale: the voice's peaks)
			if (held++ % hold == 0) heldY = levels > 0.0f ? std::round (y / fs * levels) / levels * fs : y;
			y = heldY;
		}
		y += tone * RADIO_RMS * 1.5f * carrier;
		// (the channel's clipping: a little)
		if (sound.drive > 0.0f) y = std::tanh (sound.drive * y) / sound.drive;
		out[i] = RADIO_OUT * y;
	}
	if (pos >= total || (cutAt && pos >= cutAt)) done.store (true, std::memory_order_release);
}

double RadioSynth::Snr (double d)
{
	// (the received power falls as 1/d^2: the signal to noise as 20 log10 d;
	// ground stations' dishes and gain keep it usable far out)
	return std::clamp (80.0 - 20.0 * std::log10 (std::max (d, 1e6) / 1e6), 10.0, 50.0);
}

RadioSynth::Sound RadioSynth::SoundFor (const std::string &file)
{
	// (each CAPCOM's radio its own sound: c2's harder, by ear; c1 and the chatter the default)
	return file.find ("_c2_") != std::string::npos ? CAPCOM2_RADIO : RadioSynth::Sound ();
}

} // namespace audio
