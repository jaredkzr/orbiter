// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Track.h"
#include <algorithm>
#include <cmath>

namespace audio {

double V3::Len () const
{
	return std::sqrt (x*x + y*y + z*z);
}

Track::Track (double age, double step): maxAge (age), minStep (step)
{
}

void Track::Add (double t, const V3 &p)
{
	if (!pts.empty () && t <= pts.back ().t) {
		if (t < pts.back ().t) pts.clear (), pruned = false; // (time went back: a new path)
		else { // same time: move the point
			Pt &b = pts.back ();
			b.s = pts.size () > 1 ? pts[pts.size () - 2].s + (p - pts[pts.size () - 2].p).Len () : 0.0;
			b.p = p;
			return;
		}
	}
	if (pts.size () >= 2 && pts.back ().t - pts[pts.size () - 2].t < minStep) {
		// (the last point is still too close to the one before: it moves on)
		Pt &b = pts.back ();
		const Pt &a = pts[pts.size () - 2];
		b.t = t, b.p = p, b.s = a.s + (p - a.p).Len ();
	} else {
		double s = pts.empty () ? 0.0 : pts.back ().s + (p - pts.back ().p).Len ();
		pts.push_back ({t, p, s});
	}
	while (pts.size () > 2 && pts[1].t < t - maxAge) pts.pop_front (), pruned = true;
}

int Track::Heard (double t, const V3 &L, double c, Image *out, int maxn) const
{
	if (pts.empty () || !(c > 0.0)) return 0;
	int n = 0;
	auto add = [&](double tau, const V3 &p, const V3 &v) {
		if (n < maxn) out[n] = {tau, p, v};
		n++;
	};

	// the path up to t (points after it are the future: not heard yet)
	size_t m = pts.size ();
	while (m > 1 && pts[m-1].t > t) m--;
	const Pt &first = pts.front ();
	if (first.t > t) return 0; // (all of it in the future: not there yet)
	// where the source is now, and the path's length up to there
	const Pt &last = pts[m-1];
	V3 pNow = last.p;
	double sNow = last.s;
	bool held = true; // (the source at rest after its last point)
	if (m < pts.size ()) {
		const Pt &b = pts[m];
		double f = (t - last.t) / (b.t - last.t);
		pNow = last.p + (b.p - last.p) * f;
		sNow = last.s + (b.s - last.s) * f;
		held = false;
	}
	const double dNow = (L - pNow).Len ();

	// after the last point: held there
	if (held && dNow <= c * (t - last.t)) {
		add (t - dNow / c, last.p, V3 ());
		return n; // (anything older would be heard later than this, still on its way: no)
	}

	// the segments, newest first: |D - V u| = c (T0 - u) for u in [0, h]
	// with D = L - P_a, T0 = t - t_a
	const double pathToNow = sNow - first.s;
	for (size_t i = held ? m - 1 : m; i > 0; i--) {
		const Pt &a = pts[i-1], &b = pts[i];
		double seg = b.t - a.t;
		if (seg <= 0.0) continue;
		double h = std::min (seg, t - a.t); // (the segment up to now)
		V3 V = (b.p - a.p) * (1.0 / seg);
		V3 D = L - a.p;
		double T0 = t - a.t;
		double A = V.Dot (V) - c * c;
		double B = -2.0 * D.Dot (V) + 2.0 * c * c * T0;
		double C = D.Dot (D) - c * c * T0 * T0;
		double r[2]; int nr = 0;
		if (std::fabs (A) < 1e-12 * c * c) {
			if (std::fabs (B) > 0.0) r[nr++] = -C / B;
		} else {
			double disc = B * B - 4.0 * A * C;
			if (disc >= 0.0) {
				double sq = std::sqrt (disc);
				// (the numerically stable pair)
				double q = -0.5 * (B + (B >= 0.0 ? sq : -sq));
				if (q != 0.0) r[nr++] = q / A, r[nr++] = C / q;
				else r[nr++] = -B / (2.0 * A);
			}
		}
		// newest first within the segment: larger u first
		if (nr == 2 && r[0] < r[1]) std::swap (r[0], r[1]);
		bool newest = (i == (held ? m - 1 : m));
		for (int k = 0; k < nr; k++) {
			double u = r[k];
			if (u < 0.0 || u > h || T0 - u < 0.0) continue;
			if (u == h && !newest) continue; // (the shared point: counted in the newer segment)
			add (a.t + u, a.p + V * u, V);
		}
		// no root can be older: the sound from before a would have to come
		// further than the listener's distance plus all the path there is
		if (c * (t - a.t) > dNow + pathToNow) return n;
	}

	// before the first point: going on as it started, backwards in time
	// (unless the path was cut: sound older than the track is no longer heard)
	if (pruned) return n;
	V3 v0;
	if (m >= 2 && pts[1].t > first.t) v0 = (pts[1].p - first.p) * (1.0 / (pts[1].t - first.t));
	// |D - v0 u| = c (T0 - u), u <= 0
	V3 D = L - first.p;
	double T0 = t - first.t;
	double A = v0.Dot (v0) - c * c;
	double B = -2.0 * D.Dot (v0) + 2.0 * c * c * T0;
	double C = D.Dot (D) - c * c * T0 * T0;
	double r[2]; int nr = 0;
	if (std::fabs (A) < 1e-12 * c * c) {
		if (std::fabs (B) > 0.0) r[nr++] = -C / B;
	} else {
		double disc = B * B - 4.0 * A * C;
		if (disc >= 0.0) {
			double sq = std::sqrt (disc);
			double q = -0.5 * (B + (B >= 0.0 ? sq : -sq));
			if (q != 0.0) r[nr++] = q / A, r[nr++] = C / q;
			else r[nr++] = -B / (2.0 * A);
		}
	}
	if (nr == 2 && r[0] < r[1]) std::swap (r[0], r[1]);
	for (int k = 0; k < nr; k++)
		if (r[k] < 0.0) add (first.t + r[k], first.p + v0 * r[k], v0);
	return n;
}

} // namespace audio
