// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MusicSynth.h"
#include "AudioEngine.h"
#include <algorithm>
#include <cmath>

namespace audio {
namespace music {

const char *Name (Mood m)
{
	static const char *n[NMOOD] = { "silent", "menu", "ground", "ascent", "orbit", "deep", "approach", "reentry" };
	return m >= 0 && m < NMOOD ? n[m] : "?";
}

namespace {

constexpr float PI = 3.14159265f;

// ---- oscillators: a sine table, phases as 32-bit fractions of a turn ----
constexpr int TBITS = 12, TSIZE = 1 << TBITS, FBITS = 32 - TBITS;
struct SineTable {
	float v[TSIZE + 1];
	SineTable () { for (int i = 0; i <= TSIZE; i++) v[i] = (float)std::sin (2.0 * 3.14159265358979 * i / TSIZE); }
};
const float *Table () { static const SineTable t; return t.v; }
inline float Sine (const float *tb, uint32_t ph)
{
	const uint32_t i = ph >> FBITS;
	const float f = (float)(ph & ((1u << FBITS) - 1)) * (1.0f / (float)(1u << FBITS));
	return tb[i] + f * (tb[i + 1] - tb[i]);
}
inline float MidiHz (float m) { return 440.0f * std::exp2 ((m - 69.0f) / 12.0f); }
inline uint32_t Inc (double hz, float rate) { return (uint32_t)(int64_t)std::llround (std::min (hz, 0.49 * rate) / rate * 4294967296.0); }

// ---- harmony -------------------------------------------------------------
enum Mode { IONIAN, LYDIAN, MIXOLYDIAN, DORIAN, AEOLIAN, PHRYGIAN };
const int SCALE[6][7] = {
	{ 0, 2, 4, 5, 7, 9, 11 },   // Ionian
	{ 0, 2, 4, 6, 7, 9, 11 },   // Lydian: bright, floating (the raised fourth)
	{ 0, 2, 4, 5, 7, 9, 10 },   // Mixolydian: open, earthy
	{ 0, 2, 3, 5, 7, 9, 10 },   // Dorian: minor, unsettled, hopeful
	{ 0, 2, 3, 5, 7, 8, 10 },   // Aeolian: minor, wide
	{ 0, 1, 3, 5, 7, 8, 10 },   // Phrygian: dark (the flat second)
};

/// Voicings: a chord's tones as scale steps above its root (three-note
/// pads leave out the first: the bass has the root)
enum Voicing { ADD9, SEVENTH, OPEN, SUS2, SUS4, QUARTAL, NVOICING };
const int VOICE[NVOICING][4] = {
	{ 0, 2, 4, 8 },     // root, third, fifth, ninth
	{ 0, 2, 4, 6 },     // root, third, fifth, seventh
	{ 0, 4, 7, 8 },     // root, fifth, octave, ninth
	{ 0, 1, 4, 7 },     // root, second, fifth, octave
	{ 0, 3, 4, 7 },     // root, fourth, fifth, octave
	{ 0, 3, 6, 9 },     // stacked fourths
};

struct MoodDef {
	Mode mode;
	float degree[7];            // the chord roots' weights, by scale degree (0: never - a diminished fifth)
	float voicing[NVOICING];
	float chordMin, chordMax;   // [s]
	float rest;                 // a chord's chance to be a rest (the pads silent)
	int padCenter;              // the pads' register (MIDI: their mean)
	int notes;                  // pad notes (3 or 4)
	float bright;               // the pads' partials' roll-off (higher: brighter)
	float attack, release;      // the pads' [s]
	float pad, bass;            // levels
	int bassLow;                // the bass's lowest note (MIDI)
	bool pedal;                 // the bass on the tonic throughout
	float bellsPerMin; int bellCenter; float bell;
	float pulseRate, pulse;     // notes per second; level
	float wet;                  // the reverb's share
	float level;                // the whole
};

const MoodDef MOODS[NMOOD] = {
	// SILENT (the orbit's, at no level)
	{ LYDIAN, { 3, 2, 1, 0, 1.2f, 2, 0.8f }, { 3, 2, 2, 1, 0, 1 }, 11, 17, 0.1f, 67, 4, 3.5f, 4, 9, 0.9f, 0.55f, 38, false, 0, 84, 0.5f, 0, 0, 0.6f, 0.0f },
	// MENU: Lydian, warm and open
	{ LYDIAN, { 3, 2, 1.2f, 0, 1.5f, 2, 0.8f }, { 3, 2, 1, 1, 0, 0.5f }, 9, 14, 0.0f, 64, 4, 4.0f, 3, 7, 1.0f, 0.45f, 38, false, 4, 81, 0.5f, 0, 0, 0.5f, 1.0f },
	// GROUND: Mixolydian, mellow, sparse
	{ MIXOLYDIAN, { 3, 1, 0, 2, 1, 1, 2 }, { 2, 1, 2, 1, 1, 0 }, 12, 18, 0.15f, 60, 3, 2.8f, 5, 9, 0.85f, 0.45f, 36, false, 1.5f, 76, 0.4f, 0, 0, 0.45f, 0.75f },
	// ASCENT: Aeolian over a pedal, a pulse driving it on
	{ AEOLIAN, { 3, 0, 1, 1.5f, 0.7f, 2, 2 }, { 1, 0.5f, 3, 2, 1, 1 }, 6, 9, 0.0f, 62, 4, 3.0f, 2, 4, 0.9f, 0.6f, 33, true, 0, 79, 0.0f, 3.0f, 0.55f, 0.3f, 1.0f },
	// ORBIT: Lydian, high and spacious, bells
	{ LYDIAN, { 3, 2, 1, 0, 1.2f, 2, 0.8f }, { 3, 2, 2, 1, 0, 1 }, 11, 17, 0.1f, 67, 4, 4.5f, 4, 9, 0.9f, 0.45f, 38, false, 3, 84, 0.5f, 0, 0, 0.6f, 0.9f },
	// DEEP: Aeolian, low, slow, rests, a pedal
	{ AEOLIAN, { 3, 0, 1, 1.5f, 1, 2, 1 }, { 1, 0.5f, 3, 2, 0.5f, 1.5f }, 18, 30, 0.25f, 57, 3, 2.4f, 8, 14, 0.8f, 0.6f, 31, true, 0.8f, 79, 0.45f, 0, 0, 0.7f, 0.8f },
	// APPROACH: Dorian, suspended, a slow pulse
	{ DORIAN, { 3, 1.5f, 1, 2, 1, 0, 1.5f }, { 1, 1, 1, 2.5f, 2.5f, 2 }, 8, 12, 0.0f, 62, 4, 3.4f, 3, 6, 0.8f, 0.55f, 36, false, 0.6f, 79, 0.35f, 1.5f, 0.35f, 0.45f, 0.85f },
	// REENTRY: Phrygian, a low drone, little else
	{ PHRYGIAN, { 3, 1.5f, 0.5f, 1, 0, 1, 0.5f }, { 0, 0, 3, 0, 1, 1 }, 16, 24, 0.5f, 55, 3, 1.8f, 6, 10, 0.5f, 0.8f, 31, true, 0, 79, 0.0f, 0, 0, 0.55f, 0.6f },
};

// ---- notes -----------------------------------------------------------------
enum Kind { PAD, BASS, BELL, PULSE, NKIND };
constexpr int MAXP = 12, MAXN = 48;
constexpr unsigned BLOCK = 32; // samples: the control rate

struct Note {
	bool on = false, releasing = false, twin = false, decays = false;
	Kind kind = PAD;
	int midi = 0, np = 0;
	uint32_t ph[2 * MAXP], inc[2 * MAXP];
	float amp[MAXP], decay[MAXP];
	float gl[2], gr[2];             // the copies' gains, left and right
	float level = 1.0f, wet = 0.5f;
	float attack = 1.0f, release = 1.0f; // [samples]
	float t = 0.0f;                 // [samples] since its start (or release)
	float relFrom = 1.0f;
	float env = 0.0f, envPrev = 0.0f;
	float lfoPh = 0.0f, lfoRate = 0.0f, lfoDepth = 0.0f; // a slow swell [rad, rad/sample]
};

/// Equal-power pan (-1 left .. 1 right)
inline void Pan (float p, float &l, float &r)
{
	const float a = (std::clamp (p, -1.0f, 1.0f) + 1.0f) * (PI / 4.0f);
	l = std::cos (a), r = std::sin (a);
}

// ---- the reverb: a feedback delay network ---------------------------------
struct Allpass {
	std::vector<float> b; size_t i = 0; float g = 0.6f;
	void Init (size_t n, float gain) { b.assign (std::max<size_t> (n, 1), 0.0f); i = 0; g = gain; }
	inline float Process (float x) {
		const float d = b[i];
		const float y = d - g * x;
		b[i] = x + g * y;
		if (++i == b.size ()) i = 0;
		return y;
	}
};

struct Reverb {
	static constexpr int N = 8;
	std::vector<float> buf[N];
	size_t pos[N] = {}, len[N] = {};
	float g[N] = {}, lp[N] = {}, damp = 0.5f;
	Allpass apL[2], apR[2];
	void Init (float rate, float t60, float dampHz) {
		static const float ms[N] = { 43.1f, 51.7f, 59.3f, 67.9f, 73.1f, 83.3f, 91.7f, 101.9f };
		for (int i = 0; i < N; i++) {
			len[i] = (size_t)(ms[i] * 1.6f * rate / 1000.0f);
			buf[i].assign (len[i], 0.0f);
			pos[i] = 0, lp[i] = 0.0f;
			g[i] = std::pow (10.0f, -3.0f * (float)len[i] / (rate * t60));
		}
		damp = 1.0f - std::exp (-2.0f * PI * dampHz / rate);
		apL[0].Init ((size_t)(0.0053f * rate), 0.6f), apL[1].Init ((size_t)(0.0119f * rate), 0.6f);
		apR[0].Init ((size_t)(0.0061f * rate), 0.6f), apR[1].Init ((size_t)(0.0107f * rate), 0.6f);
	}
	inline void Process (float inL, float inR, float &outL, float &outR) {
		inL = apL[1].Process (apL[0].Process (inL));
		inR = apR[1].Process (apR[0].Process (inR));
		float y[N];
		for (int i = 0; i < N; i++) {
			const float v = buf[i][pos[i]];
			lp[i] += damp * (v - lp[i]);
			y[i] = lp[i] * g[i];
		}
		outL = 0.5f * (y[0] - y[2] + y[4] - y[6]);
		outR = 0.5f * (y[1] - y[3] + y[5] - y[7]);
		// Hadamard: three stages of butterflies, then 1/sqrt(8)
		for (int h = 1; h < N; h <<= 1)
			for (int i = 0; i < N; i += 2 * h)
				for (int j = i; j < i + h; j++) {
					const float a = y[j], b = y[j + h];
					y[j] = a + b, y[j + h] = a - b;
				}
		for (int i = 0; i < N; i++) {
			buf[i][pos[i]] = 0.35355339f * y[i] + ((i & 1) ? inR : inL);
			if (++pos[i] == len[i]) pos[i] = 0;
		}
	}
};

/// The levels: the output's RMS at RMS (Composer::RMS) for a mood at level 1
/// (measured: the unit tests check it); the reverb's return
constexpr float NORM = 0.098f, REVERB_GAIN = 0.35f;

} // namespace

// ---- the composer ----------------------------------------------------------

struct Composer::Impl {
	float rate;
	dsp::Rng rng;
	const float *tb = Table ();
	Note notes[MAXN];
	Reverb rev;
	Mood mood = SILENT;
	float intensity = 0.0f;
	int tonic = 2;          // a pitch class (0: C)
	double now = 0.0;       // [samples]
	double nextChord = 0.0, nextBell = 0.0, nextPulse = 0.0;
	int root = -1, sinceTonic = 0, chordCount = 0, bellCount = 0, pulseCount = 0;
	bool wasRest = false;
	std::vector<int> chord; // the bass, then the pads (MIDI)
	int lastBell = 0, pulseStep = 0;
	struct Pending { double at; int midi; float vel; };
	std::vector<Pending> pendingBells;
	// glides: the whole's level, each kind's, the brightness (the output's low-pass)
	dsp::Smooth level, kind[NKIND], bright;
	float lpL = 0.0f, lpR = 0.0f;
	std::vector<float> dryL, dryR, wetL, wetR;

