// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VObjects.h"
#include "VkMesh.h"
#include "VulkanClient.h"
#include "Particles.h"
#include "PlanetSurface.h"
#include "Atmosphere.h"
#include "Shadows.h"
#include "LegacyTex.h"
#include <map>
#include <random>
#include "VesselAPI.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <unordered_map>

// ======================================================================
// VObject

VObject::VObject (OBJHANDLE h, Scene *s)
	: hObj(h), scene(s)
{
	size = oapiGetSize (hObj);
}

void VObject::Update ()
{
	oapiGetGlobalPos (hObj, &gpos);
	oapiGetRotationMatrix (hObj, &grot);
	camDist = length (gpos - scene->CameraPos());
}

// ======================================================================
// VVessel

VVessel::VVessel (OBJHANDLE h, Scene *s)
	: VObject(h, s)
{
	vessel = oapiGetVesselInterface (hObj);
	LoadMeshes ();
}

VVessel::~VVessel ()
{
	// restore animated vertex lists to their default state
	for (auto &it : vtxListDefaults)
		std::copy (it.second.begin(), it.second.end(), it.first);
}

void VVessel::LoadMeshes ()
{
	meshes.clear();
	UINT n = vessel->GetMeshCount();
	meshes.resize (n);
	for (UINT i = 0; i < n; i++)
		InsertMesh (i);
}

void VVessel::InsertMesh (UINT idx)
{
	if (idx >= meshes.size()) meshes.resize (idx + 1);
	MeshRec &rec = meshes[idx];
	rec = MeshRec();
	MESHHANDLE hMesh = vessel->GetMeshTemplate (idx);
	if (!hMesh) {
		const char *name = vessel->GetMeshName (idx);
		if (name) hMesh = oapiLoadMeshGlobal (name);
	}
	if (!hMesh) return;
	VulkanClient *gc = scene->Client();
	rec.mesh = std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), hMesh);
	const char *mname = vessel->GetMeshName (idx);
	std::string meshName = mname ? std::string (mname) : gc->MeshName (hMesh);
	if (!meshName.empty()) rec.mesh->ApplyMaterialConfig (gc->Materials(), meshName);
	vessel->GetMeshOffset (idx, rec.ofs);
	rec.visMode = vessel->GetMeshVisibilityMode (idx);
	rec.grpTransform.assign (rec.mesh->nGroup(), Mat4::Identity());
	rec.grpVisible.assign (rec.mesh->nGroup(), 1);
}

MESHHANDLE VVessel::GetMesh (UINT idx)
{
	// device mesh handle (see VESSEL::GetDevMesh)
	return (idx < meshes.size() && meshes[idx].mesh ? (MESHHANDLE)meshes[idx].mesh.get() : NULL);
}

int VVessel::VisEvent (DWORD msg, DWORD_PTR context)
{
	switch (msg) {
	case EVENT_VESSEL_INSMESH:
		InsertMesh ((UINT)context);
		return 1;
	case EVENT_VESSEL_DELMESH:
		if ((UINT)context == (UINT)-1) meshes.clear();
		else if ((UINT)context < meshes.size()) meshes[(UINT)context] = MeshRec();
		return 1;
	case EVENT_VESSEL_MESHVISMODE:
		if ((UINT)context < meshes.size())
			meshes[(UINT)context].visMode = vessel->GetMeshVisibilityMode ((UINT)context);
		return 1;
	case EVENT_VESSEL_MESHOFS:
		if ((UINT)context < meshes.size())
			vessel->GetMeshOffset ((UINT)context, meshes[(UINT)context].ofs);
		return 1;
	case EVENT_VESSEL_RESETANIM:
	case EVENT_VESSEL_CLEARANIM:
	case EVENT_VESSEL_DELANIM:
	case EVENT_VESSEL_NEWANIM:
		return 1; // animations are evaluated from their current state every frame
	}
	return 0;
}

void VVessel::Update ()
{
	VObject::Update ();
	UpdateAnimations ();
}

// ----------------------------------------------------------------------
// Animations
//
// The mesh is stored in the animation's default state. Each component maps
// the animation state onto a progress 0..1 over [state0,state1]; its
// transformation is evaluated for the difference between the current and
// the default progress. Child components inherit the transformation of
// their parents.

namespace {

	double Progress (const ANIMATIONCOMP *c, double state)
	{
		if (c->state1 == c->state0) return state >= c->state0 ? 1.0 : 0.0;
		return std::clamp ((state - c->state0) / (c->state1 - c->state0), 0.0, 1.0);
	}

