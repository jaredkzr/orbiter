// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Loudness.h"
#include "Assets.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

namespace audio {

namespace {
	/// A biquad, direct form I (double: these are measurements)
	struct Bq {
		double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		double Run (double x) {
			const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1, x1 = x, y2 = y1, y1 = y;
			return y;
		}
	};
	/// BS.1770's K-weighting at any rate (its pre-filter and RLB filter,
	/// from their analogue prototypes: Mansbridge et al.'s parameters)
	void KWeighting (unsigned rate, Bq &shelf, Bq &hp)
	{
		const double fs = rate, PI = 3.14159265358979;
		{   // the shelf: +4 dB above 1681.97 Hz, Q 0.7072
			const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
			const double K = std::tan (PI * f0 / fs), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
			const double a0 = 1.0 + K / Q + K * K;
			shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
			shelf.b1 = 2.0 * (K * K - Vh) / a0;
			shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
			shelf.a1 = 2.0 * (K * K - 1.0) / a0;
			shelf.a2 = (1.0 - K / Q + K * K) / a0;
		}
		{   // the high-pass: 38.14 Hz, Q 0.5003
			const double f0 = 38.13547087602444, Q = 0.5003270373238773;
			const double K = std::tan (PI * f0 / fs);
			const double a0 = 1.0 + K / Q + K * K;
			hp.b0 = 1.0, hp.b1 = -2.0, hp.b2 = 1.0;
			hp.a1 = 2.0 * (K * K - 1.0) / a0;
			hp.a2 = (1.0 - K / Q + K * K) / a0;
		}
	}
}

double Loudness (const float *x, size_t frames, unsigned ch, unsigned rate)
{
	if (!x || !frames || !ch || !rate) return -INFINITY;
	const size_t step = rate / 10, block = 4 * step; // (400 ms, every 100 ms)
	std::vector<double> e ((frames + step - 1) / step, 0.0); // (the K-weighted energy of each 100 ms)
	for (unsigned c = 0; c < ch; c++) {
		Bq shelf, hp;
		KWeighting (rate, shelf, hp);
		const double wc = c < 3 ? 1.0 : 1.41; // (surrounds weighted up; this is stereo or mono anyway)
		for (size_t i = 0; i < frames; i++) {
			const double y = hp.Run (shelf.Run (x[i * ch + c]));
			e[i / step] += wc * y * y;
		}
	}
	std::vector<double> z;
	if (frames < block) { // (shorter than a block: the whole)
		double s = 0.0;
		for (double v : e) s += v;
		z.push_back (s / frames);
	} else
		for (size_t b = 0; b + 4 <= e.size () && (b + 4) * step <= frames; b++)
			z.push_back ((e[b] + e[b + 1] + e[b + 2] + e[b + 3]) / block);
	auto lufs = [] (double v) { return -0.691 + 10.0 * std::log10 (std::max (v, 1e-20)); };
	auto gated = [&] (double gate) {
		double s = 0.0; int n = 0;
		for (double v : z) if (lufs (v) > gate) s += v, n++;
		return n ? s / n : 0.0;
	};
	const double abs = gated (-70.0);
	if (abs <= 0.0) return -INFINITY;
	const double rel = gated (lufs (abs) - 10.0);
	return rel > 0.0 ? lufs (rel) : -INFINITY;
}

double FileLoudness (const std::string &path)
{
	return Assets::Get ().Measured (path).lufs;
}

double LoudnessGain (const std::string &path, double target, double maxDb)
{
	const Assets::Measure r = Assets::Get ().Measured (path);
	if (!std::isfinite (r.lufs)) return 1.0;
	double g = std::pow (10.0, std::clamp (target - r.lufs, -maxDb, maxDb) / 20.0);
	if (r.peak > 0.0) g = std::min (g, std::max (1.0, PEAK_MAX / r.peak)); // (raised no further than its peak at -1 dBFS)
	return g;
}

} // namespace audio
