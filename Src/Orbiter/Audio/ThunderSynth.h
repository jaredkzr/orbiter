// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Thunder: the sound of a lightning channel, after Few's model (Few 1969,
// "Power spectrum of thunder"; Few 1982 in Handbook of Atmospherics).
//
//  - the channel: kilometres long, tortuous - straight segments a few tens
//    of metres long, each kinked from the last by some 16 degrees on
//    average
//  - each segment: heated in microseconds, its shock relaxes into a weak
//    N-wave at the relaxation radius R0 = sqrt (E_l / (pi p0)) (E_l: the
//    energy per metre; about 0.5 m on Earth); the wave's length about
//    2.6 R0 / c, lengthening as d^1/4 as it travels; its overpressure
//    falling as (R0/d)^3/4 within the segment's length (a cylinder), then
//    as 1/d; most of it radiated broadside (a lobe about the plane
//    normal to the segment)
//  - what is heard: every segment's N-wave, each arriving at its own
//    distance over c - the nearest first: the crack (a near channel), the
//    roll and rumble of the rest; each stroke of the flash again
//
// Rendered once on the simulation thread for the listener's position when
// the flash happens (a buffer of pressure at the listener [Pa], its first
// arrival at the start), played when the first sound arrives.
// =======================================================================

#ifndef __THUNDERSYNTH_H
#define __THUNDERSYNTH_H

#include "Synth.h"
#include <vector>

namespace audio {

class ThunderSynth : public SynthSource {
public:
	struct Channel {
		std::vector<double> x, y, z;   // its points [m] (a frame of the listener's choosing)
	};
	/// A tortuous channel from a start along a direction, `length` [m] long
	/// (segments of seg [m], kinked about 16 degrees on average)
	static Channel Make (const double start[3], const double dir[3], double length, double seg, uint32_t seed);
	/// The relaxation radius [m] for an energy per length [J/m] in air at
	/// pressure p [Pa]
	static double RelaxationRadius (double El, double p) { return std::sqrt (El / (3.14159265 * std::max (p, 1.0))); }

	/// Render the thunder heard at `listener` (same frame) from the channel's
	/// strokes (their times [s] after the first), in air of pressure p and
	/// sound speed c; returns the first arrival's distance [m]. Pressure at
	/// the listener [Pa]; dmin: the nearest point's distance [m]
	static std::vector<float> Render (const Channel &ch, const double listener[3], const std::vector<double> &strokes,
		double El, double p, double c, unsigned rate, double &dmin);

	/// A buffer to play (pressure, normalized by the caller's level)
	static SourcePtr<ThunderSynth> Make (Engine &e, std::vector<float> &&buf);
	void Fire () { fire.store (true, std::memory_order_release); }
	bool Done () const override { return done.load (std::memory_order_acquire); }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit ThunderSynth (std::vector<float> &&b): buf (std::move (b)) {}
	std::vector<float> buf;
	std::atomic<bool> fire{false}, done{false};
	size_t pos = 0;
	double fpos = 0.0;
};

} // namespace audio

#endif // !__THUNDERSYNTH_H