	Impl (float r, uint32_t seed): rate (r), rng (seed) {
		static const int tonics[] = { 0, 2, 3, 4, 5, 7, 9 }; // C, D, Eb, E, F, G, A
		tonic = tonics[rng.Next () % 7];
		rev.Init (rate, 6.5f, 4500.0f);
		dryL.resize (BLOCK), dryR.resize (BLOCK), wetL.resize (BLOCK), wetR.resize (BLOCK);
		pendingBells.reserve (8);
		bright.v = MOODS[ORBIT].bright;
	}

	double Sec (double s) const { return s * rate; }
	float U (float a, float b) { return a + (b - a) * rng.Uniform (); }

	Note *Free () {
		for (Note &n : notes) if (!n.on) return &n;
		// (all taken: the quietest releasing one, else none)
		Note *q = nullptr;
		for (Note &n : notes) if (n.releasing && (!q || n.env < q->env)) q = &n;
		return q;
	}
	void ReleaseNote (Note &n, float seconds) {
		if (!n.on || n.releasing) return;
		n.releasing = true;
		n.relFrom = n.env;
		n.release = std::max (1.0f, seconds * rate);
		n.t = 0.0f;
	}

	/// A held note: a pad (partials rolling off with the brightness, doubled
	/// and detuned) or the bass
	void StartHeld (Kind k, int midi, float level, float pan, float attack, float bri, float wet) {
		Note *n = Free ();
		if (!n) return;
		*n = Note ();
		n->on = true, n->kind = k, n->midi = midi, n->twin = true;
		const float f = MidiHz ((float)midi);
		const float top = std::min (7000.0f, 0.4f * rate);
		float sum = 0.0f;
		if (k == BASS) {
			static const float a[4] = { 1.0f, 0.4f, 0.15f, 0.06f };
			n->np = 4;
			for (int i = 0; i < 4; i++) n->amp[i] = a[i], sum += a[i] * a[i];
		} else {
			n->np = std::clamp ((int)(top / f), 1, MAXP);
			for (int i = 0; i < n->np; i++) {
				const float kk = (float)(i + 1);
				n->amp[i] = std::exp (-(kk - 1.0f) / bri) / kk * U (0.85f, 1.15f);
				sum += n->amp[i] * n->amp[i];
			}
		}
		const float norm = 1.0f / std::sqrt (sum);
		const float cents = k == BASS ? U (1.5f, 3.0f) : U (3.5f, 8.0f);
		const float dA = std::exp2 (-cents / 1200.0f), dB = std::exp2 (cents / 1200.0f);
		for (int i = 0; i < n->np; i++) {
			n->amp[i] *= norm;
			n->decay[i] = 1.0f;
			n->inc[2 * i] = Inc ((double)f * (i + 1) * dA, rate);
			n->inc[2 * i + 1] = Inc ((double)f * (i + 1) * dB, rate);
			n->ph[2 * i] = rng.Next (), n->ph[2 * i + 1] = rng.Next ();
		}
		const float w = k == BASS ? 0.1f : 0.3f; // (the copies apart: the chorus's width)
		Pan (pan - w, n->gl[0], n->gr[0]);
		Pan (pan + w, n->gl[1], n->gr[1]);
		for (int c = 0; c < 2; c++) n->gl[c] *= 0.70710678f, n->gr[c] *= 0.70710678f;
		n->level = level, n->wet = wet;
		n->attack = std::max (1.0f, attack * rate);
		n->lfoPh = U (0.0f, 2.0f * PI);
		n->lfoRate = 2.0f * PI * U (0.04f, 0.12f) / rate;
		n->lfoDepth = k == BASS ? 0.08f : U (0.12f, 0.25f);
	}

