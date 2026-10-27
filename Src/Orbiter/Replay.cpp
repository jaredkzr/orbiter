// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Replay.h"
#include "Debris.h"
#include "UiAnchor.h"
#include "Orbiter.h"
#include "Psys.h"
#include "Vessel.h"
#include "Celbody.h"
#include "Camera.h"
#include "Log.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>

extern Orbiter *g_pOrbiter;
extern PlanetarySystem *g_psys;
extern Camera *g_camera;
extern TimeData td;

namespace {
	std::string RelTimeStr (double dt)
	{
		char buf[64];
		const char *sgn = dt < -0.05 ? "-" : " ";
		dt = fabs (dt);
		int m = (int)(dt / 60.0);
		snprintf (buf, sizeof(buf), "%s%d:%04.1f", sgn, m, dt - 60.0 * m);
		return buf;
	}

	// Catmull-Rom interpolation of p1..p2 (u: 0..1), with neighbours p0 and p3
	double CatmullRom (double p0, double p1, double p2, double p3, double u)
	{
		double u2 = u*u, u3 = u2*u;
		return 0.5 * ((2.0*p1) + (-p0 + p2)*u + (2.0*p0 - 5.0*p1 + 4.0*p2 - p3)*u2 + (-p0 + 3.0*p1 - 3.0*p2 + p3)*u3);
	}
}

Replay::Replay (size_t maxFrames): ImGuiDialog ("Replay"), maxFrames(maxFrames)
{
	active = true; // (the dialog draws only while the replay is active)
}

Replay::~Replay ()
{
	if (active_) Exit ();
}

// =======================================================================
// Recording

void Replay::Record ()
{
	if (active_) return;
	double simt = td.SimT1;
	if (!frames.empty() && simt <= frames.back().simt) return;
	Frame f;
	f.simt = simt;
	f.v.reserve (g_psys->nVessel());
	for (DWORD i = 0; i < g_psys->nVessel(); i++) {
		Vessel *v = g_psys->GetVessel (i);
		VesselRec r;
		r.v = v;
		r.pos = v->GPos(), r.vel = v->GVel();
		r.omega = v->s0->omega;
		r.q = v->s0->Q;
		r.th.reserve (v->m_thruster.size());
		for (const ThrustSpec *ts : v->m_thruster) r.th.push_back ((float)ts->level);
		r.anim.reserve (v->nanim);
		for (UINT k = 0; k < v->nanim; k++) r.anim.push_back ((float)v->anim[k].state);
		f.v.push_back (std::move (r));
	}
	for (size_t i = 0; i < g_psys->nDebris(); i++) {
		Debris *d = g_psys->GetDebris (i);
		if (!d->AtRest()) f.d.push_back ({ d, d->GPos(), d->GVel(), d->s0->Q });
	}
	frames.push_back (std::move (f));
	while (frames.size() > maxFrames) frames.pop_front();
}

void Replay::Forget (const Debris *d)
{
	for (auto &f : frames)
		f.d.erase (std::remove_if (f.d.begin(), f.d.end(), [d](const DebrisRec &r) { return r.d == d; }), f.d.end());
	liveDebris.erase (std::remove_if (liveDebris.begin(), liveDebris.end(), [d](const LiveDebris &l) { return l.d == d; }), liveDebris.end());
	keys.erase (std::remove_if (keys.begin(), keys.end(), [d](const CamKey &k) { return k.target == (const Body*)d; }), keys.end());
}

void Replay::Forget (const Vessel *v)
{
	for (auto &f : frames)
		f.v.erase (std::remove_if (f.v.begin(), f.v.end(), [v](const VesselRec &r) { return r.v == v; }), f.v.end());
	live.erase (std::remove_if (live.begin(), live.end(), [v](const Live &l) { return l.v == v; }), live.end());
	keys.erase (std::remove_if (keys.begin(), keys.end(), [v](const CamKey &k) { return k.target == (const Body*)v; }), keys.end());
}

// =======================================================================
// Replay mode

