// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Head-related transfer functions: how a sound from a direction reaches
// each ear - its head's shadow, the pinna's colouring, the delay between
// the ears - for headphones (binaural). MIT's KEMAR measurements (710
// directions, elevations -40..90 degrees), diffuse-field equalized,
// minimum-phase filters of 64 taps and each ear's delay apart
// (Utils/Audio/make_hrtf.py, Sound/HRTF/kemar.hrtf).
//
// A direction between the measured ones: the four about it, weighted
// (between two azimuths on each of two elevations).
// =======================================================================

#ifndef __AUDIOHRTF_H
#define __AUDIOHRTF_H

#include <string>
#include <vector>

namespace audio {

class Hrtf {
public:
	static constexpr int TAPS = 64;
	/// The longest delay of an ear [s] (the interaural, and the arrival)
	static constexpr double MAX_DELAY = 0.0012;

	/// Load the file, at this sample rate (other rates: resampled)
	bool Load (const std::string &path, unsigned rate);
	bool Loaded () const { return !dirs.empty (); }
	unsigned Rate () const { return rate; }

	/// The filters (TAPS each) and delays [samples] for a direction in the
	/// listener's frame (x right, y up, z ahead; any length but 0)
	void Get (float x, float y, float z, float *left, float *right, float &delayL, float &delayR) const;
	/// The longest delay [samples]
	int MaxDelay () const { return maxDelay; }

	/// Elevation, azimuth [deg] of a direction (azimuth clockwise from ahead)
	static void Angles (float x, float y, float z, float &el, float &az);

private:
	struct Dir { float el, az, dl, dr; float l[TAPS], r[TAPS]; };
	std::vector<Dir> dirs;
	struct Ring { float el; std::vector<int> idx; }; // (by azimuth)
	std::vector<Ring> rings;                         // (by elevation)
	unsigned rate = 48000;
	int maxDelay = 0;
	/// Two directions on a ring about az, and the second's weight
	void OnRing (const Ring &r, float az, int &a, int &b, float &w) const;
};

} // namespace audio

#endif // !__AUDIOHRTF_H