	Mat4 LocalTransform (const ANIMATIONCOMP *c, double state, double defstate)
	{
		double p = Progress (c, state), p0 = Progress (c, defstate);
		double dp = p - p0;
		MGROUP_TRANSFORM *tr = c->trans;
		if (!tr || dp == 0.0) return Mat4::Identity();
		switch (tr->Type()) {
		case MGROUP_TRANSFORM::ROTATE: {
			auto *rot = static_cast<MGROUP_ROTATE*>(tr);
			double len = length (rot->axis);
			if (len == 0.0) return Mat4::Identity();
			return Mat4::RotationAbout (rot->ref, rot->axis / len, rot->angle * dp);
		}
		case MGROUP_TRANSFORM::TRANSLATE: {
			auto *trn = static_cast<MGROUP_TRANSLATE*>(tr);
			return Mat4::Translation (trn->shift * dp);
		}
		case MGROUP_TRANSFORM::SCALE: {
			auto *scl = static_cast<MGROUP_SCALE*>(tr);
			auto f = [](double s, double prog) { return 1.0 + (s - 1.0) * prog; };
			auto ratio = [&](double s) { double d = f(s, p0); return d != 0.0 ? f(s, p) / d : 1.0; };
			return Mat4::Translation (scl->ref) *
				Mat4::Scale (ratio (scl->scale.x), ratio (scl->scale.y), ratio (scl->scale.z)) *
				Mat4::Translation (-scl->ref);
		}
		default:
			return Mat4::Identity();
		}
	}

} // namespace

void VVessel::UpdateAnimations ()
{
	for (auto &rec : meshes) {
		std::fill (rec.grpTransform.begin(), rec.grpTransform.end(), Mat4::Identity());
	}

	ANIMATION *anim = nullptr;
	UINT nanim = vessel->GetAnimPtr (&anim);
	if (!nanim || !anim) return;

	// owning animation of each component (needed for the state of parent components)
	std::unordered_map<const ANIMATIONCOMP*, const ANIMATION*> owner;
	for (UINT a = 0; a < nanim; a++)
		for (UINT c = 0; c < anim[a].ncomp; c++)
			owner[anim[a].comp[c]] = &anim[a];

	std::unordered_map<const ANIMATIONCOMP*, Mat4> compMatrix;
	std::function<Mat4(const ANIMATIONCOMP*)> CompMatrix = [&](const ANIMATIONCOMP *c) -> Mat4 {
		auto it = compMatrix.find (c);
		if (it != compMatrix.end()) return it->second;
		const ANIMATION *a = owner.count (c) ? owner[c] : nullptr;
		Mat4 M = a ? LocalTransform (c, a->state, a->defstate) : Mat4::Identity();
		if (c->parent) M = CompMatrix (c->parent) * M;
		compMatrix[c] = M;
		return M;
	};

	for (UINT a = 0; a < nanim; a++) {
		for (UINT ci = 0; ci < anim[a].ncomp; ci++) {
			const ANIMATIONCOMP *c = anim[a].comp[ci];
			MGROUP_TRANSFORM *tr = c->trans;
			if (!tr || !tr->grp || !tr->ngrp) continue;
			Mat4 M = CompMatrix (c);
			if (tr->mesh == LOCALVERTEXLIST) {
				VECTOR3 *list = (VECTOR3*)tr->grp;
				auto it = vtxListDefaults.find (list);
				if (it == vtxListDefaults.end())
					it = vtxListDefaults.emplace (list, std::vector<VECTOR3> (list, list + tr->ngrp)).first;
				for (UINT k = 0; k < tr->ngrp && k < it->second.size(); k++)
					list[k] = M.TransformPoint (it->second[k]);
			} else if (tr->mesh < meshes.size() && meshes[tr->mesh].mesh) {
				MeshRec &rec = meshes[tr->mesh];
				for (UINT k = 0; k < tr->ngrp; k++) {
					UINT g = tr->grp[k];
					if (g < rec.grpTransform.size())
						rec.grpTransform[g] = M * rec.grpTransform[g];
				}
			}
		}
	}
}

