// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A sound source: what plays into an emitter's path node (a file, a
// generator, a synthesizer), and how sources are owned - SourcePtr, whose
// deleter takes a source out of the audio graph before it is destroyed.
// =======================================================================

#ifndef __AUDIOSOURCE_H
#define __AUDIOSOURCE_H

#include <memory>

namespace audio {

/// What makes the sound of an emitter: a sound file, a generator, a
/// synthesizer. It plays into the emitter's path node and follows the
/// Doppler factor (its playback rate).
class Source {
public:
	virtual ~Source () = default;
	/// Connect the output to a node's input 0 (mono)
	virtual bool Attach (void *node) = 0;
	virtual void Start () = 0;
	virtual void Stop () = 0;
	/// The Doppler factor (received / emitted frequency)
	virtual void SetDoppler (float f) = 0;
	/// A pitch of its own (sound files: played faster), times the Doppler factor
	virtual void SetPitch (float p) {}
	/// It has played to its end (sound files not looped)
	virtual bool Done () const { return false; }
	/// Held (paused) or going again from where it was held
	virtual void Pause (bool p) { if (p) Stop (); else Start (); }
	/// Sound files: to a time [s] in the file; where it is [s] (< 0: not a file)
	virtual bool Seek (double s) { return false; }
	virtual double Cursor () const { return -1.0; }
	/// A continuous sound, its state all in its parameters (an engine, the
	/// wind): stopped while it can't be heard (World: the voice cap), going
	/// on as it is when it can (one-shots aren't: they must finish in time)
	virtual bool Continuous () const { return false; }
	/// How much it sounds by itself now (0: silent - an engine off; 1: as
	/// loud as it goes), from its parameters (the voice cap's ranking)
	virtual float Activity () const { return 1.0f; }
	/// Out of the audio graph, waiting for the audio thread to leave it:
	/// called by SourceDeleter while the whole object still lives, before
	/// any destructor runs (a derived class's members are what it renders)
	virtual void Detach () {}
};

/// Deletes a source the safe way: out of the graph first (Detach), then
/// destroyed - the audio thread never renders a half-destroyed source
struct SourceDeleter {
	void operator() (Source *s) const { if (s) { s->Detach (); delete s; } }
};
/// How sources are owned (every synthesizer's Make returns one)
template <class T> using SourcePtr = std::unique_ptr<T, SourceDeleter>;

} // namespace audio

#endif // !__AUDIOSOURCE_H
