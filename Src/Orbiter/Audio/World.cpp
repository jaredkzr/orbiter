// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "World.h"
#include "DebugEnv.h"
#include "Assets.h"
#include "miniaudio.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>

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

	V3 Mul (const double *R, const V3 &v)   // R v (row-major)
	{ return V3 (R[0]*v.x + R[1]*v.y + R[2]*v.z, R[3]*v.x + R[4]*v.y + R[5]*v.z, R[6]*v.x + R[7]*v.y + R[8]*v.z); }
	V3 TMul (const double *R, const V3 &v)  // R^T v
	{ return V3 (R[0]*v.x + R[3]*v.y + R[6]*v.z, R[1]*v.x + R[4]*v.y + R[7]*v.z, R[2]*v.x + R[5]*v.y + R[8]*v.z); }

	// ---- sources ---------------------------------------------------------

	class SoundSource : public Source {
	public:
		~SoundSource () override { Detach (); }
		void Detach () override
		{
			if (soundInit) ma_sound_uninit (&sound), soundInit = false;
			if (waveInit) ma_waveform_uninit (&wave), waveInit = false;
			clip.Uninit ();
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
			if (ma_sound_init_ex (e.Ma (), &sc, &sound) != MA_SUCCESS) return false;
			soundInit = true;
			return true;
		}
		bool InitClip (Engine &e, ClipRef c, bool loop)
		{
			return clip.Init (e.Ma (), std::move (c), nullptr, 1, loop);
		}
		bool Attach (void *node) override
		{
			ma_sound *s = S ();
			return s && ma_node_attach_output_bus (s, 0, node, 0) == MA_SUCCESS;
		}
		void Start () override { if (ma_sound *s = S ()) ma_sound_start (s); }
		void Stop () override { if (ma_sound *s = S ()) ma_sound_stop (s); }
		void SetDoppler (float f) override { dop = f; if (ma_sound *s = S ()) ma_sound_set_pitch (s, std::max (dop * pitch, 0.01f)); }
		void SetPitch (float p) override { pitch = p; if (ma_sound *s = S ()) ma_sound_set_pitch (s, std::max (dop * pitch, 0.01f)); }
		bool Done () const override { const ma_sound *s = S (); return s && ma_sound_at_end (s); }
		bool Seek (double t) override { ma_sound *s = S (); return s && ma_sound_seek_to_second (s, (float)std::max (0.0, t)) == MA_SUCCESS; }
		double Cursor () const override
		{
			float c = 0.0f;
			const ma_sound *s = S ();
			return s && ma_sound_get_cursor_in_seconds (const_cast<ma_sound*> (s), &c) == MA_SUCCESS ? c : -1.0;
		}
	private:
		ma_sound *S () { return soundInit ? &sound : clip ? clip.Sound () : nullptr; }
		const ma_sound *S () const { return soundInit ? &sound : clip ? clip.Sound () : nullptr; }
		ma_waveform wave;
		ma_sound sound;               // (the tone's)
		ClipSound clip;               // (a file's)
		bool waveInit = false, soundInit = false;
		float dop = 1.0f, pitch = 1.0f;
	};
}

SourcePtr<Source> MakeToneSource (Engine &e, double freq, double amp)
{
	if (!e.Active ()) return nullptr;
	SourcePtr<SoundSource> s (new SoundSource ());
	if (!s->InitTone (e, freq, amp)) return nullptr;
	return s;
}

SourcePtr<Source> MakeClipSource (Engine &e, ClipRef clip, bool loop)
{
	if (!e.Active () || !clip) return nullptr;
	SourcePtr<SoundSource> s (new SoundSource ());
	if (!s->InitClip (e, std::move (clip), loop)) return nullptr;
	return s;
}

SourcePtr<Source> MakeFileSource (Engine &e, const std::string &path, bool loop)
{
	if (!e.Active ()) return nullptr;
	return MakeClipSource (e, Assets::Get ().Load (path, e.SampleRate ()), loop);
}

// ---- the world ------------------------------------------------------------

