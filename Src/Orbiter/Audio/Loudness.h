// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Loudness (ITU-R BS.1770-4): K-weighted (a high-pass at 38 Hz, a +4 dB
// shelf above 1.7 kHz), in 400 ms blocks overlapping by 75 %, gated
// (absolute -70 LUFS, then relative -10 LU). The voice's and the radio's
// recordings are brought to one loudness with it: callouts recorded by
// different people at different levels sound alike.
// =======================================================================

#ifndef __AUDIOLOUDNESS_H
#define __AUDIOLOUDNESS_H

#include <string>

namespace audio {

/// Integrated loudness [LUFS] of interleaved frames (-inf: silent)
double Loudness (const float *x, size_t frames, unsigned channels, unsigned rate);

/// A sound file's loudness [LUFS] (decoded; remembered for each file;
/// NAN: not readable)
double FileLoudness (const std::string &path);

/// The gain bringing a file to a loudness [LUFS] (1: not measurable), at
/// most +-maxDb, and raising its peak no higher than PEAK_MAX
double LoudnessGain (const std::string &path, double target, double maxDb = 12.0);
constexpr double PEAK_MAX = 0.891; // (-1 dBFS)

} // namespace audio

#endif // !__AUDIOLOUDNESS_H