	/// A struck note: a bell (glassy: partials a little off the harmonics)
	/// or the pulse's soft mallet
	void Strike (Kind k, int midi, float vel, float pan, float wet) {
		Note *n = Free ();
		if (!n) return;
		*n = Note ();
		n->on = true, n->kind = k, n->midi = midi, n->decays = true;
		const float f = MidiHz ((float)midi);
		static const float bellR[5] = { 1.0f, 2.0f, 2.76f, 4.07f, 5.4f };
		static const float bellA[5] = { 1.0f, 0.35f, 0.3f, 0.12f, 0.1f };
		static const float bellT[5] = { 4.5f, 2.5f, 1.6f, 1.0f, 0.6f };
		static const float malR[3] = { 1.0f, 2.0f, 3.0f };
		static const float malA[3] = { 1.0f, 0.3f, 0.1f };
		static const float malT[3] = { 0.45f, 0.25f, 0.15f };
		const float *R = k == BELL ? bellR : malR, *A = k == BELL ? bellA : malA, *T = k == BELL ? bellT : malT;
		const int np = k == BELL ? 5 : 3;
		n->np = 0;
		for (int i = 0; i < np; i++) {
			if (f * R[i] > 0.4f * rate) break;
			n->amp[i] = A[i];
			n->decay[i] = std::exp (-1.0f / (T[i] * rate));
			n->inc[2 * i] = Inc ((double)f * R[i], rate);
			n->ph[2 * i] = 0;
			n->np++;
		}
		Pan (pan, n->gl[0], n->gr[0]);
		n->gl[1] = n->gr[1] = 0.0f;
		n->level = vel, n->wet = wet;
		n->attack = std::max (1.0f, (k == BELL ? 0.006f : 0.004f) * rate);
	}