void Emitter::PushBytes (double t, const void *p, size_t n)
{
	if (n != histSize) ClearHistory (), histSize = n;              // (another kind of parameters)
	size_t count = histTimes.size () - histFirst;
	if (count && t < histNow) ClearHistory (), count = 0;           // (time went back)
	histNow = t;
	unsigned char *last = count ? histBytes.data () + (histTimes.size () - 1) * n : nullptr;
	if (count && t == histTimes.back ()) memcpy (last, p, n);
	else if (count && !memcmp (last, p, n)) {}                      // (as it was: kept once)
	else {
		histTimes.push_back (t);
		histBytes.insert (histBytes.end (), (const unsigned char*)p, (const unsigned char*)p + n);
		count++;
	}
	// (a minute is as far back as sound is heard: the track's age)
	while (count > 2 && histTimes[histFirst + 1] < t - 61.0) histFirst++, count--;
	if (histFirst > 256 && histFirst > count) {
		histTimes.erase (histTimes.begin (), histTimes.begin () + histFirst);
		histBytes.erase (histBytes.begin (), histBytes.begin () + histFirst * n);
		histFirst = 0;
	}
}

const unsigned char *Emitter::RecalledBytes (size_t n) const
{
	if (histTimes.size () == histFirst || n != histSize) return nullptr;
	// the newest moment not after the one now heard (before the first: the first)
	const double tq = histNow - hearDelay;
	auto it = std::upper_bound (histTimes.begin () + histFirst, histTimes.end (), tq);
	size_t i = it == histTimes.begin () + histFirst ? histFirst : (size_t)(it - histTimes.begin ()) - 1;
	return histBytes.data () + i * n;
}

World::World (Engine &e): engine (e)
{
}

World::~World ()
{
	Clear ();
}

EmitterId World::Add (const EmitterDesc &d, SourcePtr<Source> src)
{
	if (!engine.Active () || !src) return EmitterId ();
	auto e = std::make_unique<Emitter>();
	e->desc = d;
	if (!e->node.Init (engine.Ma ())) return EmitterId ();
	ma_node_attach_output_bus (e->node.Node (), PathNode::AIR, engine.Group (EXTERIOR), 0);
	ma_node_attach_output_bus (e->node.Node (), PathNode::HULL, engine.Group (INTERIOR), 0);
	if (!src->Attach (e->node.Node ())) return EmitterId ();
	e->source = std::move (src);
	e->created = tPrev;   // (< 0: the next update's time)
	e->begun = !d.delayed;
	if (e->begun) e->source->Start ();
	// (a slot: a free one, else a new one; a generation never used before)
	uint32_t slot;
	if (!freeSlots.empty ()) slot = freeSlots.back (), freeSlots.pop_back ();
	else slot = (uint32_t)slots.size (), slots.push_back ({0, nullptr});
	if (++generation == 0) ++generation;
	e->id = {slot, generation};
	slots[slot] = {generation, e.get ()};
	emitters.push_back (std::move (e));
	return emitters.back ()->id;
}

Emitter *World::Get (EmitterId id) const
{
	if (!id || id.slot >= slots.size () || slots[id.slot].first != id.gen) return nullptr;
	return slots[id.slot].second;
}

void World::Erase (std::vector<std::unique_ptr<Emitter>>::iterator it)
{
	Emitter &e = **it;
	// (the source first: it plays into the node)
	e.source.reset ();
	e.node.Uninit ();
	slots[e.id.slot] = {0, nullptr};
	freeSlots.push_back (e.id.slot);
	emitters.erase (it);
}

void World::Remove (EmitterId id)
{
	Emitter *e = Get (id);
	if (!e) return;
	for (auto it = emitters.begin (); it != emitters.end (); ++it)
		if (it->get () == e) { Erase (it); return; }
}

BodyRef World::Ref (const Body *b)
{
	if (!b) return BodyRef ();
	auto it = serials.find (b);
	if (it == serials.end ()) it = serials.emplace (b, ++nextSerial).first;
	return {b, it->second};
}