bool Replay::Enter ()
{
	if (active_ || frames.size() < 2) return false;
	liveMJD = td.MJD0;
	liveRunning = g_pOrbiter->IsRunning();
	if (liveRunning) g_pOrbiter->Pause (true);
	live.clear();
	for (DWORD i = 0; i < g_psys->nVessel(); i++) {
		Vessel *v = g_psys->GetVessel (i);
		Live l;
		l.v = v;
		l.s.Set (*v->s0);
		l.cpos = v->cpos, l.cvel = v->cvel;
		for (const ThrustSpec *ts : v->m_thruster) l.th.push_back (ts->level), l.thperm.push_back (ts->level_permanent);
		for (UINT k = 0; k < v->nanim; k++) l.anim.push_back (v->anim[k].state);
		live.push_back (std::move (l));
	}
	liveDebris.clear();
	for (size_t i = 0; i < g_psys->nDebris(); i++) {
		Debris *d = g_psys->GetDebris (i);
		LiveDebris l;
		l.d = d;
		l.s.Set (*d->s0);
		l.cpos = d->cpos, l.cvel = d->cvel;
		liveDebris.push_back (l);
	}
	active_ = true;
	playing = true;
	t = std::max (Start(), End() - 30.0);
	tIn = Start(), tOut = End();
	status.clear();
	LOGOUT ("Replay: %.1f s recorded (%zu frames)", End() - Start(), frames.size());
	return true;
}

void Replay::Exit ()
{
	if (!active_) return;
	if (exporting) FinishExport ();
	active_ = false;
	td.JumpTo (liveMJD);
	g_psys->UpdateCelestials ();
	for (const Live &l : live) {
		Vessel *v = l.v;
		v->s0->Set (l.s);
		v->cpos = l.cpos, v->cvel = l.cvel;
		for (size_t k = 0; k < l.th.size() && k < v->m_thruster.size(); k++)
			v->m_thruster[k]->level = l.th[k], v->m_thruster[k]->level_permanent = l.thperm[k];
		for (size_t k = 0; k < l.anim.size() && k < v->nanim; k++) v->anim[k].state = l.anim[k];
	}
	live.clear();
	for (const LiveDebris &l : liveDebris) {
		l.d->s0->Set (l.s);
		l.d->cpos = l.cpos, l.d->cvel = l.cvel;
		l.d->SetHidden (false);
	}
	liveDebris.clear();
	if (liveRunning) g_pOrbiter->Pause (false);
}

void Replay::Seek (double simt)
{
	t = std::clamp (simt, Start(), End());
}

void Replay::SetCelestials ()
{
	g_psys->UpdateCelestials ();
}

