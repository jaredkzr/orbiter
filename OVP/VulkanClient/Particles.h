// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Particle streams (exhaust plumes, contrails, reentry plasma)
// =======================================================================

#ifndef __PARTICLES_H
#define __PARTICLES_H

#include "GraphicsAPI.h"
#include <random>
#include <vector>

class VkTexture;
class Scene;
class TextureManager;

/// Billboard vertex: view-space position, texture coordinates, linear colour
struct PVtx {
	float x, y, z, u, v;
	float r, g, b, a;
};

class VkParticleStream : public oapi::ParticleStream {
public:
	enum class Kind { Generic, Exhaust, Reentry };

	VkParticleStream (oapi::GraphicsClient *gc, TextureManager *texmgr, PARTICLESTREAMSPEC *pss, Kind kind);
	~VkParticleStream ();

	/// Advance the particles by the simulation step dt [s]
	void Update (double dt);
	/// Append camera-facing billboards to the scene
	void Render (Scene *scene);
	/// Detached from its source and no particles left: can be deleted
	bool Expired () const { return !hRef && particles.empty(); }

private:
	struct Particle {
		VECTOR3 pos, vel;     // global frame
		double age, life;
		float alpha0;
		uint8_t sub;          // texture quadrant
	};
	double LevelAlpha (double lvl) const;
	double AtmAlpha (double rho) const;
	bool SourceState (VECTOR3 &pos, VECTOR3 &dir, VECTOR3 &vel, double &lvl);

	PARTICLESTREAMSPEC spec;
	Kind kind;
	TextureManager *texmgr;
	VkTexture *tex = nullptr;
	std::vector<Particle> particles;
	std::mt19937 rng;
	double emitAccum = 0.0;
	bool havePrev = false;
	VECTOR3 prevSrc{};
	OBJHANDLE hAtmRef = nullptr;  // body whose atmosphere slows the particles
	VECTOR3 sunDir{ 1, 0, 0 };    // for diffuse lighting
	float light = 1.0f;
	double reentryLevel = 0.0;
};

#endif // !__PARTICLES_H
