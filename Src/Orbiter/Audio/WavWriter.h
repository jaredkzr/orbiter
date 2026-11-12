// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A WAV file of 32-bit float samples, written as it grows: the header's
// sizes are brought up to date at every Flush, so the file is readable
// even when the process ends without closing it.
// =======================================================================

#ifndef __WAVWRITER_H
#define __WAVWRITER_H

#include <cstdint>
#include <cstdio>
#include <string>

namespace audio {

class WavWriter {
public:
	WavWriter () = default;
	~WavWriter () { Close (); }
	WavWriter (const WavWriter&) = delete;
	WavWriter &operator= (const WavWriter&) = delete;

	bool Open (const std::string &path, unsigned sampleRate, unsigned channels);
	/// Append `frames` interleaved frames
	void Write (const float *data, uint64_t frames);
	/// Bring the header up to date and flush the file
	void Flush ();
	void Close ();

	bool IsOpen () const { return f != nullptr; }
	uint64_t Frames () const { return frames; }

private:
	void WriteHeader ();
	FILE *f = nullptr;
	unsigned rate = 0, nch = 0;
	uint64_t frames = 0;
};

} // namespace audio

#endif // !__WAVWRITER_H