void Replay::Show (double simt)
{
	if (frames.size() < 2) return;
	simt = std::clamp (simt, Start(), End());
	// bracketing frames
	auto it = std::upper_bound (frames.begin(), frames.end(), simt, [](double s, const Frame &f) { return s < f.simt; });
	if (it == frames.end()) --it;
	if (it == frames.begin()) ++it;
	const Frame &f1 = *it, &f0 = *(it - 1);
	double dt = f1.simt - f0.simt;
	double u = dt > 0.0 ? std::clamp ((simt - f0.simt) / dt, 0.0, 1.0) : 1.0;

	td.JumpTo (td.MJD_ref + simt / 86400.0);
	SetCelestials ();

	double u2 = u*u, u3 = u2*u;
	double h00 = 2*u3 - 3*u2 + 1, h10 = u3 - 2*u2 + u, h01 = -2*u3 + 3*u2, h11 = u3 - u2;
	double d00 = (6*u2 - 6*u) / dt, d10 = 3*u2 - 4*u + 1, d01 = (-6*u2 + 6*u) / dt, d11 = 3*u2 - 2*u;
	for (size_t i = 0; i < f0.v.size(); i++) {
		const VesselRec &a = f0.v[i];
		const VesselRec *b = (i < f1.v.size() && f1.v[i].v == a.v) ? &f1.v[i] : nullptr;
		if (!b) for (const auto &r : f1.v) if (r.v == a.v) { b = &r; break; }
		if (!b) continue;
		Vessel *v = a.v;
		StateVectors *s = v->s0;
		if (dt > 0.0) {
			s->pos = a.pos * h00 + a.vel * (h10 * dt) + b->pos * h01 + b->vel * (h11 * dt);
			s->vel = a.pos * d00 + a.vel * d10 + b->pos * d01 + b->vel * d11;
		} else s->pos = b->pos, s->vel = b->vel;
		Quaternion q;
		q.interp (a.q, b->q, u);
		s->SetRot (q);
		s->omega = a.omega + (b->omega - a.omega) * u;
		if (v->cbody) v->cpos = s->pos - v->cbody->GPos(), v->cvel = s->vel - v->cbody->GVel();
		for (size_t k = 0; k < a.th.size() && k < b->th.size() && k < v->m_thruster.size(); k++)
			v->m_thruster[k]->level = a.th[k] + (b->th[k] - a.th[k]) * u;
		for (size_t k = 0; k < a.anim.size() && k < b->anim.size() && k < v->nanim; k++)
			v->anim[k].state = a.anim[k] + (b->anim[k] - a.anim[k]) * u;
	}

	// debris: not there before it broke off; in motion as recorded; at
	// rest where it is now
	for (const LiveDebris &l : liveDebris) {
		Debris *d = l.d;
		d->SetHidden (simt < d->CreatedT());
		if (d->Hidden()) continue;
		const DebrisRec *a = nullptr, *b = nullptr;
		for (const auto &r : f0.d) if (r.d == d) { a = &r; break; }
		for (const auto &r : f1.d) if (r.d == d) { b = &r; break; }
		StateVectors *s = d->s0;
		if (a && b && dt > 0.0) {
			s->pos = a->pos * h00 + a->vel * (h10 * dt) + b->pos * h01 + b->vel * (h11 * dt);
			s->vel = a->pos * d00 + a->vel * d10 + b->pos * d01 + b->vel * d11;
			Quaternion q;
			q.interp (a->q, b->q, u);
			s->SetRot (q);
		} else if (a || b) {
			const DebrisRec *r = a ? a : b;
			s->pos = r->pos, s->vel = r->vel;
			s->SetRot (r->q);
		} else {
			s->Set (l.s); // (at rest: as now)
		}
		if (d->cbody) d->cpos = s->pos - d->cbody->GPos(), d->cvel = s->vel - d->cbody->GVel();
	}
}

void Replay::Update ()
{
	if (!active_) return;
	double tEnd = exporting ? std::min (End(), tOut) : End();
	if (exporting) {
		if (t > tEnd) { // (the last frame was rendered in the previous frame)
			FinishExport ();
			return;
		}
		// one frame of the video per rendered frame
		Show (t);
		CameraKeys ();
		char path[512];
		snprintf (path, sizeof(path), "%s/frame_%05d", exportDir.c_str(), exportFrame++);
		if (oapi::GraphicsClient *gc = g_pOrbiter->GetGraphicsClient())
			gc->clbkSaveSurfaceToImage (0, path, oapi::IMAGE_PNG);
		t += speed / exportFps;
		return;
	}
	if (playing) {
		t += speed * td.SysDT;
		if (t >= tEnd) {
			if (loop) t = std::max (Start(), tIn);
			else t = tEnd, playing = false;
		}
	}
	t = std::clamp (t, Start(), End());
	Show (t);
	if (cinematic) CameraKeys ();
}

// =======================================================================
// Cinematic camera: keyframes of the camera's target, mode, distance,
// direction and field of view, interpolated in replay time

void Replay::AddKey ()
{
	Body *tgt = g_camera->Target();
	if (!tgt) return;
	CamKey k{t, tgt, g_camera->IsExternal(), g_camera->GetExtMode(),
		g_camera->Distance() / std::max (1.0, tgt->Size()), g_camera->Phi(), g_camera->Theta(), g_camera->Aperture()};
	for (auto &q : keys)
		if (fabs (q.t - t) < 0.05) { q = k; return; }
	keys.insert (std::upper_bound (keys.begin(), keys.end(), k, [](const CamKey &a, const CamKey &b) { return a.t < b.t; }), k);
}

