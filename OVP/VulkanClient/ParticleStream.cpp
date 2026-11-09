// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ParticleStream.h"
#include "Scene.h"
#include "Plumes.h"
#include "VulkanClient.h"
#include "VkTexture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cctype>
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
		spec.lmin = spec.lmax = spec.amin = spec.amax = 1.0; // (the FLAT maps: opaque)
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
	ReleaseRings ();
	texmgr->Release (tex);
}

void VkParticleStream::ReleaseRings ()
{
	// (retired: the particles live on; or freed if they are gone)
	ParticleSystem *ps = client->Particles ();
	if (ring.sys && ring.sys == ps) ps->Release (ring);
	trail.Release (ps);
}

bool VkParticleStream::Expired () const
{
	// (its rings are released when deleted: the particles left live on)
	return !hRef;
}

// The documented mappings (Orbiter Developer Manual, "Particle streams";
// OrbiterAPI.h PARTICLESTREAMSPEC)

double VkParticleStream::LevelAlpha (double L) const
{
	switch (spec.levelmap) {
	case PARTICLESTREAMSPEC::LVL_FLAT: return std::clamp (spec.lmin, 0.0, 1.0);
	case PARTICLESTREAMSPEC::LVL_LIN:  return std::clamp (L, 0.0, 1.0);
	case PARTICLESTREAMSPEC::LVL_SQRT: return std::sqrt (std::clamp (L, 0.0, 1.0));
	case PARTICLESTREAMSPEC::LVL_PLIN:
	case PARTICLESTREAMSPEC::LVL_PSQRT: {
		// (lmin < 0: particles at level 0 too; lmax > 1: never opaque)
		if (L < spec.lmin) return 0.0;
		double a = spec.lmax > spec.lmin ? std::clamp ((L - spec.lmin) / (spec.lmax - spec.lmin), 0.0, 1.0) : 1.0;
		return spec.levelmap == PARTICLESTREAMSPEC::LVL_PSQRT ? std::sqrt (a) : a;
	}
	}
	return 1.0;
}

double VkParticleStream::AtmAlpha (double x) const
{
	// x: the air density (exhaust and other streams), or the friction power
	// 0.5 rho^0.6 v^3 (reentry streams)
	switch (spec.atmsmap) {
	case PARTICLESTREAMSPEC::ATM_FLAT: return std::clamp (spec.amin, 0.0, 1.0);
	case PARTICLESTREAMSPEC::ATM_PLIN:
		if (x < spec.amin) return 0.0;
		if (spec.amax <= spec.amin) return 1.0;
		return std::clamp ((x - spec.amin) / (spec.amax - spec.amin), 0.0, 1.0);
	case PARTICLESTREAMSPEC::ATM_PLOG:
		if (spec.amin <= 0.0 || x < spec.amin) return 0.0;
		if (spec.amax <= spec.amin) return 1.0;
		return std::clamp (std::log (x / spec.amin) / std::log (spec.amax / spec.amin), 0.0, 1.0);
	}
	return 1.0;
}

bool VkParticleStream::SourceState (VECTOR3 &gpos, VECTOR3 &gdir, VECTOR3 &gvel, double &alpha)
{
	if (!hRef) return false;
	VESSEL *v = oapiGetVesselInterface (hRef);
	if (!v) return false;
	v->GetGlobalVel (gvel);
	hAtmRef = v->GetAtmRef ();
	rho = hAtmRef ? v->GetAtmDensity () : 0.0;
	haveAir = hAtmRef && v->GetAirspeedVector (FRAME_GLOBAL, airVec);
	if (!haveAir) airVec = _V(0, 0, 0);
	double va = length (airVec);

	if (kind == Kind::Reentry) {
		// plasma at the leading side of the vessel, flowing with the air
		if (!haveAir || va < 1.0) { strength = 0.0; return false; }
		// (the friction power, as documented for reentry streams)
		double x = 0.5 * std::pow (rho, 0.6) * va * va * va;
		alpha = LevelAlpha (1.0) * AtmAlpha (x);
		gdir = airVec / -va;
		v->Local2Global (_V(0, 0, 0), gpos);
		gpos += airVec * (v->GetSize() * 0.5 / va);
		gvel -= airVec; // emitted at rest relative to the atmosphere
	} else {
		alpha = LevelAlpha (level ? *level : 1.0) * AtmAlpha (rho);
		v->Local2Global (*pos, gpos);
		v->GlobalRot (*dir, gdir);
	}
	strength = alpha;
	return true;
}