	const MoodDef &Def () const { return MOODS[mood]; }
	int Pc (int degree) const { // the pitch class (from the tonic, in semitones) of a scale degree (any octave)
		const int d = ((degree % 7) + 7) % 7;
		return SCALE[Def ().mode][d] + 12 * (int)std::floor (degree / 7.0);
	}

	void NewChord () {
		const MoodDef &m = Def ();
		chordCount++;
		// a rest now and then (not two running, not at the first)
		const bool rest = chordCount > 1 && !wasRest && rng.Uniform () < m.rest;
		wasRest = rest;
		// the root: a weighted walk, not standing still, drawn home to the tonic
		float w[7], tot = 0.0f;
		for (int d = 0; d < 7; d++) {
			w[d] = m.degree[d];
			if (d == root) w[d] = 0.0f;
			if (d == 0) w[d] *= 1.0f + 0.6f * sinceTonic;
			tot += w[d];
		}
		int r = 0;
		if (root < 0) r = 0;
		else if (tot > 0.0f) {
			float x = rng.Uniform () * tot;
			for (r = 0; r < 6 && x >= w[r]; r++) x -= w[r];
		}
		root = r;
		sinceTonic = r == 0 ? 0 : sinceTonic + 1;
		// the voicing
		float vt = 0.0f;
		for (float v : m.voicing) vt += v;
		int v = 0;
		{
			float x = rng.Uniform () * vt;
			for (v = 0; v < NVOICING - 1 && x >= m.voicing[v]; v++) x -= m.voicing[v];
		}
		std::vector<int> tones;
		for (int i = 4 - m.notes; i < 4; i++) tones.push_back (tonic + 48 + Pc (r + VOICE[v][i]));
		float mean = 0.0f;
		for (int t : tones) mean += t;
		mean /= tones.size ();
		const int shift = 12 * (int)std::lround ((m.padCenter - mean) / 12.0f);
		for (int &t : tones) t += shift;
		// the bass: the root (or the tonic), from its lowest note up
		const int bpc = ((tonic + Pc (m.pedal ? 0 : r)) % 12 + 12) % 12;
		const int bass = m.bassLow + ((bpc - m.bassLow) % 12 + 12) % 12;

		// pads: common tones held over, the rest released and struck anew
		std::vector<int> fresh;
		if (!rest) fresh = tones;
		for (Note &n : notes) {
			if (!n.on || n.releasing || n.kind != PAD) continue;
			auto it = std::find (fresh.begin (), fresh.end (), n.midi);
			if (it != fresh.end ()) fresh.erase (it);
			else ReleaseNote (n, m.release);
		}
		const float lv = 1.0f / std::sqrt ((float)m.notes);
		for (size_t i = 0; i < fresh.size (); i++) {
			const float pan = tones.size () > 1 ? -0.55f + 1.1f * (float)(std::find (tones.begin (), tones.end (), fresh[i]) - tones.begin ()) / (tones.size () - 1) : 0.0f;
			StartHeld (PAD, fresh[i], lv * U (0.8f, 1.0f), pan, m.attack * U (0.8f, 1.2f), m.bright * (1.0f + (mood == ASCENT ? 0.6f * intensity : 0.0f)), m.wet);
		}
		bool held = false;
		for (Note &n : notes) {
			if (!n.on || n.releasing || n.kind != BASS) continue;
			if (n.midi == bass && !held) held = true;
			else ReleaseNote (n, m.release);
		}
		if (!held) StartHeld (BASS, bass, 1.0f, 0.0f, m.attack * 1.3f, 2.0f, m.wet * 0.7f);
		chord.clear ();
		chord.push_back (bass);
		if (!rest) chord.insert (chord.end (), tones.begin (), tones.end ());
		nextChord = now + Sec (U (m.chordMin, m.chordMax));
	}