void Replay::CameraKeys ()
{
	if (keys.empty()) return;
	size_t i = 0;
	while (i + 1 < keys.size() && keys[i+1].t <= t) i++;
	const CamKey &k1 = keys[i];
	CamKey c = k1;
	if (t > k1.t && i + 1 < keys.size()) {
		const CamKey &k2 = keys[i+1];
		if (k2.target == k1.target && k2.external == k1.external && k2.mode == k1.mode) {
			const CamKey &k0 = (i > 0 && keys[i-1].target == k1.target) ? keys[i-1] : k1;
			const CamKey &k3 = (i + 2 < keys.size() && keys[i+2].target == k1.target) ? keys[i+2] : k2;
			double u = (t - k1.t) / (k2.t - k1.t);
			// (angles unwrapped about k1)
			auto un = [](double a, double ref) { while (a - ref > Pi) a -= Pi2; while (a - ref < -Pi) a += Pi2; return a; };
			c.dist = exp (CatmullRom (log (k0.dist), log (k1.dist), log (k2.dist), log (k3.dist), u));
			c.phi = CatmullRom (un (k0.phi, k1.phi), k1.phi, un (k2.phi, k1.phi), un (k3.phi, k1.phi), u);
			c.theta = CatmullRom (k0.theta, k1.theta, k2.theta, k3.theta, u);
			c.ap = CatmullRom (k0.ap, k1.ap, k2.ap, k3.ap, u);
		}
	}
	if (g_camera->Target() != c.target || g_camera->IsExternal() != c.external)
		g_pOrbiter->SetView (c.target, c.external ? 1 : 0);
	if (c.external && c.mode != CAMERA_GROUNDOBSERVER) { // (ground observer: its own position; only the field of view follows)
		if (g_camera->GetExtMode() != c.mode) g_camera->SetTrackMode (c.mode);
		g_camera->SetDistance (c.dist);
		g_camera->AddPhi (2.0 * (c.phi - g_camera->Phi()));   // (adds half the angle)
		g_camera->AddTheta (2.0 * (c.theta - g_camera->Theta()));
	}
	g_camera->SetAperture (c.ap);
}

// =======================================================================
// Export: one image per frame at the export frame rate, then a video

void Replay::Export (double simt0, double simt1, int fps)
{
	if (!active_) return;
	tIn = simt0, tOut = simt1;
	exportFps = fps;
	StartExport ();
}

void Replay::StartExport ()
{
	char name[64];
	time_t now = time (nullptr);
	strftime (name, sizeof(name), "Replay-%Y%m%d-%H%M%S", localtime (&now));
	exportDir = std::string ("Images/") + name;
	std::error_code ec;
	std::filesystem::create_directories (exportDir, ec);
	if (ec) {
		status = "Could not create " + exportDir;
		return;
	}
	t = std::max (Start(), tIn);
	exportFrame = 0;
	exporting = true;
	status.clear();
}

void Replay::FinishExport ()
{
	exporting = false;
	playing = false;
	// the frames are written in the background: wait for the last one
	if (exportFrame > 0) {
		char last[512];
		snprintf (last, sizeof(last), "%s/frame_%05d.png", exportDir.c_str(), exportFrame - 1);
		for (int i = 0; i < 3000 && !std::filesystem::exists (last); i++)
			std::this_thread::sleep_for (std::chrono::milliseconds (10));
	}
	char cmd[1024];
#ifdef _WIN32
	const char *quiet = "> NUL 2>&1";
#else
	const char *quiet = "> /dev/null 2>&1";
#endif
	snprintf (cmd, sizeof(cmd), "ffmpeg -version %s", quiet);
	if (std::system (cmd) == 0) {
		snprintf (cmd, sizeof(cmd), "ffmpeg -y -loglevel error -framerate %d -i \"%s/frame_%%05d.png\" -c:v libx264 -pix_fmt yuv420p -crf 18 \"%s/replay.mp4\" %s",
			exportFps, exportDir.c_str(), exportDir.c_str(), quiet);
		if (std::system (cmd) == 0) status = exportDir + "/replay.mp4 (" + std::to_string (exportFrame) + " frames)";
		else status = "Video encoding failed; frames in " + exportDir;
	} else {
		status = std::to_string (exportFrame) + " frames in " + exportDir + " (no ffmpeg for a video)";
	}
	LOGOUT ("Replay export: %s", status.c_str());
}