void VkParticleStream::DecideMode ()
{
	double simt = oapiGetSimTime ();
	if (mode != Mode::Unknown && (kind != Kind::Exhaust || simt - modeT < 2.0)) return;
	if (!sceneSeen || !hRef) { mode = Mode::Unknown; return; } // (no scene yet: wait)
	VESSEL *v = oapiGetVesselInterface (hRef);
	if (!v) return;
	modeT = simt;
	if (kind == Kind::Reentry) {
		// (plumes off: the class configuration is not read, the sheath is drawn)
		mode = plumes && plumes->config.ReentryLegacy (v) ? Mode::Normal : Mode::LevelOnly;
		return;
	}
	mode = Mode::Normal;
	if (kind != Kind::Exhaust) return;
	// the thruster's exhaust drawn as a plume volume (the core points both
	// an exhaust's and its streams' level at the thruster's level)
	int type = -1;
	for (DWORD i = 0, n = v->GetExhaustCount (); i < n && level && plumes; i++) {
		EXHAUSTSPEC es;
		if (!v->GetExhaustSpec (i, &es) || !es.th || es.level != level) continue;
		PlumeOverride o = plumes->config.Get (v, i);
		type = o.type >= 0 ? o.type : (int)PlumeConfig::Infer (v, es.th);
		if (type != (int)PlumeType::Texture) break;
	}
	bool plume = type >= 0 && type != (int)PlumeType::Texture;
	if (spec.ltype == PARTICLESTREAMSPEC::EMISSIVE) {
		if (plume) mode = Mode::Suppressed; // (the plume is the flame)
		return;
	}
	// a contrail stream (the texture's name, or the default texture)
	std::string name = tex ? tex->name : std::string ();
	for (auto &c : name) c = (char)tolower ((unsigned char)c);
	bool contrailTex = name.find ("contrail") != std::string::npos;
	// the engine's own smoke (EmitSmoke) is the one smoke source: a diffuse
	// exhaust stream on a thruster whose class makes smoke is not drawn too
	// (two columns of different look, stopped at the flame's flank). The
	// condensation trails of liquid engines stay contrails
	if (plume && PlumeConfig::Class ((PlumeType)type).smokeOpacity > 0.0f && !(contrailTex && type != (int)PlumeType::Solid)) {
		mode = Mode::Suppressed;
		return;
	}
	if (!contrailTex) return;
	mode = Mode::Contrail;
	rocket = type == (int)PlumeType::Hydrolox || type == (int)PlumeType::Methalox || type == (int)PlumeType::Kerolox;
}

void VkParticleStream::Update (double dt)
{
	if (dt <= 0.0 || oapiGetPause ()) return;
	ParticleSystem *sys = client->Particles ();
	VECTOR3 src, dir, vel;
	double alpha = 0.0;
	DecideMode ();
	bool ok = SourceState (src, dir, vel, alpha);
	if (!ok) strength = 0.0;
	if (!sys || !sys->enabled || !ok || alpha < 0.01 || mode == Mode::Unknown || mode == Mode::Suppressed || mode == Mode::LevelOnly) {
		havePrev = false;
		emitAccum = 0.0;
		trail.Reset ();
		return;
	}
	if (mode == Mode::Contrail) {
		// the gate instead of the add-on's density mapping: its level only
		double lvl = LevelAlpha (level ? *level : 1.0);
		if (!haveAir || !hAtmRef) { trail.Reset (); return; }
		double P;
		double T = contrail::AirTemperature (hAtmRef, src, &P);
		double lng, lat, rad;
		oapiGlobalToEqu (hAtmRef, src, &lng, &lat, &rad);
		contrail::Gate g = contrail::Evaluate (T, P, rad - oapiGetSize (hAtmRef), rocket, contrail::SessionHumidity (hAtmRef));
		// (the line about half the add-on's puff: it is drawn thin)
		trail.Emit (sys, src, vel, airVec, g, std::clamp (0.6 * spec.srcsize, 1.5, 10.0), lvl, dt);
		return;
	}
	if (!havePrev) prevSrc = src - vel * dt;
	havePrev = true;
	// When the source moves through the air quickly, more particles are
	// emitted, so that the trail stays continuous (spacing of about twice
	// the particle size) instead of breaking into isolated puffs; their
	// opacity is reduced to keep the trail's density.
	double rate = spec.srcrate, alphaScale = 1.0;
	if (kind != Kind::Reentry && haveAir && spec.srcsize > 0.0) {
		double needed = length (airVec) / (2.0 * spec.srcsize);
		if (needed > rate) {
			alphaScale = std::max (0.35, std::sqrt (rate / needed));
			rate = std::min (needed, 60.0);
		}
	}
	// more, smaller particles: about the same covered area and density
	double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
	double life = std::max (spec.lifetime, 0.01);
	// (the ring for the rate emitted: grown, shrunk, released when idle)
	if (!sys->Reserve (ring, rate * D, life, ParticleSystem::Class::Legacy, 0.0, tex, spec.ltype == PARTICLESTREAMSPEC::EMISSIVE)) {
		prevSrc = src;
		return;
	}
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
	e.lifetime = life, e.dt = dt;
	e.size = spec.srcsize * sizeScale, e.growth = spec.growthrate * sizeScale;
	e.alpha = alpha * alphaScale * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
	// the slowdown in the air: atmslowdown at sea level density, scaled by
	// sqrt (rho / 1.293); exhaust gas takes on the air's velocity quickly (as
	// the plume bends)
	double slow = hAtmRef ? spec.atmslowdown * std::sqrt (std::max (rho, 0.0) / 1.293) : 0.0;
	e.slowdown = kind == Kind::Exhaust && hAtmRef ? std::max (slow, ParticleSystem::ExhaustDrag (rho)) : slow;
	e.emissive = spec.ltype == PARTICLESTREAMSPEC::EMISSIVE;
	e.reentry = kind == Kind::Reentry;
	e.cls = ParticleSystem::Class::Legacy;
	sys->Emit (ring, e, n);
	prevSrc = src;
}

void VkParticleStream::Render (Scene *scene)
{
	plumes = scene->Plumes (); // (null: plumes off)
	sceneSeen = true;
	trail.tex = scene->FireballTexture (3);
	ParticleSystem *sys = client->Particles ();
	if (!sys) return;
	sys->Draw (ring, tex, spec.ltype == PARTICLESTREAMSPEC::EMISSIVE, scene->FrameSet ());
	trail.Draw (sys, scene->FrameSet ());
}
