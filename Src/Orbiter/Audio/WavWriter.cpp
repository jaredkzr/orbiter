// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "WavWriter.h"
#include <cstring>

namespace audio {

namespace {
	void Put16 (unsigned char *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
	void Put32 (unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xff; }
}

bool WavWriter::Open (const std::string &path, unsigned sampleRate, unsigned channels)
{
	Close ();
	f = fopen (path.c_str(), "wb");
	if (!f) return false;
	rate = sampleRate, nch = channels, frames = 0;
	WriteHeader ();
	return true;
}

void WavWriter::WriteHeader ()
{
	// RIFF/WAVE, format 3 (IEEE float) with the extension size field and a
	// fact chunk, as the format asks for non-PCM data
	unsigned char h[58];
	uint64_t bytes64 = frames * nch * 4;
	uint32_t bytes = bytes64 > 0xffffffc0u ? 0xffffffc0u : (uint32_t)bytes64;
	memcpy (h, "RIFF", 4);    Put32 (h + 4, 50 + bytes);
	memcpy (h + 8, "WAVE", 4);
	memcpy (h + 12, "fmt ", 4); Put32 (h + 16, 18);
	Put16 (h + 20, 3);          // IEEE float
	Put16 (h + 22, (uint16_t)nch);
	Put32 (h + 24, rate);
	Put32 (h + 28, rate * nch * 4);
	Put16 (h + 32, (uint16_t)(nch * 4));
	Put16 (h + 34, 32);
	Put16 (h + 36, 0);          // (no extension)
	memcpy (h + 38, "fact", 4); Put32 (h + 42, 4); Put32 (h + 46, (uint32_t)frames);
	memcpy (h + 50, "data", 4); Put32 (h + 54, bytes);
	fseek (f, 0, SEEK_SET);
	fwrite (h, 1, sizeof(h), f);
	fseek (f, 0, SEEK_END);
}

void WavWriter::Write (const float *data, uint64_t n)
{
	if (!f || !n) return;
	// (little-endian float, as the machines this runs on)
	fwrite (data, sizeof(float) * nch, n, f);
	frames += n;
}

void WavWriter::Flush ()
{
	if (!f) return;
	WriteHeader ();
	fflush (f);
}

void WavWriter::Close ()
{
	if (!f) return;
	WriteHeader ();
	fclose (f);
	f = nullptr;
}

} // namespace audio
