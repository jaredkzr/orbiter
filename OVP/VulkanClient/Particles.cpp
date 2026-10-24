// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Particles.h"
#include "Scene.h"
#include "VkTexture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cmath>

namespace {

	constexpr size_t MAX_PARTICLES = 5000;

	OBJHANDLE FindSun ()
	{
		for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
			OBJHANDLE h = oapiGetGbodyByIndex (i);
			if (oapiGetObjectType (h) == OBJTP_STAR) return h;
		}
		return nullptr;
	}

	double Smoothstep (double e0, double e1, double x)
	{
		double t = std::clamp ((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

} // namespace

VkParticleStream::VkParticleStream (oapi::GraphicsClient *gc, TextureManager *tm, PARTICLESTREAMSPEC *pss, Kind k)
	: oapi::ParticleStream (gc, pss), kind(k), texmgr(tm), rng((unsigned)(uintptr_t)this)
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
	texmgr->Release (tex);
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

	// lighting of diffuse (smoke) particles: day or night side of the planet
	if (spec.ltype == PARTICLESTREAMSPEC::DIFFUSE) {
		OBJHANDLE hPlanet = v->GetSurfaceRef ();
		OBJHANDLE hSun = FindSun ();
		if (hPlanet && hSun) {
			VECTOR3 pp, sp;
			oapiGetGlobalPos (hPlanet, &pp);
			oapiGetGlobalPos (hSun, &sp);
			VECTOR3 up = unit (gpos - pp), s = unit (sp - gpos);
			light = (float)(0.15 + 0.85 * Smoothstep (-0.15, 0.1, dotp (up, s)));
		}
	}
	return true;
}

void VkParticleStream::Update (double dt)
{
	if (dt <= 0.0) return;

	// age and move existing particles; in an atmosphere they are slowed
	// down towards the velocity of the co-rotating air
	VECTOR3 bodyPos{}, bodyVel{}, omega{};
	bool slow = hAtmRef && spec.atmslowdown > 0.0;
	if (slow) {
		oapiGetGlobalPos (hAtmRef, &bodyPos);
		oapiGetGlobalVel (hAtmRef, &bodyVel);
		double T = oapiGetPlanetPeriod (hAtmRef);
		if (T != 0.0) {
			MATRIX3 R;
			oapiGetRotationMatrix (hAtmRef, &R);
			omega = mul (R, _V(0, 1, 0)) * (PI2 / T);
		}
	}
	double damp = slow ? std::exp (-spec.atmslowdown * dt) : 1.0;
	size_t n = 0;
	for (auto &p : particles) {
		p.age += dt;
		if (p.age >= p.life) continue;
		if (slow) {
			VECTOR3 air = bodyVel + crossp (p.pos - bodyPos, omega); // left-handed frame
			p.vel = air + (p.vel - air) * damp;
		}
		p.pos += p.vel * dt;
		particles[n++] = p;
	}
	particles.resize (n);

	// emit new particles along the source path of this step
	VECTOR3 src, dir, vel;
	double alpha;
	if (!SourceState (src, dir, vel, alpha) || alpha < 0.01) {
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
	alpha *= alphaScale;
	emitAccum += rate * dt;
	int nemit = (int)emitAccum;
	emitAccum -= nemit;
	std::uniform_real_distribution<double> uni (0.0, 1.0);
	// the stream direction is the thrust direction: the exhaust leaves the
	// other way. srcspread randomises the velocity by up to +-v0*srcspread/2
	// per axis.
	double spread = spec.srcspread * spec.v0;
	for (int i = 0; i < nemit && particles.size() < MAX_PARTICLES; i++) {
		double f = (i + uni (rng)) / nemit; // emission time within the step
		Particle p;
		VECTOR3 dv = _V(uni (rng) - 0.5, uni (rng) - 0.5, uni (rng) - 0.5) * spread;
		p.vel = vel - dir * spec.v0 + dv;
		p.pos = prevSrc + (src - prevSrc) * f + p.vel * ((1.0 - f) * dt);
		p.age = (1.0 - f) * dt;
		p.life = std::max (0.01, spec.lifetime * (0.5 + uni (rng)));
		p.alpha0 = (float)alpha;
		p.sub = kind == Kind::Reentry ? 1 : (uint8_t)(rng() & 3);
		particles.push_back (p);
	}
	prevSrc = src;
}

void VkParticleStream::Render (Scene *scene)
{
	if (particles.empty() || !tex) return;
	bool emissive = (spec.ltype == PARTICLESTREAMSPEC::EMISSIVE);
	float c = emissive ? 1.0f : light;
	std::vector<PVtx> v;
	v.reserve (particles.size() * 6);
	for (const auto &p : particles) {
		float t = (float)(p.age / p.life);
		float a = p.alpha0 * (1.0f - t);
		if (a <= 0.0f) continue;
		float h = (float)(0.5 * (spec.srcsize + spec.growthrate * p.age));
		VECTOR3 vp = scene->ViewPos (p.pos);
		float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z;
		float u0 = (p.sub & 1) * 0.5f, v0 = (p.sub >> 1) * 0.5f;
		PVtx q[4] = {
			{ x - h, y - h, z, u0,        v0,        c, c, c, a },
			{ x + h, y - h, z, u0 + 0.5f, v0,        c, c, c, a },
			{ x + h, y + h, z, u0 + 0.5f, v0 + 0.5f, c, c, c, a },
			{ x - h, y + h, z, u0,        v0 + 0.5f, c, c, c, a },
		};
		v.push_back (q[0]); v.push_back (q[1]); v.push_back (q[2]);
		v.push_back (q[0]); v.push_back (q[2]); v.push_back (q[3]);
	}
	scene->DrawBillboards (tex, emissive, v.data(), v.size());
}