void VVessel::Render (RenderPass pass)
{
	bool internal = oapiCameraInternal() && oapiCameraTarget() == hObj;
	bool vc = internal && oapiCockpitMode() == COCKPIT_VIRTUAL;
	Mat4 mv = scene->ModelView (gpos, grot);
	scene->Sunlight (gpos);
	scene->SetObjectGlassGlow (glassGlow);
	SetHeating ();

	// virtual cockpit displays: MFD and HUD surfaces replace the textures of the registered groups
	std::vector<std::vector<GroupTexOverride>> ovr;
	if (vc) {
		ovr.resize (meshes.size());
		VulkanClient *gc = scene->Client();
		for (int i = 0; i < MAXMFD; i++) {
			const VCMFDSPEC *spec = nullptr;
			SURFHANDLE surf = gc->GetVCMFDSurface (i, &spec);
			if (surf && spec && spec->nmesh < meshes.size())
				ovr[spec->nmesh].push_back ({ spec->ngroup, (VkTexture*)surf, false });
		}
		const VCHUDSPEC *hs = nullptr;
		SURFHANDLE hud = gc->GetVCHUDSurface (&hs);
		if (hud && hs && hs->nmesh < meshes.size())
			ovr[hs->nmesh].push_back ({ hs->ngroup, (VkTexture*)hud, true });
	}

	for (size_t m = 0; m < meshes.size(); m++) {
		MeshRec &rec = meshes[m];
		if (!rec.mesh) continue;
		bool visible;
		if (!internal)
			visible = (rec.visMode & MESHVIS_EXTERNAL) != 0;
		else
			visible = (rec.visMode & MESHVIS_COCKPIT) || (vc && (rec.visMode & MESHVIS_VC)) || (rec.visMode & MESHVIS_EXTPASS);
		if (!visible) continue;
		Mat4 meshMv = mv * Mat4::Translation (rec.ofs);
		scene->DrawMesh (rec.mesh.get(), meshMv, pass, &rec.grpTransform, &rec.grpVisible,
			(m < ovr.size() && !ovr[m].empty()) ? &ovr[m] : nullptr);
	}
	static const float noGlow[3] = { 0, 0, 0 };
	scene->SetObjectGlassGlow (noGlow);
	scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
	if (pass == RenderPass::Transparent) RenderExhaust ();
}

void VVessel::SetHeating ()
{
	// stagnation-point heat flux (Sutton-Graves, 1 m nose radius) [W/m^2]
	VESSEL *v = oapiGetVesselInterface (hObj);
	VECTOR3 as;
	v->GetAirspeedVector (FRAME_LOCAL, as);
	double spd = length (as), rho = v->GetAtmDensity ();
	double q = 1.74e-4 * sqrt (rho) * spd * spd * spd;
	const double qMin = 1e5, qMax = 8e5; // glow onset, full glow
	float t = (float)std::clamp ((q - qMin) / (qMax - qMin), 0.0, 1.0);
	if (t <= 0.0f || spd <= 0.0) {
		scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
		return;
	}
	VECTOR3 dir = scene->ViewDir (mul (grot, as / spd));
	scene->SetObjectHeat (dir, t);
}

void VVessel::RenderShadow (ShadowMap &sm)
{
	if (length (gpos - sm.Centre()) > sm.Radius() + size) return;
	Mat4 model = Mat4::FromRotTrans (grot, gpos - sm.Centre());
	for (auto &rec : meshes)
		if (rec.mesh && (rec.visMode & MESHVIS_EXTERNAL))
			sm.DrawMesh (rec.mesh.get(), model * Mat4::Translation (rec.ofs), &rec.grpTransform, &rec.grpVisible);
}

namespace {
	// beacons as light sources: half intensity at this many beacon radii
	constexpr double BEACON_LIGHT_REACH = 8.0;
	constexpr double BEACON_LIGHT_INTENSITY = 0.8;
	// interior lights seen from outside: intensity factor, range factor, and
	// the emission of the vessel's glass per unit of light
	constexpr double INTERIOR_SPILL = 0.5;
	constexpr double INTERIOR_SPILL_REACH = 4.0;
	constexpr float INTERIOR_GLASS_GLOW = 0.6f;
	constexpr float BEACON_GLOW = 3.0f; // brightness of beacon sprites
	constexpr float STAR_BRIGHTNESS = 20.0f;      // the sun's disc relative to lit white surfaces
	constexpr float STAR_GLOW = 2.0f;             // brightness of the glow around it
	constexpr double STAR_GLOW_SIZE = 6.0;        // glow radius [star radii]
	constexpr double STAR_GLOW_MIN_ANGLE = 0.02;  // minimum glow radius [rad]

	bool BeaconLit (const BEACONLIGHTSPEC *bs, double simt)
	{
		if (!bs->active || !bs->pos || !bs->col) return false;
		return bs->period <= 0.0 || fmod (simt + bs->tofs, bs->period) <= bs->duration;
	}
}