	void Bells () {
		const MoodDef &m = Def ();
		// a phrase: one to three notes, stepping near the last
		std::vector<int> cand;
		static const int degs[5] = { 0, 1, 2, 4, 5 };
		for (int midi = m.bellCenter - 5; midi <= m.bellCenter + 10; midi++) {
			const int pc = ((midi - tonic) % 12 + 12) % 12;
			bool ok = false;
			for (int d : degs) ok |= SCALE[m.mode][d] == pc;
			for (size_t i = 1; i < chord.size () && !ok; i++) ok = ((chord[i] - midi) % 12 + 12) % 12 == 0;
			if (ok) cand.push_back (midi);
		}
		if (cand.empty ()) return;
		const int count = 1 + (rng.Uniform () < 0.5f) + (rng.Uniform () < 0.25f);
		double at = now;
		for (int i = 0; i < count; i++) {
			int midi = cand[rng.Next () % cand.size ()];
			if (lastBell && rng.Uniform () < 0.7f) { // (near the last)
				std::vector<int> near;
				for (int c : cand) if (c != lastBell && std::abs (c - lastBell) <= 5) near.push_back (c);
				if (!near.empty ()) midi = near[rng.Next () % near.size ()];
			}
			lastBell = midi;
			pendingBells.push_back ({ at, midi, U (0.6f, 1.0f) });
			at += Sec (U (0.3f, 0.9f));
		}
	}

