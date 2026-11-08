// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Particle streams of the core API (exhaust, contrails, reentry plasma):
// each stream owns a ring of particles in the shared GPU pool
// (ParticleSystem::Reserve/Emit/Release) and decides how many particles it
// emits along its source's path in a time step (as add-ons define them,
// PARTICLESTREAMSPEC, with the documented level and atmosphere mappings).
// Where the client draws the effect its own way, the stream steps aside:
// - an emissive exhaust stream on a thruster drawn as a plume volume
//   (its type not Texture): not emitted (the plume is the flame);
// - a diffuse contrail stream (texture "Contrail*"): the add-on's density
//   mapping replaced by the contrail gate (Contrail.h: temperature,
//   humidity), drawn as thin lit lines; on a solid motor it stays smoke;
// - a reentry stream: its level (amin/amax over the friction power) only
//   scales the plasma wake of the vessel (VVessel), unless the vessel's
//   class keeps its add-on reentry streams (ReentryLegacy = TRUE in
//   Config/VK/Plumes/<class>.cfg).
// =======================================================================

#ifndef __PARTICLESTREAM_H
#define __PARTICLESTREAM_H

#include "Particles.h"
#include "Contrail.h"

class PlumeRenderer;

class VkParticleStream : public oapi::ParticleStream {
public:
	enum class Kind { Generic, Exhaust, Reentry };

	VkParticleStream (oapi::GraphicsClient *gc, TextureManager *texmgr, PARTICLESTREAMSPEC *pss, Kind kind);
	~VkParticleStream ();

	/// Emit the particles of the simulation step dt [s]
	void Update (double dt);
	/// Draw the particles (scene with reactive mask)
	void Render (Scene *scene);
	/// Detached from its source: can be deleted (its particles live on in
	/// retired rings)
	bool Expired () const;

	Kind GetKind () const { return kind; }
	OBJHANDLE Source () const { return hRef; }
	/// the stream's opacity now (level and atmosphere mappings; reentry: over
	/// the friction power), 0..1
	double Strength () const { return strength; }

private:
	/// how the stream is drawn (decided once the scene's plume settings are known)
	enum class Mode { Unknown, Normal, Suppressed, Contrail, LevelOnly };
	double LevelAlpha (double lvl) const;
	double AtmAlpha (double x) const;
	bool SourceState (VECTOR3 &pos, VECTOR3 &dir, VECTOR3 &vel, double &alpha);
	void DecideMode ();
	void ReleaseRings ();

	PARTICLESTREAMSPEC spec;
	Kind kind;
	Mode mode = Mode::Unknown;
	double modeT = -1e30;       // [s] sim time the mode was decided (exhausts: again every 2 s)
	bool rocket = false;        // (contrail: the thruster's exhaust carries a rocket's water)
	TextureManager *texmgr;
	class VulkanClient *client;
	PlumeRenderer *plumes = nullptr; // (the scene's, from Render; null: plumes off)
	bool sceneSeen = false;
	VkTexture *tex = nullptr;
	ParticleRing ring;
	contrail::Emitter trail;    // (Mode::Contrail)
	std::mt19937 rng;
	double emitAccum = 0.0;
	bool havePrev = false;
	VECTOR3 prevSrc{};
	OBJHANDLE hAtmRef = nullptr;  // body whose atmosphere slows the particles
	double rho = 0.0;             // air density at the source [kg/m^3]
	VECTOR3 airVec{};             // the source's velocity relative to the air (global) [m/s]
	bool haveAir = false;
	double strength = 0.0;
};

#endif // !__PARTICLESTREAM_H
