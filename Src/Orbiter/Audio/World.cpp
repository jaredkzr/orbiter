// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "World.h"
#include "miniaudio.h"
#include "Camera.h"
#include "Vessel.h"
#include "Planet.h"
#include "TimeData.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern Camera *g_camera;
extern Vessel *g_focusobj;
extern TimeData td;

namespace audio {

namespace {
	/// Through the hull of the cockpit the listener is in: the air's sound
	/// (about -20 dB, muffled)
	constexpr float HULL_AIR_GAIN = 0.1f;
	constexpr float HULL_AIR_CUTOFF = 700.0f;
	/// Through the structure: a dull, solid sound
	constexpr float STRUCTURE_CUTOFF = 2500.0f;
	/// Cinematic: no travel time, a mild Doppler
	constexpr float CINEMATIC_DOPPLER_MIN = 0.8f, CINEMATIC_DOPPLER_MAX = 1.25f;
	/// Gains are held below this (a sanity limit: levels are physical, P_FULL
	/// full scale, the ears compress them; 1e4 is 190 dB SPL)
	constexpr float GAIN_MAX = 1e4f;
	/// Panning: a source to the side is this far to the side (about 12 dB
	/// between the speakers, as the head shadows the far ear; not a speaker
	/// alone)
	constexpr double PAN_WIDTH = 0.7;
	/// A source moving further than this in a step jumped: faster than
	/// JUMP_SPEED [m/s], and more than JUMP_MIN [m]
	constexpr double JUMP_SPEED = 30000.0, JUMP_MIN = 200.0;

	V3 ToV3 (const Vector &v) { return V3 (v.x, v.y, v.z); }
	V3 Mul (const double *R, const V3 &v)   // R v (row-major)
	{ return V3 (R[0]*v.x + R[1]*v.y + R[2]*v.z, R[3]*v.x + R[4]*v.y + R[5]*v.z, R[6]*v.x + R[7]*v.y + R[8]*v.z); }
	V3 TMul (const double *R, const V3 &v)  // R^T v
	{ return V3 (R[0]*v.x + R[3]*v.y + R[6]*v.z, R[1]*v.x + R[4]*v.y + R[7]*v.z, R[2]*v.x + R[5]*v.y + R[8]*v.z); }

	/// The vessel structure a body belongs to (docked or attached
	/// vessels are one)
	const void *Structure (const Body *b)
	{
		if (!b || b->Type () != OBJTP_VESSEL) return b;
		const Vessel *v = static_cast<const Vessel*>(b);
		if (const SuperVessel *s = v->SuperStruct ()) return s;
		const Vessel *root = v;
		while (const Vessel *p = root->AttachParent ()) root = p;
		return root;
	}

	// ---- sources ---------------------------------------------------------