BodyRef World::Known (const Body *b) const
{
	auto it = b ? serials.find (b) : serials.end ();
	return it == serials.end () ? BodyRef () : BodyRef {b, it->second};
}

bool World::Alive (const BodyRef &r) const
{
	if (!r) return false;
	auto it = serials.find (r.body);
	return it != serials.end () && it->second == r.serial;
}

void World::PlayShot (SourcePtr<Source> src, const V3 &dir, float gain, double length)
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
	if (len > 0.0) t.dir[0] = (float)(dir.x / len), t.dir[1] = (float)(dir.y / len), t.dir[2] = (float)(dir.z / len);
	sh.node->Set (PathNode::AIR, t);
	src->Start ();
	sh.src = std::move (src);
	sh.end = now + length;
	shots.push_back (std::move (sh));
}

void World::UpdateShots ()
{
	for (auto it = shots.begin (); it != shots.end (); ) {
		if (now >= it->end) {
			it->src.reset ();
			it->node->Uninit ();
			it = shots.erase (it);
		} else ++it;
	}
}

void World::Forget (const Body *body)
{
	for (size_t i = emitters.size (); i-- > 0; )
		if (emitters[i]->desc.body == body) Erase (emitters.begin () + i);
	if (insideOf == body) insideOf = nullptr;
	serials.erase (body);   // (a new body at its address: a new identity)
}

void World::Clear ()
{
	while (!emitters.empty ()) Erase (emitters.end () - 1);
	for (auto &sh : shots) sh.src.reset (), sh.node->Uninit ();
	shots.clear ();
	airBody = nullptr;
	tPrev = -1.0;
	serials.clear ();   // (the session's bodies: all gone)
}

void World::ResetPaths ()
{
	for (auto &e : emitters) e->track.Clear (), e->hasLast = false, e->ClearHistory (), e->state = Emitter::State ();
	tPrev = -1.0;
}

void World::Cull ()
{
	std::vector<std::pair<float, Emitter*>> c;   // (now: the clock of what is heard)
	for (auto &e : emitters)
		if (e->source && e->source->Continuous ()) c.push_back ({ std::max (e->state.air, e->state.hull) * e->source->Activity (), e.get () });
	std::sort (c.begin (), c.end (), [] (const auto &a, const auto &b) { return a.first > b.first; });
	voices = culled = 0;
	const int cap = Dbg ().voices > 0 ? Dbg ().voices : MAX_VOICES; // (tests: ORBITER_AUDIO_VOICES)
	for (auto &x : c) {
		Emitter &e = *x.second;
		if (x.first > CULL_LEVEL) e.silentSince = -1.0;
		else if (e.silentSince < 0.0) e.silentSince = now;
		const bool heard = e.silentSince < 0.0 || now - e.silentSince < CULL_AFTER;
		if (heard && voices < cap) {
			voices++;
			if (e.culled) e.node.SetRunning (true), e.source->Start (), e.culled = false;
		} else {
			culled++;
			if (!e.culled) e.source->Stop (), e.node.SetRunning (false), e.culled = true; // (its ways too: nothing to filter)
		}
	}
}