void VVessel::CollectLights (std::vector<LightSource> &lights)
{
	glassGlow[0] = glassGlow[1] = glassGlow[2] = 0.0f;
	bool internal = oapiCameraInternal() && oapiCameraTarget() == hObj;
	VECTOR3 cam = scene->CameraPos();
	for (DWORD i = 0; i < vessel->LightEmitterCount(); i++) {
		const LightEmitter *le = vessel->GetLightEmitter (i);
		if (!le || !le->IsActive()) continue;
		int vis = le->GetVisibility();
		// Interior lights seen from outside spill through the windows: a weaker
		// light reaching further, and a glow of the vessel's glass.
		bool spill = !internal && !(vis & LightEmitter::VIS_EXTERNAL);
		if (internal && !(vis & LightEmitter::VIS_COCKPIT)) continue;
		double in = le->GetIntensity();
		if (in <= 0.0) continue;
		if (spill) in *= INTERIOR_SPILL;
		LightSource l{};
		l.pos = gpos + mul (grot, le->GetPosition());
		if (le->GetType() == LightEmitter::LT_POINT) {
			const PointLight *pl = (const PointLight*)le;
			l.range = pl->GetRange();
			for (int k = 0; k < 3; k++) l.att[k] = pl->GetAttenuation()[k];
		} else if (le->GetType() == LightEmitter::LT_SPOT) {
			const SpotLight *sl = (const SpotLight*)le;
			l.range = sl->GetRange();
			for (int k = 0; k < 3; k++) l.att[k] = sl->GetAttenuation()[k];
			l.spot = true;
			l.dir = mul (grot, le->GetDirection());
			l.umbra = sl->GetUmbra();
			l.penumbra = sl->GetPenumbra();
		} else {
			continue;
		}
		if (spill) {
			l.range *= INTERIOR_SPILL_REACH;
			l.att[1] /= INTERIOR_SPILL_REACH;
			l.att[2] /= INTERIOR_SPILL_REACH * INTERIOR_SPILL_REACH;
		}
		if (length (l.pos - cam) > l.range + 10e3) continue; // nothing in view can be lit
		const COLOUR4 &c = le->GetDiffuseColour();
		l.col[0] = (float)(c.r * in), l.col[1] = (float)(c.g * in), l.col[2] = (float)(c.b * in);
		if (spill)
			for (int k = 0; k < 3; k++) glassGlow[k] += l.col[k] * INTERIOR_GLASS_GLOW;
		lights.push_back (l);
	}

	// lit beacons also illuminate their surroundings (mainly the vessel itself)
	double simt = oapiGetSimTime ();
	for (DWORD i = 0; ; i++) {
		const BEACONLIGHTSPEC *bs = vessel->GetBeacon (i);
		if (!bs) break;
		if (!BeaconLit (bs, simt)) continue;
		LightSource l{};
		l.pos = gpos + mul (grot, *bs->pos);
		double reach = BEACON_LIGHT_REACH * bs->size; // distance of half intensity
		l.range = 4.0 * reach;
		if (length (l.pos - cam) > l.range + 2e3) continue;
		l.att[0] = 1.0, l.att[1] = 0.0, l.att[2] = 1.0 / (reach * reach);
		for (int k = 0; k < 3; k++) l.col[k] = (float)((&bs->col->x)[k] * BEACON_LIGHT_INTENSITY);
		lights.push_back (l);
	}
}

