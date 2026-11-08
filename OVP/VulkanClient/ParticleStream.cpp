// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ParticleStream.h"
#include "Scene.h"
#include "VulkanClient.h"
#include "VkTexture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cmath>

// ======================================================================
// VkParticleStream

VkParticleStream::VkParticleStream (oapi::GraphicsClient *gc, TextureManager *tm, PARTICLESTREAMSPEC *pss, Kind k)
	: oapi::ParticleStream (gc, pss), kind(k), texmgr(tm), client((VulkanClient*)gc), rng((unsigned)(uintptr_t)this)
{
	if (pss) spec = *pss;
	else {
		spec = PARTICLESTREAMSPEC{};
		spec.srcsize = 1.0, spec.srcrate = 10.0, spec.lifetime = 2.0;
		spec.ltype = PARTICLESTREAMSPEC::DIFFUSE;
		spec.levelmap = PARTICLESTREAMSPEC::LVL_LIN;
		spec.atmsmap = PARTICLESTREAMSPEC::ATM_FLAT;
	}
	if (spec.tex) {
		tex = (VkTexture*)spec.tex;
		texmgr->AddRef (tex);
	} else {
		tex = texmgr->Load (kind == Kind::Reentry ? "Reentry.dds" : "Contrail1.dds");
	}
}

VkParticleStream::~VkParticleStream ()
{
	if (allocated && sys == client->Particles ()) sys->Free (first, capacity);
	texmgr->Release (tex);
}

bool VkParticleStream::EnsureRing ()
{
	ParticleSystem *ps = client->Particles ();
	if (!ps) return false;
	if (allocated && ps == sys) return true;
	sys = ps;
	head = 0;
	// ring size: the most particles alive at once (the emission rate rises
	// at speed in an atmosphere, up to 60 per second)
	double rate = std::max (spec.srcrate, kind == Kind::Reentry ? 0.0 : 60.0) * sys->density;
	double want = rate * std::max (spec.lifetime, 0.01) * 1.5 + 64.0;
	allocated = sys->Allocate ((uint32_t)std::min (want, 65536.0), first, capacity);
	if (!allocated) oapiWriteLogV ("VulkanClient: particle pool full");
	return allocated;
}

bool VkParticleStream::Expired () const
{
	// (particles live up to 1.5 lifetimes)
	return !hRef && simTime - lastEmit > 1.5 * spec.lifetime + 0.1;
}

double VkParticleStream::LevelAlpha (double lvl) const
{
	switch (spec.levelmap) {
	case PARTICLESTREAMSPEC::LVL_FLAT: return 1.0;
	case PARTICLESTREAMSPEC::LVL_LIN:  return std::clamp (lvl, 0.0, 1.0);
	case PARTICLESTREAMSPEC::LVL_SQRT: return std::sqrt (std::clamp (lvl, 0.0, 1.0));
	case PARTICLESTREAMSPEC::LVL_PLIN:
	case PARTICLESTREAMSPEC::LVL_PSQRT: {
		if (lvl < spec.lmin) return 0.0;
		double a = (spec.lmax > spec.lmin) ? std::min (1.0, (lvl - spec.lmin) / (spec.lmax - spec.lmin)) : 1.0;
		return spec.levelmap == PARTICLESTREAMSPEC::LVL_PSQRT ? std::sqrt (a) : a;
	}
	}
	return 1.0;
}

double VkParticleStream::AtmAlpha (double rho) const
{
	switch (spec.atmsmap) {
	case PARTICLESTREAMSPEC::ATM_FLAT: return 1.0;
	case PARTICLESTREAMSPEC::ATM_PLIN:
		if (spec.amax <= spec.amin) return rho >= spec.amin ? 1.0 : 0.0;
		return std::clamp ((rho - spec.amin) / (spec.amax - spec.amin), 0.0, 1.0);
	case PARTICLESTREAMSPEC::ATM_PLOG:
		if (rho <= spec.amin || spec.amin <= 0.0) return 0.0;
		if (spec.amax <= spec.amin) return 1.0;
		return std::clamp (std::log (rho / spec.amin) / std::log (spec.amax / spec.amin), 0.0, 1.0);
	}
	return 1.0;
}