	void Pulse () {
		const MoodDef &m = Def ();
		if (chord.size () < 2) return;
		// an arpeggio of the chord, up and down, in the middle
		std::vector<int> t;
		for (size_t i = 1; i < chord.size (); i++) {
			int x = chord[i];
			while (x > 70) x -= 12;
			while (x < 55) x += 12;
			t.push_back (x);
		}
		std::sort (t.begin (), t.end ());
		const int n = (int)t.size (), period = std::max (1, 2 * n - 2);
		const int s = pulseStep % period;
		const int idx = s < n ? s : period - s;
		const float vel = (pulseStep % 4 == 0 ? 1.0f : 0.7f) * U (0.9f, 1.0f);
		Strike (PULSE, t[std::clamp (idx, 0, n - 1)], vel, (pulseStep & 1) ? 0.35f : -0.35f, m.wet * 0.6f);
		pulseStep++;
		pulseCount++;
	}

	void SetMood (Mood m) {
		if (m == mood) return;
		const Mood prev = mood;
		mood = m;
		if (m == SILENT) {
			for (Note &n : notes) if (n.kind == PAD || n.kind == BASS) ReleaseNote (n, 2.5f);
			pendingBells.clear ();
			return;
		}
		// the change shows at the next chord, soon
		if (prev == SILENT) nextChord = now, nextBell = now + Sec (U (3.0f, 8.0f));
		else nextChord = std::min (nextChord, now + Sec (U (0.8f, 2.5f)));
		nextPulse = now;
	}