void World::Update (const Snapshot &snap, const Scene &sc)
{
	if (!snap.listener) return;
	const double t = snap.t;
	scene = &sc;
	now = snap.playT;
	ramp = snap.dt;

	// the listener: the camera, in the frame of the planet it is at
	if (snap.frame != airBody || t < tPrev) {
		// a new frame (or time went back): the paths start over
		for (auto &e : emitters) e->track.Clear (), e->hasLast = false;
		tPrev = -1.0;
		airBody = snap.frame;
	}
	memcpy (camRot, snap.ear.rot, sizeof (camRot));
	if (airBody) {
		memcpy (airRot, snap.framePose.rot, sizeof (airRot));
		framePos = snap.framePose.pos;
		L = TMul (airRot, snap.ear.pos - snap.framePose.pos);
		lAir = snap.air;
	} else {
		L = snap.ear.pos;
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
	inside = snap.inside;
	insideOf = inside ? snap.insideOf : nullptr;

	for (auto &e : emitters) UpdateEmitter (*e, t);
	UpdateShots ();
	Cull ();

	Lprev = L;
	tPrev = t;
	scene = nullptr;
}

void World::UpdateEmitter (Emitter &e, double t)
{
	Emitter::State &s = e.state;
	const EmitterDesc &d = e.desc;
	Pose bp;
	if (!d.body || !scene->PoseOf (d.body, bp)) { e.node.Set (PathNode::AIR, {}, ramp), e.node.Set (PathNode::HULL, {}, ramp); return; }

	// where it is now, in the air's frame
	const V3 g = bp.pos + Mul (bp.rot, d.offset);
	V3 P = airBody ? TMul (airRot, g - frameOrigin ()) : g;
	// a jump (placed somewhere else: a scenario editor, a script): a new path
	if (e.hasLast && t > e.lastT) {
		double jump = (P - e.lastP).Len ();
		if (jump > std::max (JUMP_MIN, JUMP_SPEED * (t - e.lastT))) e.track.Restart ();
	}
	e.track.Add (t, P);
	e.lastP = P, e.lastT = t, e.hasLast = true;

	// ---- through the hull: the listener in its cockpit -----------------
	PathNode::Target hull;
	const bool own = inside && d.hull > 0.0f && scene->StructureOf (d.body) == insideOf;
	if (own) {
		double dist = (P - L).Len ();
		hull.gain = std::min (GAIN_MAX, d.level * d.hull * (float)Spreading (dist, std::max<double> (d.refDist, 0.5 * scene->SizeOf (d.body))));
		hull.cutoff = STRUCTURE_CUTOFF;
		hull.pan = 0.0f;
	}
	e.node.Set (PathNode::HULL, hull, ramp);
	s.hull = hull.gain;

	// ---- through the air ------------------------------------------------
	Air sAir = Effective (scene->AirAt (airBody, g), realism);
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
				const V3 dg = Mul (bp.rot, d.dir);
				V3 dl3 = airBody ? TMul (airRot, dg) : dg;
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
			V3 dc;
			if (dist > 1e-3) {
				dc = TMul (camRot, Mul (airRot, dl)) * (1.0 / dist);
				pan = (float)std::clamp (PAN_WIDTH * dc.x, -1.0, 1.0);
			}
			// the cockpit's hull between
			if (inside) {
				gain *= HULL_AIR_GAIN;
				fc = std::min<double> (fc, HULL_AIR_CUTOFF);
			}
			// (the own vessel's sound reaches the cockpit through the structure:
			// through the air, too, but not panned around the head)
			if (own) pan = 0.0f, dop = 1.0f, dc = V3 (); // (and not shifted: the structure carries it)
			air.gain = (float)std::min<double> (GAIN_MAX, gain);
			air.cutoff = (float)fc;
			air.pan = pan;
			air.dir[0] = (float)dc.x, air.dir[1] = (float)dc.y, air.dir[2] = (float)dc.z; // (binaural: its direction)
			s.doppler = dop;
			s.cutoff = air.cutoff;
			e.source->SetDoppler (dop);
		}
	}
	if (!s.images) {
		s.dist = (P - L).Len (), s.delay = 0.0; // (not heard: where it is now)
		if (own) e.source->SetDoppler (1.0f);
	}
	e.node.Set (PathNode::AIR, air, ramp);
	s.air = air.gain;
	// the source as the sound now arriving left it (the structure carries
	// its own vessel's sound at once)
	e.hearDelay = (own || !s.images) ? 0.0 : s.delay;
	// a recording begun when the sound of its start arrives (through the air: its travel time; the
	// structure, a cinematic world: at once; time gone back - a replay: at once)
	if (!e.begun && !e.held) {
		if (e.created < 0.0) e.created = t;
		// (no way through the air at all - vacuum at either end: begun now, unheard, so it ends)
		const bool airPath = airBody && sAir.Present () && la.Present ();
		if (own || !airPath || (s.images > 0 && t - s.delay >= e.created) || t < e.created) e.begun = true, e.source->Start ();
	}
}

} // namespace audio