void VVessel::RenderExhaust ()
{
	// Exhaust textures hold the flame strip in u = [0, 1/4] (nozzle at v = 0)
	// and a glow sprite in [1/2, 1] x [0, 1/2].
	DWORD n = vessel->GetExhaustCount ();
	static std::mt19937 rng (12345);
	std::uniform_real_distribution<double> uni (0.0, 1.0);
	std::map<VkTexture*, std::vector<PVtx>> batches;
	for (DWORD i = 0; i < n; i++) {
		double lvl = vessel->GetExhaustLevel (i);
		if (lvl <= 0.0) continue;
		EXHAUSTSPEC es;
		if (!vessel->GetExhaustSpec (i, &es) || !es.lpos || !es.ldir) continue;
		VECTOR3 gd = mul (grot, *es.ldir);
		VECTOR3 s0 = gpos + mul (grot, *es.lpos) - gd * es.lofs;
		VECTOR3 s1 = s0 - gd * es.lsize;
		float a = (float)(std::min (1.0, lvl) * (1.0 - es.modulate * uni (rng)));
		VECTOR3 vs = scene->ViewPos (s0), ve = scene->ViewPos (s1);
		VECTOR3 axis = ve - vs, mid = (vs + ve) * 0.5;
		VECTOR3 perp = crossp (axis, mid);
		double len = length (perp);
		if (len < 1e-9) continue; // looking straight along the flame
		perp *= 0.5 * es.wsize / len;
		VkTexture *tex = es.tex ? (VkTexture*)es.tex : scene->DefaultExhaustTex ();
		auto &v = batches[tex];
		auto P = [&](const VECTOR3 &p, float u, float t) {
			return PVtx{ (float)p.x, (float)p.y, (float)p.z, u, t, 1.0f, 1.0f, 1.0f, a };
		};
		PVtx q[4] = { P (vs + perp, 0.0f, 0.0f), P (vs - perp, 0.25f, 0.0f), P (ve - perp, 0.25f, 1.0f), P (ve + perp, 0.0f, 1.0f) };
		v.insert (v.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
		// glow at the nozzle, facing the camera
		float h = (float)es.wsize;
		float x = (float)vs.x, y = (float)vs.y, z = (float)vs.z;
		PVtx g[4] = {
			{ x - h, y - h, z, 0.5f, 0.0f, 1, 1, 1, a }, { x + h, y - h, z, 1.0f, 0.0f, 1, 1, 1, a },
			{ x + h, y + h, z, 1.0f, 0.5f, 1, 1, 1, a }, { x - h, y + h, z, 0.5f, 0.5f, 1, 1, 1, a } };
		v.insert (v.end(), { g[0], g[1], g[2], g[0], g[2], g[3] });
	}
	// beacons: blinking glows
	double simt = oapiGetSimTime ();
	for (DWORD i = 0; ; i++) {
		const BEACONLIGHTSPEC *bs = vessel->GetBeacon (i);
		if (!bs) break;
		if (!BeaconLit (bs, simt)) continue;
		VECTOR3 vp = scene->ViewPos (gpos + mul (grot, *bs->pos));
		// keep a minimum apparent size, so that beacons stay visible from afar
		float h = (float)std::max (bs->size, vp.z * 0.002);
		float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z;
		// beacons are much brighter than lit surfaces (they bloom in HDR)
		float r = (float)bs->col->x * BEACON_GLOW, g = (float)bs->col->y * BEACON_GLOW, b = (float)bs->col->z * BEACON_GLOW;
		PVtx q[4] = {
			{ x - h, y - h, z, 0.0f, 0.0f, r, g, b, 1 }, { x + h, y - h, z, 1.0f, 0.0f, r, g, b, 1 },
			{ x + h, y + h, z, 1.0f, 1.0f, r, g, b, 1 }, { x - h, y + h, z, 0.0f, 1.0f, r, g, b, 1 } };
		auto &v = batches[scene->GlowTex ()];
		v.insert (v.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
	}
	for (auto &b : batches) scene->DrawBillboards (b.first, true, b.second.data(), b.second.size());
}

// ======================================================================
// VPlanet: lit sphere (textured planets follow with the tile manager)

namespace {
	struct BodyColour { const char *name; float r, g, b; };
	const BodyColour bodyColours[] = {
		{ "Sun",     1.00f, 0.95f, 0.85f },
		{ "Mercury", 0.55f, 0.52f, 0.50f },
		{ "Venus",   0.90f, 0.82f, 0.62f },
		{ "Earth",   0.30f, 0.45f, 0.75f },
		{ "Moon",    0.55f, 0.55f, 0.53f },
		{ "Mars",    0.75f, 0.42f, 0.25f },
		{ "Jupiter", 0.80f, 0.70f, 0.55f },
		{ "Saturn",  0.85f, 0.78f, 0.58f },
		{ "Uranus",  0.62f, 0.82f, 0.88f },
		{ "Neptune", 0.35f, 0.50f, 0.85f },
	};
}

VPlanet::VPlanet (OBJHANDLE h, Scene *s, bool star)
	: VObject(h, s), isStar(star)
{
	char name[256];
	oapiGetObjectName (hObj, name, 256);
	colour[0] = colour[1] = colour[2] = 0.5f;
	colour[3] = 1.0f;
	auto iequal = [](const char *a, const char *b) {
		for (; *a && *b; a++, b++)
			if (tolower ((unsigned char)*a) != tolower ((unsigned char)*b)) return false;
		return *a == *b;
	};
	for (auto &bc : bodyColours)
		if (iequal (bc.name, name)) {
			colour[0] = bc.r, colour[1] = bc.g, colour[2] = bc.b;
			break;
		}
	if (!isStar) {
		atmosphere = std::make_unique<Atmosphere> (hObj, scene);
		surface = PlanetSurface::Create (hObj, scene);
		if (surface) clouds = PlanetSurface::Create (hObj, scene, true);
		atmosphere->SetGround (surface ? surface->RootTexture (0) : nullptr, surface ? surface->RootTexture (1) : nullptr, colour);
		if (clouds) {
			const double *alt = (const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_CLOUDALT);
			atmosphere->SetClouds (clouds->RootTexture (0), clouds->RootTexture (1), alt ? *alt : 0.0);
		}
		LoadLegacyTextures ();
	}
}

VPlanet::~VPlanet ()
{
	TextureManager *tm = scene->Client()->TexMgr();
	tm->Release (globalTex);
	tm->Release (ringTex);
	tm->Release (cloudTex);
	if (ringNIdx) {
		vkc::Context *c = scene->Client()->Ctx();
		vkc::Buffer vb = ringVtx, ib = ringIdx;
		c->DeferDestroy ([c, vb, ib]() mutable { c->DestroyBuffer (vb); c->DestroyBuffer (ib); });
	}
}

void VPlanet::LoadLegacyTextures ()
{
	char name[256], path[512];
	oapiGetObjectName (hObj, name, 256);
	// .tex files live in the Textures directory, or in the planet texture directory
	std::vector<std::string> dirs = { "Textures/" };
	if (scene->Client()->PlanetTexturePath (name, path)) {
		std::string d (path);
		std::replace (d.begin(), d.end(), '\\', '/');
		size_t sl = d.find_last_of ('/');
		if (sl != std::string::npos) dirs.push_back (d.substr (0, sl + 1));
	}
	auto load = [&](const std::string &file, std::vector<std::vector<uint8_t>> &images) {
		for (auto &d : dirs)
			if (legacytex::Read (d + file, images)) return true;
		return false;
	};
	TextureManager *tm = scene->Client()->TexMgr();
	std::vector<std::vector<uint8_t>> images;
	uint32_t w, h;
	std::vector<uint8_t> rgba;

	if (!surface && load (std::string (name) + ".tex", images) && legacytex::GlobalMap (images, w, h, rgba))
		globalTex = tm->CreateRGBA (w, h, rgba.data());

	const bool *hasClouds = (const bool*)oapiGetObjectParam (hObj, OBJPRM_PLANET_HASCLOUDS);
	if (!clouds && hasClouds && *hasClouds && load (std::string (name) + "_cloud.tex", images) &&
		legacytex::GlobalMap (images, w, h, rgba)) {
		cloudTex = tm->CreateRGBA (w, h, rgba.data());
		const double *alt = (const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_CLOUDALT);
		cloudAlt = alt ? *alt : 0.0;
	}

	const bool *hasRings = (const bool*)oapiGetObjectParam (hObj, OBJPRM_PLANET_HASRINGS);
	if (hasRings && *hasRings && load (std::string (name) + "_ring.tex", images) &&
		legacytex::Decode (images.back(), w, h, rgba)) {
		ringTex = tm->CreateRGBA (w, h, rgba.data());
		BuildRing (*(const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_RINGMINRAD),
		           *(const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_RINGMAXRAD));
	}
}

void VPlanet::BuildRing (double irad, double orad)
{
	// Flat annulus in the equatorial plane (units of the planet radius). The
	// ring texture is laid out for a ring of 16 straight sections: a radial
	// line runs through it from (fo,0) at the outer section corner to (fi,1)
	// at the inner radius. Every radial line of the annulus samples it that
	// way, which keeps the bands circular.
	const double alpha = PI / 16.0;
	double nrad = orad / cos (alpha);
	double fo = 0.5 * (1.0 - orad / nrad), fi = 0.5 * (1.0 - irad / nrad);
	double t0 = (nrad - orad) / (nrad - irad);
	const int N = 512;
	std::vector<NTVERTEX> vtx;
	std::vector<uint32_t> idx;
	for (int i = 0; i <= N; i++) {
		double phi = PI2 * i / N, c = cos (phi), s = sin (phi);
		NTVERTEX vo{}, vi{};
		vo.x = (float)(orad * c), vo.z = (float)(orad * s);
		vi.x = (float)(irad * c), vi.z = (float)(irad * s);
		vo.ny = vi.ny = 1.0f;
		vo.tu = (float)(fo + (fi - fo) * t0), vo.tv = (float)t0;
		vi.tu = (float)fi, vi.tv = 1.0f;
		vtx.push_back (vo), vtx.push_back (vi);
	}
	for (int i = 0; i < N; i++) {
		uint32_t a = 2 * i, b = a + 1, c = a + 2, d = a + 3;
		idx.insert (idx.end(), { a, b, c, b, d, c });  // seen from above
		idx.insert (idx.end(), { a, c, b, b, c, d });  // seen from below
	}
	vkc::Context *ctx = scene->Client()->Ctx();
	ringVtx = ctx->CreateDeviceBuffer (vtx.data(), vtx.size() * sizeof(NTVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	ringIdx = ctx->CreateDeviceBuffer (idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	ringNIdx = (uint32_t)idx.size();
}

void VPlanet::RenderRing ()
{
	if (!ringNIdx || !ringTex) return;
	Mat4 mv = scene->ModelView (gpos, grot) * Mat4::Scale (size, size, size);
	const float white[4] = { 1, 1, 1, 1 };
	scene->DrawGeometry (ringVtx.buffer, ringIdx.buffer, ringNIdx, mv, ringTex, white, false, true);
}

// ======================================================================
// VBase

namespace {
	constexpr double BASE_VIS_RANGE = 300e3;  // bases closer than this are rendered [m]
	// Ground structures lie on the terrain; they are drawn pulled towards the
	// camera by this fraction of their distance (a uniform scale in view space
	// keeps their screen position), which avoids depth fighting.
	constexpr double GROUND_DEPTH_PULL = 2e-4;
}

VBase::VBase (OBJHANDLE h, Scene *s) : hBase(h), scene(s)
{
	VulkanClient *gc = scene->Client();
	MESHHANDLE *bs = nullptr, *as = nullptr;
	DWORD nbs = 0, nas = 0;
	gc->GetBaseStructures (hBase, &bs, &nbs, &as, &nas);
	for (DWORD i = 0; i < nbs; i++)
		if (bs[i]) ground.push_back (std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), bs[i]));
	for (DWORD i = 0; i < nas; i++)
		if (as[i]) structures.push_back (std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), as[i]));
}

VBase::~VBase ()
{
}

void VBase::Render (RenderPass pass)
{
	VECTOR3 gpos;
	MATRIX3 grot;
	oapiGetGlobalPos (hBase, &gpos);
	oapiGetRotationMatrix (hBase, &grot);
	Mat4 mv = scene->ModelView (gpos, grot);
	scene->Sunlight (gpos);
	Mat4 pulled = Mat4::Scale (1.0 - GROUND_DEPTH_PULL, 1.0 - GROUND_DEPTH_PULL, 1.0 - GROUND_DEPTH_PULL) * mv;
	for (auto &m : ground) scene->DrawMesh (m.get(), pulled, pass);
	for (auto &m : structures) scene->DrawMesh (m.get(), mv, pass);
}

void VBase::RenderShadow (ShadowMap &sm)
{
	VECTOR3 gpos;
	MATRIX3 grot;
	oapiGetGlobalPos (hBase, &gpos);
	oapiGetRotationMatrix (hBase, &grot);
	Mat4 model = Mat4::FromRotTrans (grot, gpos - sm.Centre());
	for (auto &m : structures) sm.DrawMesh (m.get(), model);
}

// ======================================================================

void VPlanet::RenderShadow (ShadowMap &sm)
{
	for (auto &b : bases) b->RenderShadow (sm);
}

void VPlanet::UpdateBases ()
{
	DWORD n = oapiGetBaseCount (hObj);
	for (DWORD i = 0; i < n; i++) {
		OBJHANDLE hb = oapiGetBaseByIndex (hObj, i);
		VECTOR3 bp;
		oapiGetGlobalPos (hb, &bp);
		double d = length (bp - scene->CameraPos());
		auto it = std::find_if (bases.begin(), bases.end(), [hb](const auto &b) { return b->Handle() == hb; });
		bool have = it != bases.end();
		if (!have && d < BASE_VIS_RANGE)
			bases.push_back (std::make_unique<VBase> (hb, scene));
		else if (have && d > BASE_VIS_RANGE * 1.2)
			bases.erase (it);
	}
}

void VPlanet::Update ()
{
	VObject::Update ();
	if (!isStar) UpdateBases ();
	if (surface) surface->Update ();
	if (clouds) clouds->Update ();
}

void VPlanet::RenderSky ()
{
	if (!atmosphere) return;
	atmosphere->Update ();
	atmosphere->RenderSky ();
}

void VPlanet::Render (RenderPass pass)
{
	if (pass == RenderPass::Transparent) {
		if (isStar) { RenderGlow (); return; }
		for (auto &b : bases) b->Render (pass);
		if (clouds) clouds->Render (atmosphere.get());
		if (scene->PixelSize (size, camDist) >= 0.5) {
			if (cloudTex) {
				// legacy cloud layer: longitude 0 of the map at the -x axis
				double r = size + cloudAlt;
				MATRIX3 R = mul (grot, _M(-1, 0, 0, 0, 1, 0, 0, 0, -1));
				const float white[4] = { 1, 1, 1, 1 };
				scene->DrawGeometry (scene->SphereVtx(), scene->SphereIdx(), scene->SphereNIdx(),
					scene->ModelView (gpos, R) * Mat4::Scale (r, r, r), cloudTex, white, false, true);
			}
			RenderRing ();
		}
		return;
	}
	if (scene->PixelSize (size, camDist) < 0.5) return; // too small to see
	if (surface) {
		surface->Render (atmosphere.get());
		for (auto &b : bases) b->Render (pass);
		return;
	}

	// Until planetary surfaces are rendered from terrain tiles, a sphere stands
	// in for the planet. Close to the surface it is drawn with the local surface
	// radius below the camera, and with its pole (where the mesh vertices are
	// densest) pointing at the camera, so that the facets match the surface there.
	double radius = size;
	MATRIX3 R = grot;
	if (globalTex) {
		// whole-sphere map: longitude -180 (left edge) at the mesh's u = 0
		R = mul (grot, _M(-1, 0, 0, 0, 1, 0, 0, 0, -1));
		Mat4 mv = scene->ModelView (gpos, R) * Mat4::Scale (size, size, size);
		const float white[4] = { 1, 1, 1, 1 };
		scene->DrawSphere (mv, white, false, globalTex);
		return;
	}
	if (!isStar && camDist < 2.0 * size) {
		double lng, lat, rad;
		oapiGlobalToEqu (hObj, scene->CameraPos(), &lng, &lat, &rad);
		radius = size + oapiSurfaceElevation (hObj, lng, lat);
		VECTOR3 up = tmul (grot, scene->CameraPos() - gpos);
		up /= length (up);
		// rotation taking the mesh y axis to 'up'
		VECTOR3 ref = fabs (up.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0);
		VECTOR3 ex = crossp (ref, up);
		ex /= length (ex);
		VECTOR3 ez = crossp (ex, up);
		MATRIX3 Q = { ex.x, up.x, ez.x,
		              ex.y, up.y, ez.y,
		              ex.z, up.z, ez.z };
		R = mul (grot, Q);
	}
	Mat4 mv = scene->ModelView (gpos, R) * Mat4::Scale (radius, radius, radius);
	if (isStar) {
		// the sun's disc is far brighter than any lit surface
		float c[4] = { colour[0] * STAR_BRIGHTNESS, colour[1] * STAR_BRIGHTNESS, colour[2] * STAR_BRIGHTNESS, 1.0f };
		scene->DrawSphere (mv, c, true);
	} else {
		scene->DrawSphere (mv, colour, false);
	}
}

void VPlanet::RenderGlow ()
{
	// glow around a star, with a minimum apparent size so that it remains
	// visible when the disc itself is smaller than a pixel
	VECTOR3 vp = scene->ViewPos (gpos);
	if (vp.z <= 0.0) return;
	double h = std::max (size * STAR_GLOW_SIZE, vp.z * STAR_GLOW_MIN_ANGLE);
	// at its true distance, so that planets in front hide it (reversed float depth)
	float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z, hh = (float)h;
	float r = colour[0] * STAR_GLOW, g = colour[1] * STAR_GLOW, b = colour[2] * STAR_GLOW;
	PVtx q[4] = {
		{ x - hh, y - hh, z, 0.0f, 0.0f, r, g, b, 1 }, { x + hh, y - hh, z, 1.0f, 0.0f, r, g, b, 1 },
		{ x + hh, y + hh, z, 1.0f, 1.0f, r, g, b, 1 }, { x - hh, y + hh, z, 0.0f, 1.0f, r, g, b, 1 } };
	PVtx v[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
	scene->DrawBillboards (scene->GlowTex (), true, v, 6);
}