	/// The control rate: events, glides, envelopes (m samples)
	void Control (unsigned m) {
		const MoodDef &d = Def ();
		const float x = intensity;
		const bool driven = mood == ASCENT || mood == APPROACH;
		const float pulseGain = d.pulse * (driven ? 0.25f + 0.75f * x : 1.0f);
		const float padGain = d.pad * (mood == ASCENT ? 0.75f + 0.25f * x : 1.0f);
		const float brightT = d.bright * (mood == ASCENT ? 1.0f + 0.6f * x : 1.0f);
		const float levelT = d.level;
		level.Step (levelT, dsp::Smooth::K (levelT < level.v ? (mood == SILENT ? 1.2f : 4.0f) : 4.0f, m, rate));
		const float kk = dsp::Smooth::K (3.0f, m, rate);
		kind[PAD].Step (padGain, kk);
		kind[BASS].Step (d.bass, kk);
		kind[BELL].Step (d.bell, kk);
		kind[PULSE].Step (pulseGain, kk);
		bright.Step (brightT, kk);

		if (mood != SILENT) {
			if (now >= nextChord) NewChord ();
			if (d.bellsPerMin > 0.0f && now >= nextBell) {
				if (nextBell > 0.0) Bells ();
				const float mean = 60.0f / d.bellsPerMin;
				nextBell = now + Sec (std::max (4.0f, -mean * std::log (std::max (rng.Uniform (), 1e-4f))));
			}
			for (size_t i = 0; i < pendingBells.size (); ) {
				if (now >= pendingBells[i].at) {
					const Pending b = pendingBells[i];
					Strike (BELL, b.midi, b.vel, U (-0.7f, 0.7f), std::min (1.0f, d.wet + 0.3f));
					bellCount++;
					pendingBells.erase (pendingBells.begin () + i);
				} else i++;
			}
			const float pr = d.pulseRate * (mood == APPROACH ? 0.8f + 0.4f * x : 1.0f);
			if (pr > 0.0f && kind[PULSE].v > 0.01f && now >= nextPulse) {
				Pulse ();
				nextPulse = std::max (nextPulse + Sec (1.0 / pr), now);
			}
		}

		// (silent and faded: whatever still rings is gone)
		if (mood == SILENT && level.v < 1e-4f)
			for (Note &n : notes) n.on = false;
		for (Note &n : notes) {
			if (!n.on) continue;
			n.envPrev = n.env;
			n.t += (float)m;
			if (n.decays) {
				n.env = n.t < n.attack ? n.t / n.attack : 1.0f;
				float s = 0.0f;
				for (int i = 0; i < n.np; i++) s += n.amp[i];
				if (n.t > n.attack && s < 2e-3f) n.on = false; // (-54 dB)
			} else if (!n.releasing) {
				n.env = n.t < n.attack ? 0.5f - 0.5f * std::cos (PI * n.t / n.attack) : 1.0f;
			} else {
				n.env = n.t < n.release ? n.relFrom * (0.5f + 0.5f * std::cos (PI * n.t / n.release)) : 0.0f;
				if (n.t >= n.release) n.on = false, n.env = 0.0f;
			}
			n.lfoPh += n.lfoRate * (float)m;
			if (n.lfoPh > 2.0f * PI) n.lfoPh -= 2.0f * PI;
		}
		now += m;
	}

	void Play (Note &n, unsigned m) {
		const float lfo = 1.0f + n.lfoDepth * std::sin (n.lfoPh);
		const float g = n.level * lfo * kind[n.kind].v;
		float g0 = g * n.envPrev, g1 = g * n.env;
		if (g0 < 1e-7f && g1 < 1e-7f) {
			// (silent: the phases on, the decays too)
			for (int i = 0; i < n.np; i++) {
				n.ph[2 * i] += n.inc[2 * i] * m;
				if (n.twin) n.ph[2 * i + 1] += n.inc[2 * i + 1] * m;
				if (n.decays) n.amp[i] *= std::pow (n.decay[i], (float)m);
			}
			return;
		}
		const float dg = (g1 - g0) / m;
		const float dry = 1.0f - 0.5f * n.wet, wet = n.wet;
		for (unsigned s = 0; s < m; s++) {
			float a = 0.0f, b = 0.0f;
			if (n.twin) {
				for (int i = 0; i < n.np; i++) {
					a += n.amp[i] * Sine (tb, n.ph[2 * i]);
					b += n.amp[i] * Sine (tb, n.ph[2 * i + 1]);
					n.ph[2 * i] += n.inc[2 * i];
					n.ph[2 * i + 1] += n.inc[2 * i + 1];
				}
			} else {
				for (int i = 0; i < n.np; i++) {
					a += n.amp[i] * Sine (tb, n.ph[2 * i]);
					n.ph[2 * i] += n.inc[2 * i];
					if (n.decays) n.amp[i] *= n.decay[i];
				}
			}
			const float gs = g0 + dg * s;
			const float L = gs * (n.gl[0] * a + n.gl[1] * b), R = gs * (n.gr[0] * a + n.gr[1] * b);
			dryL[s] += dry * L, dryR[s] += dry * R;
			wetL[s] += wet * L, wetR[s] += wet * R;
		}
	}