	class SoundSource : public Source {
	public:
		~SoundSource () override
		{
			if (soundInit) ma_sound_uninit (&sound);
			if (waveInit) ma_waveform_uninit (&wave);
		}
		bool InitTone (Engine &e, double freq, double amp)
		{
			ma_waveform_config c = ma_waveform_config_init (ma_format_f32, 1, e.SampleRate (), ma_waveform_type_sine, amp, freq);
			if (ma_waveform_init (&c, &wave) != MA_SUCCESS) return false;
			waveInit = true;
			ma_sound_config sc = ma_sound_config_init_2 (e.Ma ());
			sc.pDataSource = &wave;
			sc.channelsOut = 1;
			sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
			return Init (e.Ma (), sc);
		}
		bool InitFile (Engine &e, const std::string &path, bool loop)
		{
			ma_sound_config sc = ma_sound_config_init_2 (e.Ma ());
			sc.pFilePath = path.c_str ();
			sc.channelsOut = 1;
			sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT | MA_SOUND_FLAG_DECODE;
			if (loop) sc.isLooping = MA_TRUE;
			return Init (e.Ma (), sc);
		}
		bool Attach (void *node) override
		{
			return soundInit && ma_node_attach_output_bus (&sound, 0, node, 0) == MA_SUCCESS;
		}
		void Start () override { if (soundInit) ma_sound_start (&sound); }
		void Stop () override { if (soundInit) ma_sound_stop (&sound); }
		void SetDoppler (float f) override { if (soundInit) ma_sound_set_pitch (&sound, std::max (f, 0.01f)); }
	private:
		bool Init (ma_engine *engine, ma_sound_config &sc)
		{
			if (ma_sound_init_ex (engine, &sc, &sound) != MA_SUCCESS) return false;
			soundInit = true;
			return true;
		}
		ma_waveform wave;
		ma_sound sound;
		bool waveInit = false, soundInit = false;
	};
}

std::unique_ptr<Source> MakeToneSource (Engine &e, double freq, double amp)
{
	if (!e.Active ()) return nullptr;
	auto s = std::make_unique<SoundSource>();
	if (!s->InitTone (e, freq, amp)) return nullptr;
	return s;
}

std::unique_ptr<Source> MakeFileSource (Engine &e, const std::string &path, bool loop)
{
	if (!e.Active ()) return nullptr;
	auto s = std::make_unique<SoundSource>();
	if (!s->InitFile (e, path, loop)) return nullptr;
	return s;
}

// ---- the world ------------------------------------------------------------

void Emitter::Push (double t, const Params &p)
{
	if (!history.empty () && t < history.back ().first) history.clear (); // (time went back)
	if (!history.empty () && t == history.back ().first) history.back ().second = p;
	else history.emplace_back (t, p);
	// (a minute is as far back as sound is heard: the track's age)
	while (history.size () > 2 && history[1].first < t - 61.0) history.pop_front ();
}

Emitter::Params Emitter::Recalled () const
{
	if (history.empty ()) return Params{};
	double tq = history.back ().first - hearDelay;
	// the newest entry not after tq (before the first: the first)
	for (size_t i = history.size (); i-- > 0; )
		if (history[i].first <= tq) return history[i].second;
	return history.front ().second;
}

World::World (Engine &e): engine (e)
{
}

World::~World ()
{
	Clear ();
}

Emitter *World::Add (const EmitterDesc &d, std::unique_ptr<Source> src)
{
	if (!engine.Active () || !src) return nullptr;
	auto e = std::make_unique<Emitter>();
	e->desc = d;
	if (!e->node.Init (engine.Ma ())) return nullptr;
	ma_node_attach_output_bus (e->node.Node (), PathNode::AIR, engine.Group (EXTERIOR), 0);
	ma_node_attach_output_bus (e->node.Node (), PathNode::HULL, engine.Group (INTERIOR), 0);
	if (!src->Attach (e->node.Node ())) return nullptr;
	e->source = std::move (src);
	e->source->Start ();
	emitters.push_back (std::move (e));
	return emitters.back ().get ();
}

void World::Remove (Emitter *e)
{
	for (auto it = emitters.begin (); it != emitters.end (); ++it) {
		if (it->get () != e) continue;
		// (the source first: it plays into the node)
		(*it)->source.reset ();
		(*it)->node.Uninit ();
		emitters.erase (it);
		return;
	}
}

void World::PlayShot (std::unique_ptr<Source> src, const V3 &dir, float gain, double length)
{
	if (!engine.Active () || !src) return;
	Shot sh;
	sh.node = std::make_unique<PathNode>();
	if (!sh.node->Init (engine.Ma (), 0.0f)) return; // (a boom's N-wave keeps its low end)
	ma_node_attach_output_bus (sh.node->Node (), PathNode::AIR, engine.Group (EXTERIOR), 0);
	if (!src->Attach (sh.node->Node ())) return;
	PathNode::Target t;
	double len = dir.Len ();
	t.gain = std::min (GAIN_MAX, gain * (inside ? HULL_AIR_GAIN : 1.0f));
	t.cutoff = inside ? HULL_AIR_CUTOFF : 20000.0f;
	t.pan = len > 0.0 ? (float)std::clamp (PAN_WIDTH * dir.x / len, -1.0, 1.0) : 0.0f;
	sh.node->Set (PathNode::AIR, t);
	src->Start ();
	sh.src = std::move (src);
	sh.end = td.SysT1 + length;
	shots.push_back (std::move (sh));
}

void World::UpdateShots ()
{
	for (auto it = shots.begin (); it != shots.end (); ) {
		if (td.SysT1 >= it->end) {
			it->src.reset ();
			it->node->Uninit ();
			it = shots.erase (it);
		} else ++it;
	}
}

void World::Forget (const Body *body)
{
	for (auto &e : emitters)
		if (e->desc.body == body) e->gone = true;
	emitters.erase (std::remove_if (emitters.begin (), emitters.end (), [](std::unique_ptr<Emitter> &e) {
		if (!e->gone) return false;
		e->source.reset ();
		e->node.Uninit ();
		return true;
	}), emitters.end ());
	if (insideOf == body) insideOf = nullptr;
}

void World::Clear ()
{
	for (auto &e : emitters) e->source.reset (), e->node.Uninit ();
	emitters.clear ();
	for (auto &sh : shots) sh.src.reset (), sh.node->Uninit ();
	shots.clear ();
	airBody = nullptr;
	tPrev = -1.0;
}

Air World::AirAt (const Planet *p, const double g[3])
{
	Air a;
	if (!p || !p->HasAtmosphere ()) return a;
	double lng, lat, rad;
	p->GlobalToEquatorial (Vector (g[0], g[1], g[2]), lng, lat, rad);
	ATMPARAM prm;
	if (!p->GetAtmParam (rad - p->Size (), lng, lat, &prm)) return a;
	const ATMCONST *ac = p->AtmParams ();
	return Air::From (prm.rho, prm.T, prm.p, ac->R, ac->gamma);
}

void World::Update ()
{
	if (!g_camera) return;
	const double t = td.SimT1;

	// the listener: the camera, in the frame of the planet it is at
	Body *target = g_camera->Target ();
	const Planet *pl = nullptr;
	if (target && target->Type () == OBJTP_VESSEL) pl = static_cast<Vessel*>(target)->ProxyPlanet ();
	else if (target && target->Type () == OBJTP_PLANET) pl = static_cast<const Planet*>(target);
	if (!pl && g_focusobj) pl = g_focusobj->ProxyPlanet ();
	if (pl != airBody || t < tPrev) {
		// a new frame (or time went back): the paths start over
		for (auto &e : emitters) e->track.Clear (), e->hasLast = false;
		tPrev = -1.0;
		airBody = pl;
	}
	const Vector &cg = g_camera->GPos ();
	double cgp[3] = { cg.x, cg.y, cg.z };
	memcpy (camRot, g_camera->GRot ().data, sizeof (camRot));
	if (airBody) {
		memcpy (airRot, airBody->GRot ().data, sizeof (airRot));
		L = ToV3 (airBody->GlobalToLocal (cg));
		lAir = AirAt (airBody, cgp);
	} else {
		L = ToV3 (cg);
		lAir = Air ();
	}
	listenerJump = false;
	if (tPrev >= 0.0 && t > tPrev) {
		V3 v = (L - Lprev) * (1.0 / (t - tPrev));
		// a camera cut: no motion through the air (no Doppler from it, no
		// boom from the change of what is heard)
		if ((L - Lprev).Len () > std::max (JUMP_MIN, JUMP_SPEED * (t - tPrev))) listenerJump = true, vL = V3 ();
		else vL = v;
	} else if (tPrev < 0.0) vL = V3 ();
	inside = g_camera->IsInternal () && target && target->Type () == OBJTP_VESSEL;
	insideOf = inside ? Structure (target) : nullptr;

	for (auto &e : emitters) UpdateEmitter (*e, t);
	UpdateShots ();

	Lprev = L;
	tPrev = t;
}

void World::UpdateEmitter (Emitter &e, double t)
{
	Emitter::State &s = e.state;
	const EmitterDesc &d = e.desc;
	if (!d.body) { e.node.Set (PathNode::AIR, {}), e.node.Set (PathNode::HULL, {}); return; }

	// where it is now, in the air's frame
	Vector off (d.offset.x, d.offset.y, d.offset.z);
	Vector g = d.body->GPos () + mul (d.body->GRot (), off);
	V3 P = airBody ? ToV3 (airBody->GlobalToLocal (g)) : ToV3 (g);
	// a jump (placed somewhere else: a scenario editor, a script): a new path
	if (e.hasLast && t > e.lastT) {
		double jump = (P - e.lastP).Len ();
		if (jump > std::max (JUMP_MIN, JUMP_SPEED * (t - e.lastT))) e.track.Restart ();
	}
	e.track.Add (t, P);
	e.lastP = P, e.lastT = t, e.hasLast = true;

	// ---- through the hull: the listener in its cockpit -----------------
	PathNode::Target hull;
	const bool own = inside && d.hull > 0.0f && Structure (d.body) == insideOf;
	if (own) {
		double dist = (P - L).Len ();
		hull.gain = std::min (GAIN_MAX, d.level * d.hull * (float)Spreading (dist, std::max<double> (d.refDist, 0.5 * d.body->Size ())));
		hull.cutoff = STRUCTURE_CUTOFF;
		hull.pan = 0.0f;
	}
	e.node.Set (PathNode::HULL, hull);
	s.hull = hull.gain;

	// ---- through the air ------------------------------------------------
	double gp[3] = { g.x, g.y, g.z };
	Air sAir = Effective (AirAt (airBody, gp), realism);
	Air la = Effective (lAir, realism);
	PathNode::Target air;
	const int prevImages = s.images;
	s.images = 0;
	if (airBody && sAir.Present () && la.Present ()) {
		// the speed of sound on the way: the mean of both ends
		double c = 0.5 * (sAir.c + la.c);
		Track::Image im[4];
		int n;
		if (realism == CINEMATIC) {
			// (heard where it is, at once)
			n = 1;
			im[0].tau = t, im[0].pos = P;
			Track::Image tmp[4];
			int m = e.track.Heard (t, L, c, tmp, 4);
			im[0].vel = m ? tmp[0].vel : V3 ();
		} else {
			n = e.track.Heard (t, L, c, im, 4);
		}
		// a pair appearing: the shock cone arrived
		if (n >= prevImages + 2 && !listenerJump && e.track.Size () > 2 && onBoom && realism == PHYSICAL && d.boom) {
			V3 dir = Mul (airRot, im[0].pos - L);
			double h = (im[0].pos - L).Len ();
			double mach = im[0].vel.Len () / std::max (sAir.c, 1.0);
			onBoom ({&e, t, TMul (camRot, dir), h, mach, sAir});
		}
		if (n > 0) {
			const Track::Image &h = im[0]; // (the newest: the nearer sound)
			V3 dl = h.pos - L;
			double dist = dl.Len ();
			s.images = n;
			s.delay = t - h.tau;
			s.dist = dist;
			// loudness: spreading, the air at both ends
			double gain = d.level * Spreading (dist, d.refDist) * Coupling (sAir) * Coupling (la);
			// the way it sounds loudest (when it left the source)
			if (d.front < 1.0f && dist > 1e-3) {
				Vector dg = mul (d.body->GRot (), Vector (d.dir.x, d.dir.y, d.dir.z));
				V3 dl3 = airBody ? TMul (airRot, ToV3 (dg)) : ToV3 (dg);
				double len = dl3.Len ();
				if (len > 0.0) {
					double cosT = -dl.Dot (dl3) / (dist * len); // (dl: from the listener to it)
					double f = 0.5 * (1.0 + cosT);
					gain *= d.front + (1.0 - d.front) * f * f;
				}
			}
			// absorption: over the way, in the mean air
			Air mean = la;
			mean.rho = std::sqrt (std::max (sAir.rho, 1e-12) * la.rho);
			double fc = AbsorptionCutoff (mean, dist);
			// Doppler
			float dop = 1.0f;
			if (dist > 1e-3) {
				V3 nv = (L - h.pos) * (1.0 / dist);
				double nn[3] = {nv.x, nv.y, nv.z}, vs[3] = {h.vel.x, h.vel.y, h.vel.z}, vl[3] = {vL.x, vL.y, vL.z};
				dop = (float)DopplerFactor (nn, vs, vl, c);
				if (realism == CINEMATIC) dop = std::clamp (dop, CINEMATIC_DOPPLER_MIN, CINEMATIC_DOPPLER_MAX);
			}
			// direction: from where it was heard, in the camera's frame (x right)
			float pan = 0.0f;
			if (dist > 1e-3) {
				V3 dc = TMul (camRot, Mul (airRot, dl));
				pan = (float)std::clamp (PAN_WIDTH * dc.x / dist, -1.0, 1.0);
			}
			// the cockpit's hull between
			if (inside) {
				gain *= HULL_AIR_GAIN;
				fc = std::min<double> (fc, HULL_AIR_CUTOFF);
			}
			// (the own vessel's sound reaches the cockpit through the structure:
			// through the air, too, but not panned around the head)
			if (own) pan = 0.0f, dop = 1.0f; // (and not shifted: the structure carries it)
			air.gain = (float)std::min<double> (GAIN_MAX, gain);
			air.cutoff = (float)fc;
			air.pan = pan;
			s.doppler = dop;
			s.cutoff = air.cutoff;
			e.source->SetDoppler (dop);
		}
	}
	if (!s.images) {
		s.dist = (P - L).Len (), s.delay = 0.0; // (not heard: where it is now)
		if (own) e.source->SetDoppler (1.0f);
	}
	e.node.Set (PathNode::AIR, air);
	s.air = air.gain;
	// the source as the sound now arriving left it (the structure carries
	// its own vessel's sound at once)
	e.hearDelay = (own || !s.images) ? 0.0 : s.delay;
}

} // namespace audio
