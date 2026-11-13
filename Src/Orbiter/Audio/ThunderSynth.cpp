// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ThunderSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	/// The N-wave's length at the relaxation radius: 2.6 R0 / c (Few)
	constexpr double TAU_K = 2.6;
	/// Its overpressure at the relaxation radius, of the ambient pressure:
	/// fitted to measured thunder - a channel of 2e5 J/m a kilometre off
	/// peaks at some 20 Pa (Holmes, Brook, Krehbiel & McCrory 1971: 0.5-10
	/// Pa at 1-10 km), the broadside stretch's N-waves arriving together
	constexpr double DP_R0 = 0.02;
	/// The mean kink between segments [rad] (Few: about 16 degrees)
	constexpr double KINK = 16.0 * 3.14159265 / 180.0;
	/// The radiation lobe: |sin theta|^LOBE (broadside), and what is left
	/// off it (a segment isn't a straight line)
	constexpr double LOBE = 6.0, LOBE_FLOOR = 0.03;

	struct Rand {
		uint32_t s;
		double U () { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s >> 8) * (1.0 / 16777216.0); }
		double G () { double u = std::max (U (), 1e-12), v = U (); return std::sqrt (-2.0 * std::log (u)) * std::cos (6.2831853 * v); }
	};
	void Norm (double v[3]) { double l = std::sqrt (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (l > 0) for (int k = 0; k < 3; k++) v[k] /= l; }
}

ThunderSynth::Channel ThunderSynth::Make (const double start[3], const double dir0[3], double length, double seg, uint32_t seed)
{
	Rand r{seed ? seed : 1u};
	Channel ch;
	double p[3] = {start[0], start[1], start[2]}, base[3] = {dir0[0], dir0[1], dir0[2]}, d[3];
	Norm (base);
	for (int k = 0; k < 3; k++) d[k] = base[k];
	const int n = std::max (2, (int)(length / seg));
	ch.x.push_back (p[0]), ch.y.push_back (p[1]), ch.z.push_back (p[2]);
	for (int i = 0; i < n; i++) {
		// kinked from the last segment at random (about KINK), pulled back
		// towards the channel's way (it wanders, it doesn't turn round)
		for (int k = 0; k < 3; k++) d[k] += KINK * r.G () / std::sqrt (2.0) + 0.15 * (base[k] - d[k]);
		Norm (d);
		for (int k = 0; k < 3; k++) p[k] += seg * d[k];
		ch.x.push_back (p[0]), ch.y.push_back (p[1]), ch.z.push_back (p[2]);
	}
	return ch;
}

std::vector<float> ThunderSynth::Render (const Channel &ch, const double L[3], const std::vector<double> &strokes,
	double El, double p0, double c, unsigned rate, double &dmin)
{
	const size_t n = ch.x.size ();
	const double R0 = RelaxationRadius (El, p0);
	c = std::max (c, 50.0);
	struct Seg { double d, sinT, l; };
	std::vector<Seg> segs;
	dmin = 1e30;
	double dmax = 0.0;
	for (size_t i = 0; i + 1 < n; i++) {
		const double m[3] = { 0.5 * (ch.x[i] + ch.x[i+1]), 0.5 * (ch.y[i] + ch.y[i+1]), 0.5 * (ch.z[i] + ch.z[i+1]) };
		double s[3] = { ch.x[i+1] - ch.x[i], ch.y[i+1] - ch.y[i], ch.z[i+1] - ch.z[i] };
		const double l = std::sqrt (s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
		double to[3] = { L[0] - m[0], L[1] - m[1], L[2] - m[2] };
		const double d = std::max (std::sqrt (to[0]*to[0] + to[1]*to[1] + to[2]*to[2]), 1.0);
		const double cosT = (s[0]*to[0] + s[1]*to[1] + s[2]*to[2]) / (std::max (l, 1e-9) * d);
		segs.push_back ({d, std::sqrt (std::max (0.0, 1.0 - cosT * cosT)), l});
		dmin = std::min (dmin, d), dmax = std::max (dmax, d);
	}
	const double last = strokes.empty () ? 0.0 : strokes.back ();
	const double len = (dmax - dmin) / c + last + 1.5;
	std::vector<float> buf ((size_t)(len * rate) + 1, 0.0f);
	for (const Seg &sg : segs) {
		// its N-wave here: overpressure (cylindrical within its length, then
		// spherical), length (d^1/4), the lobe
		const double dc = std::min (sg.d, std::max (sg.l, R0));
		double dp = DP_R0 * p0 * std::pow (R0 / std::max (dc, R0), 0.75);
		if (sg.d > dc) dp *= dc / sg.d;
		dp *= LOBE_FLOOR + (1.0 - LOBE_FLOOR) * std::pow (sg.sinT, LOBE);
		const double tau = TAU_K * R0 / c * std::pow (std::max (sg.d, R0) / R0, 0.25);
		// (its edges rounded: the air's absorption and turbulence over the
		// distance it came beyond the nearest)
		const double edge = std::min (0.3 * tau, 0.1e-3 + 2e-7 * (sg.d - dmin));
		const int nT = std::max (2, (int)(tau * rate)), nE = std::max (1, (int)(edge * rate));
		for (double ts : strokes) {
			const double t0 = ts + (sg.d - dmin) / c;
			const size_t i0 = (size_t)(t0 * rate);
			const int nR = std::min (nE, nT / 3);
			for (int j = 0; j < nT && i0 + j < buf.size (); j++) {
				// the N: up to +dp, a straight fall to -dp over tau, back to 0 -
				// its jumps rounded (raised-cosine ramps nR long)
				double v = 1.0 - 2.0 * j / (double)nT;
				if (j < nR) v *= 0.5 * (1.0 - std::cos (3.14159265 * j / nR));
				else if (j > nT - nR) v *= 0.5 * (1.0 - std::cos (3.14159265 * (nT - j) / nR));
				buf[i0 + j] += (float)(dp * v);
			}
		}
	}
	return buf;
}

std::unique_ptr<ThunderSynth> ThunderSynth::Make (Engine &e, std::vector<float> &&buf)
{
	std::unique_ptr<ThunderSynth> s (new ThunderSynth (std::move (buf)));
	if (!s->Init (e)) return nullptr;
	return s;
}

void ThunderSynth::Render (float *out, unsigned n, float dop)
{
	if (!fire.load (std::memory_order_acquire) || pos >= buf.size ()) {
		std::fill (out, out + n, 0.0f);
		if (pos >= buf.size () && fire.load (std::memory_order_relaxed)) done.store (true, std::memory_order_release);
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		// (played at the Doppler factor: a moving listener)
		size_t k = (size_t)fpos;
		out[i] = k + 1 < buf.size () ? (float)(buf[k] + (fpos - k) * (buf[k+1] - buf[k])) : 0.0f;
		fpos += dop;
	}
	pos = (size_t)fpos;
}

} // namespace audio