	void Render (float *l, float *r, unsigned n) {
		unsigned i = 0;
		while (i < n) {
			const unsigned m = std::min (BLOCK, n - i);
			const float lv0 = level.v;
			Control (m);
			const float lv1 = level.v;
			std::fill (dryL.begin (), dryL.begin () + m, 0.0f), std::fill (dryR.begin (), dryR.begin () + m, 0.0f);
			std::fill (wetL.begin (), wetL.begin () + m, 0.0f), std::fill (wetR.begin (), wetR.begin () + m, 0.0f);
			for (Note &nt : notes) if (nt.on) Play (nt, m);
			const float fc = 2500.0f + 1800.0f * bright.v;
			const float a = 1.0f - std::exp (-2.0f * PI * std::min (fc, 0.45f * rate) / rate);
			for (unsigned s = 0; s < m; s++) {
				float rl, rr;
				rev.Process (wetL[s], wetR[s], rl, rr);
				const float lv = NORM * (lv0 + (lv1 - lv0) * (float)s / m);
				float yl = lv * (dryL[s] + REVERB_GAIN * rl), yr = lv * (dryR[s] + REVERB_GAIN * rr);
				lpL += a * (yl - lpL), lpR += a * (yr - lpR);
				l[i + s] = Soft (lpL), r[i + s] = Soft (lpR);
			}
			i += m;
		}
	}
	/// Above half scale: a soft knee up to full
	static inline float Soft (float x) {
		const float ax = std::fabs (x);
		if (ax <= 0.5f) return x;
		const float y = 0.5f + 0.5f * std::tanh ((ax - 0.5f) * 2.0f);
		return x < 0.0f ? -y : y;
	}
};

Composer::Composer (float rate, uint32_t seed): p (std::make_unique<Impl> (rate, seed)) {}
Composer::~Composer () = default;
void Composer::SetMood (Mood m) { p->SetMood (m); }
void Composer::SetIntensity (float x) { p->intensity = std::clamp (x, 0.0f, 1.0f); }
void Composer::Render (float *l, float *r, unsigned n) { p->Render (l, r, n); }
Mood Composer::GetMood () const { return p->mood; }
int Composer::Tonic () const { return p->tonic; }
int Composer::Chords () const { return p->chordCount; }
int Composer::Bells () const { return p->bellCount; }
int Composer::Pulses () const { return p->pulseCount; }
int Composer::Sounding () const { int c = 0; for (const Note &n : p->notes) c += n.on; return c; }
std::vector<int> Composer::Chord () const { return p->chord; }
float Composer::Level () const { const float l = MOODS[p->mood].level; return l > 0.0f ? p->level.v / l : p->level.v; }

} // namespace music

// ---- on the music bus ------------------------------------------------------

std::unique_ptr<MusicSynth> MusicSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<MusicSynth> s (new MusicSynth ());
	s->channels = std::max (1u, e.Channels ());
	if (!s->Init (e)) return nullptr;
	s->comp = std::make_unique<music::Composer> (s->rate, seed);
	s->l.resize (2048), s->r.resize (2048);
	return s;
}

MusicSynth::~MusicSynth ()
{
	Release (); // (out of the graph before the composer goes)
}

void MusicSynth::Render (float *out, unsigned n, float)
{
	comp->SetMood ((music::Mood)mood.load (std::memory_order_relaxed));
	comp->SetIntensity (intensity.load (std::memory_order_relaxed));
	const unsigned ch = channels;
	unsigned done = 0;
	while (done < n) {
		const unsigned m = std::min<unsigned> (n - done, (unsigned)l.size ());
		comp->Render (l.data (), r.data (), m);
		float *o = out + (size_t)done * ch;
		if (ch == 1) for (unsigned i = 0; i < m; i++) o[i] = 0.5f * (l[i] + r[i]);
		else for (unsigned i = 0; i < m; i++) {
			o[i * ch] = l[i], o[i * ch + 1] = r[i];
			for (unsigned c = 2; c < ch; c++) o[i * ch + c] = 0.0f;
		}
		done += m;
	}
}

} // namespace audio
