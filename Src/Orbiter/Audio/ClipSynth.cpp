// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ClipSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

SourcePtr<ClipSynth> ClipSynth::Make (Engine &e, ClipRef clip, bool loop)
{
	if (!clip || !clip->frames) return nullptr;
	SourcePtr<ClipSynth> s (new ClipSynth (std::move (clip), loop));
	if (!s->Init (e)) return nullptr;
	return s;
}

float ClipSynth::Sample (size_t i) const
{
	const unsigned ch = clip->channels;
	const float *f = clip->pcm.data () + i * ch;
	if (ch == 1) return f[0];
	float s = 0.0f;
	for (unsigned c = 0; c < ch; c++) s += f[c];
	return s / ch;
}

void ClipSynth::Render (float *out, unsigned n, float dop)
{
	const double seek = seekTo.exchange (-1.0, std::memory_order_acq_rel);
	if (seek >= 0.0) pos = std::min (seek * clip->rate, (double)clip->frames);
	const size_t N = clip->frames;
	// the rate: the clip's to the engine's, its pitch, the Doppler factor - from the last block's to
	// this one's across the block
	const float target = (float)clip->rate / rate * pitch.load (std::memory_order_relaxed) * dop;
	if (!begun) glide = target, begun = true;
	const float r0 = glide, dr = (target - r0) / (float)std::max (n, 1u);
	for (unsigned i = 0; i < n; i++) {
		if (pos >= (double)N) {
			if (loop) pos = std::fmod (pos, (double)N);
			else { out[i] = 0.0f; continue; }
		}
		const size_t k = (size_t)pos;
		const float f = (float)(pos - (double)k);
		const float a = Sample (k), b = k + 1 < N ? Sample (k + 1) : (loop ? Sample (0) : 0.0f);
		out[i] = a + f * (b - a);
		pos += r0 + dr * (float)(i + 1);
	}
	glide = target;
	if (loop && pos >= (double)N) pos = std::fmod (pos, (double)N);   // (at its end exactly: its start)
	cursor.store (std::min (pos, (double)N) / clip->rate, std::memory_order_relaxed);
	if (!loop && pos >= (double)N) done.store (true, std::memory_order_release);
}

} // namespace audio