// =======================================================================
// Timeline and controls

void Replay::Display ()
{
	if (!active_) return;
	ImGuiIO &io = ImGui::GetIO();
	float W = io.DisplaySize.x, H = io.DisplaySize.y;
	float fs = ImGui::GetFontSize();
	float h = fs * 6.0f;
	ImGui::SetNextWindowPos (ImVec2 (0, H - h));
	ImGui::SetNextWindowSize (ImVec2 (W, h));
	ImGui::SetNextWindowBgAlpha (0.75f);
	if (!ImGui::Begin ("##replay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End ();
		return;
	}
	double t0 = Start(), t1 = End();

	ImGui::TextColored (ImVec4 (1.0f, 0.3f, 0.25f, 1.0f), "REPLAY");
	ImGui::SameLine ();
	ImGui::Text ("%s", RelTimeStr (t - t1).c_str());
	ImGui::SameLine ();
	ImGui::TextDisabled ("of %s", RelTimeStr (t0 - t1).c_str());
	ImGui::SameLine (fs * 14.0f);

	ImGui::BeginDisabled (exporting);
	if (ImGui::Button ("|<")) Seek (t0);
	uianchor::SetItem ("replay.start");
	ImGui::SameLine ();
	if (ImGui::Button ("<<")) Seek (t - 10.0);
	uianchor::SetItem ("replay.back");
	ImGui::SameLine ();
	if (ImGui::Button (playing ? "Pause" : "Play", ImVec2 (fs * 3.5f, 0))) {
		if (!playing && t >= t1) t = t0;
		playing = !playing;
	}
	uianchor::SetItem ("replay.play");
	ImGui::SameLine ();
	if (ImGui::Button (">>")) Seek (t + 10.0);
	uianchor::SetItem ("replay.forward");
	ImGui::SameLine ();
	if (ImGui::Button (">|")) Seek (t1);
	uianchor::SetItem ("replay.end");
	ImGui::SameLine ();
	static const double speeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 100.0 };
	char sbuf[32];
	snprintf (sbuf, sizeof(sbuf), "%gx", speed);
	ImGui::SetNextItemWidth (fs * 5.0f);
	if (ImGui::BeginCombo ("##speed", sbuf)) {
		for (double s : speeds) {
			snprintf (sbuf, sizeof(sbuf), "%gx", s);
			if (ImGui::Selectable (sbuf, s == speed)) speed = s;
		}
		ImGui::EndCombo ();
	}
	ImGui::SameLine ();
	ImGui::Checkbox ("Loop", &loop);
	uianchor::SetItem ("replay.loop");

	ImGui::SameLine (0, fs * 2.0f);
	ImGui::Checkbox ("Cinematic camera", &cinematic);
	uianchor::SetItem ("replay.cinematic");
	ImGui::SameLine ();
	if (ImGui::Button ("Add key")) AddKey ();
	uianchor::SetItem ("replay.addkey");
	ImGui::SameLine ();
	ImGui::BeginDisabled (keys.empty());
	if (ImGui::Button ("Delete key")) {
		auto it = std::min_element (keys.begin(), keys.end(), [this](const CamKey &a, const CamKey &b) { return fabs (a.t - t) < fabs (b.t - t); });
		if (it != keys.end()) keys.erase (it);
	}
	uianchor::SetItem ("replay.deletekey");
	ImGui::EndDisabled ();

	ImGui::SameLine (0, fs * 2.0f);
	if (ImGui::Button ("In")) tIn = t, tOut = std::max (tOut, t);
	uianchor::SetItem ("replay.in");
	ImGui::SameLine ();
	if (ImGui::Button ("Out")) tOut = t, tIn = std::min (tIn, t);
	uianchor::SetItem ("replay.out");
	ImGui::SameLine ();
	int fpsIdx = (exportFps == 30 ? 0 : 1);
	ImGui::SetNextItemWidth (fs * 4.5f);
	if (ImGui::Combo ("##fps", &fpsIdx, "30 fps\0" "60 fps\0")) exportFps = (fpsIdx == 0 ? 30 : 60);
	ImGui::SameLine ();
	if (ImGui::Button ("Export video")) StartExport ();
	uianchor::SetItem ("replay.export");
	ImGui::EndDisabled ();

	ImGui::SameLine (W - fs * 5.5f);
	bool exit = ImGui::Button ("Live", ImVec2 (fs * 4.5f, 0));
	uianchor::SetItem ("replay.live");
	if (exit) {
		ImGui::End ();
		Exit ();
		return;
	}

	// timeline: the recorded span, the export range, camera keys, the replay time
	ImGui::Spacing ();
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float tw = ImGui::GetContentRegionAvail().x, th = fs * 1.4f;
	ImGui::InvisibleButton ("##timeline", ImVec2 (tw, th));
	bool scrub = ImGui::IsItemActive ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	auto X = [&](double tt) { return p.x + (float)((tt - t0) / std::max (1e-6, t1 - t0)) * tw; };
	float yc = p.y + th * 0.5f;
	dl->AddRectFilled (ImVec2 (p.x, yc - 3), ImVec2 (p.x + tw, yc + 3), IM_COL32(80, 80, 90, 255), 3.0f);
	dl->AddRectFilled (ImVec2 (X (std::max (tIn, t0)), yc - 3), ImVec2 (X (std::min (tOut, t1)), yc + 3), IM_COL32(60, 120, 200, 255), 3.0f);
	dl->AddRectFilled (ImVec2 (p.x, yc - 3), ImVec2 (X (t), yc + 3), IM_COL32(220, 70, 60, 255), 3.0f);
	for (size_t i = 0; i < keys.size(); i++) {
		float x = X (keys[i].t);
		dl->AddTriangleFilled (ImVec2 (x, yc - 4), ImVec2 (x - 5, yc - 12), ImVec2 (x + 5, yc - 12), IM_COL32(240, 210, 60, 255));
		char name[32];
		snprintf (name, sizeof(name), "replay.key%d", (int)i);
		uianchor::Set (name, x, yc - 8);
	}
	dl->AddCircleFilled (ImVec2 (X (t), yc), 7.0f, IM_COL32(255, 255, 255, 255), 16);
	uianchor::Set ("replay.timeline.lo", p.x, yc);
	uianchor::Set ("replay.timeline.hi", p.x + tw, yc);
	uianchor::Set ("replay.timeline.head", X (t), yc);
	if (scrub && !exporting) {
		float x = std::clamp (io.MousePos.x, p.x, p.x + tw);
		Seek (t0 + (x - p.x) / tw * (t1 - t0));
	}

	if (exporting) {
		double frac = (t - std::max (t0, tIn)) / std::max (1e-6, std::min (t1, tOut) - std::max (t0, tIn));
		char buf[64];
		snprintf (buf, sizeof(buf), "Exporting frame %d", exportFrame);
		ImGui::ProgressBar ((float)std::clamp (frac, 0.0, 1.0), ImVec2 (fs * 20.0f, 0), buf);
	} else if (!status.empty()) {
		ImGui::TextDisabled ("%s", status.c_str());
	} else {
		ImGui::TextDisabled ("F5: back to the simulation. Drag the timeline to scrub; Add key records the camera for the cinematic camera.");
	}
	ImGui::End ();
}
