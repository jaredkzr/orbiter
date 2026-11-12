// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// miniaudio's implementation (Extern/miniaudio, public domain or MIT-0),
// compiled once for the core. Decoders: WAV, FLAC and MP3 (built in);
// generators (waveforms, noise) for tests and synthesis.
// The backends are loaded at run time (PipeWire through PulseAudio, ALSA).

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING          // (WavWriter writes the renders)
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_JACK
#define MA_ENABLE_NULL
#include "miniaudio.h"
