// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Particle streams of the core API (exhaust, contrails, reentry plasma):
// each stream owns a ring of particles in the shared GPU pool
// (ParticleSystem) and decides how many particles it emits along its
// source's path in a time step (as add-ons define them, PARTICLESTREAMSPEC).
// =======================================================================

#ifndef __PARTICLESTREAM_H
#define __PARTICLESTREAM_H

#include "Particles.h"

class VkParticleStream : public oapi::ParticleStream {
public:
	enum class Kind { Generic, Exhaust, Reentry };

	VkParticleStream (oapi::GraphicsClient *gc, TextureManager *texmgr, PARTICLESTREAMSPEC *pss, Kind kind);
	~VkParticleStream ();

	/// Emit the particles of the simulation step dt [s]
	void Update (double dt);
	/// Draw the particles (scene with reactive mask)
	void Render (Scene *scene);
	/// Detached from its source and no particles left: can be deleted
	bool Expired () const;

private:
	double LevelAlpha (double lvl) const;
	double AtmAlpha (double rho) const;
	/// the ring of particles in the pool (allocated when first needed: the
	/// pool belongs to the scene, which may not exist yet)
	bool EnsureRing ();
	bool SourceState (VECTOR3 &pos, VECTOR3 &dir, VECTOR3 &vel, double &lvl);

	PARTICLESTREAMSPEC spec;
	Kind kind;
	TextureManager *texmgr;
	class VulkanClient *client;
	ParticleSystem *sys = nullptr; // (that of the ring)
	VkTexture *tex = nullptr;
	uint32_t first = 0, capacity = 0, head = 0; // ring in the pool
	bool allocated = false;
	double lastEmit = -1e30, simTime = 0.0;     // [s]
	std::mt19937 rng;
	double emitAccum = 0.0;
	bool havePrev = false;
	VECTOR3 prevSrc{};
	OBJHANDLE hAtmRef = nullptr;  // body whose atmosphere slows the particles
	double reentryLevel = 0.0;
};

#endif // !__PARTICLESTREAM_H
