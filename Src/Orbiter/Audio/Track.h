// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A sound source's path through the air, and what a listener hears of it:
// the sound heard at time t left the source at an earlier time tau, where
// the source was c (t - tau) away from the listener:
//
//     |L(t) - P(tau)| = c (t - tau)
//
// The path is a polyline (positions in the air's frame, e.g. the planet's
// rotating frame) over the last minute; on each segment the equation is a
// quadratic, solved exactly. A source slower than sound is heard once
// (one emission time); a supersonic one is heard twice or not at all: not
// ahead of its shock cone, twice behind it, and a pair of emission times
// appears where the cone sweeps over the listener (the sonic boom).
// Before the path's first point the source goes on as it started (a
// parked vessel has been there all along, a flying one was flying),
// unless the path was cut (points older than the track's age dropped, or
// a restart: then there is no older sound); after its last point it is
// held there.
// =======================================================================

#ifndef __AUDIOTRACK_H
#define __AUDIOTRACK_H

#include <deque>

namespace audio {

struct V3 {
	double x = 0, y = 0, z = 0;
	V3 () = default;
	V3 (double x, double y, double z): x(x), y(y), z(z) {}
	V3 operator+ (const V3 &b) const { return {x+b.x, y+b.y, z+b.z}; }
	V3 operator- (const V3 &b) const { return {x-b.x, y-b.y, z-b.z}; }
	V3 operator* (double s) const { return {x*s, y*s, z*s}; }
	double Dot (const V3 &b) const { return x*b.x + y*b.y + z*b.z; }
	double Len () const;
};

class Track {
public:
	/// maxAge: how far back the path is kept [s]; minStep: the shortest
	/// segment [s] (closer points move the last one)
	explicit Track (double maxAge = 60.0, double minStep = 1.0 / 30.0);

	/// Add the source's position at time t (times increasing)
	void Add (double t, const V3 &p);
	/// Forget the path; it then starts with its next points (going on
	/// before them as it starts)
	void Clear () { pts.clear (), pruned = false; }
	/// Forget the path; the source appeared at its next point (a jump, a
	/// new vessel): no sound from before it
	void Restart () { pts.clear (), pruned = true; }
	bool Empty () const { return pts.empty (); }
	size_t Size () const { return pts.size (); }

	/// A moment of the source heard by the listener
	struct Image {
		double tau;      // emission time
		V3 pos;          // the source's position then
		V3 vel;          // and velocity
	};
	/// The emission times heard at the listener (position L) at time t,
	/// in air with speed of sound c, newest first. Returns their number
	/// (at most maxn written).
	int Heard (double t, const V3 &L, double c, Image *out, int maxn) const;

private:
	struct Pt { double t; V3 p; double s; }; // s: path length from the first point
	std::deque<Pt> pts;
	double maxAge, minStep;
	bool pruned = false; // (the oldest points were dropped)
};

} // namespace audio

#endif // !__AUDIOTRACK_H
