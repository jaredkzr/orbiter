// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A recording on an emitter, played as a synthesizer: its Doppler factor
// (SynthSource's, eased over a step) and its own pitch glide, block by
// block - not stepped once a frame (an ma_sound's pitch can't glide). Read
// from the decoded clip (Assets) at a fractional position, linearly
// interpolated; channels mixed to mono. Looped or once; seek, cursor.
// =======================================================================

#ifndef __AUDIOCLIPSYNTH_H
#define __AUDIOCLIPSYNTH_H

#include "Synth.h"
#include "Assets.h"

namespace audio {

class ClipSynth : public SynthSource {
public:
	static SourcePtr<ClipSynth> Make (Engine &e, ClipRef clip, bool loop);
	void SetPitch (float p) override { pitch.store (std::max (p, 0.01f), std::memory_order_relaxed); }
	bool Done () const override { return done.load (std::memory_order_acquire); }
	bool Seek (double s) override { seekTo.store (std::max (0.0, s), std::memory_order_release); done.store (false, std::memory_order_release); return true; }
	double Cursor () const override { return cursor.load (std::memory_order_relaxed); }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	ClipSynth (ClipRef c, bool l): clip (std::move (c)), loop (l) {}
	ClipRef clip;
	bool loop;
	std::atomic<float> pitch{1.0f};
	std::atomic<double> seekTo{-1.0}, cursor{0.0};
	std::atomic<bool> done{false};
	// (audio thread)
	double pos = 0.0;          // [frames of the clip]
	float glide = 1.0f;        // the rate reached (eased to the block's: no steps)
	bool begun = false;
	float Sample (size_t i) const;
};

} // namespace audio

#endif // !__AUDIOCLIPSYNTH_H