bool VkParticleStream::SourceState (VECTOR3 &gpos, VECTOR3 &gdir, VECTOR3 &gvel, double &lvl)
{
	if (!hRef) return false;
	VESSEL *v = oapiGetVesselInterface (hRef);
	if (!v) return false;
	v->GetGlobalVel (gvel);
	hAtmRef = v->GetAtmRef ();
	double rho = hAtmRef ? v->GetAtmDensity () : 0.0;

	if (kind == Kind::Reentry) {
		// plasma at the leading side of the vessel, flowing with the air
		VECTOR3 air;
		if (!hAtmRef || !v->GetAirspeedVector (FRAME_GLOBAL, air)) return false;
		double va = length (air);
		if (va < 1.0) return false;
		double q = 0.5 * rho * va * va * va; // heat flux indicator [W/m^2]
		reentryLevel = q > 0.0 ? std::clamp ((std::log10 (q) - 6.0) / 1.5, 0.0, 1.0) : 0.0;
		lvl = reentryLevel;
		gdir = air / -va;
		v->Local2Global (_V(0, 0, 0), gpos);
		gpos += air * (v->GetSize() * 0.5 / va);
		gvel -= air; // emitted at rest relative to the atmosphere
	} else {
		lvl = level ? *level : 1.0;
		v->Local2Global (*pos, gpos);
		v->GlobalRot (*dir, gdir);
	}
	lvl = LevelAlpha (lvl) * AtmAlpha (rho);
	return true;
}

void VkParticleStream::Update (double dt)
{
	if (dt <= 0.0) return;
	simTime += dt;
	VECTOR3 src, dir, vel;
	double alpha;
	if (!EnsureRing () || !SourceState (src, dir, vel, alpha) || alpha < 0.01) {
		havePrev = false;
		emitAccum = 0.0;
		return;
	}
	if (!havePrev) prevSrc = src - vel * dt;
	havePrev = true;
	// When the source moves through the air quickly, more particles are
	// emitted, so that the trail stays continuous (spacing of about twice
	// the particle size) instead of breaking into isolated puffs; their
	// opacity is reduced to keep the trail's density.
	double rate = spec.srcrate, alphaScale = 1.0;
	if (kind != Kind::Reentry && hAtmRef && spec.srcsize > 0.0) {
		VECTOR3 air;
		if (oapiGetVesselInterface (hRef)->GetAirspeedVector (FRAME_GLOBAL, air)) {
			double needed = length (air) / (2.0 * spec.srcsize);
			if (needed > rate) {
				alphaScale = std::max (0.35, std::sqrt (rate / needed));
				rate = std::min (needed, 60.0);
			}
		}
	}
	// more, smaller particles: about the same covered area and density
	double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
	emitAccum += rate * D * dt;
	uint32_t n = (uint32_t)emitAccum;
	emitAccum -= n;
	if (!n) { prevSrc = src; return; }
	ParticleSystem::Emission e{};
	e.p0 = prevSrc, e.p1 = src;
	e.vel = vel, e.dir = dir;
	// (the stream direction is the thrust direction: the exhaust leaves the
	// other way; srcspread randomises the velocity by up to +-v0*srcspread/2
	// per axis)
	e.v0 = spec.v0, e.spread = spec.srcspread * spec.v0;
	e.lifetime = spec.lifetime, e.dt = dt;
	e.size = spec.srcsize * sizeScale, e.growth = spec.growthrate * sizeScale;
	e.alpha = alpha * alphaScale * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
	// exhaust gas takes on the air's velocity quickly (as the plume bends)
	e.slowdown = !hAtmRef ? 0.0 : kind == Kind::Exhaust
		? std::max (spec.atmslowdown, ParticleSystem::ExhaustDrag (oapiGetVesselInterface (hRef)->GetAtmDensity ()))
		: spec.atmslowdown;
	e.first = first, e.capacity = capacity, e.head = head, e.count = std::min (n, capacity);
	e.emissive = spec.ltype == PARTICLESTREAMSPEC::EMISSIVE;
	e.reentry = kind == Kind::Reentry;
	sys->Queue (e);
	head = (head + e.count) % capacity;
	lastEmit = simTime;
	prevSrc = src;
}

void VkParticleStream::Render (Scene *scene)
{
	if (!allocated || sys != client->Particles () || simTime - lastEmit > 1.5 * spec.lifetime + 0.1) return;
	sys->Draw (first, capacity, tex, spec.ltype == PARTICLESTREAMSPEC::EMISSIVE, scene->FrameSet ());
}
