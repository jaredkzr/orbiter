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
#include "UiTheme.h"
#include "IconsLucide.h"
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
	const ui::Fonts &f = ui::GetFonts();
	const ImGuiStyle &st = ImGui::GetStyle();

	// a panel floating above the bottom edge: state and transport, the
	// timeline, the camera keys and the export
	float m = ui::Px (16);
	float bw = std::min (W - 2 * m, ui::Px (1240));
	ImGui::SetNextWindowPos (ImVec2 ((W - bw) * 0.5f, H - m), ImGuiCond_Always, ImVec2 (0.0f, 1.0f));
	ImGui::SetNextWindowSizeConstraints (ImVec2 (bw, 0), ImVec2 (bw, H));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (ui::Px (20), ui::Px (16)));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, ui::Px (12));
	ImGui::PushStyleColor (ImGuiCol_WindowBg, ui::Col (ui::WINDOW, 0.97f));
	bool open = ImGui::Begin ("##replay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::PopStyleColor ();
	ImGui::PopStyleVar (2);
	if (!open) {
		ImGui::End ();
		return;
	}
	double t0 = Start(), t1 = End();
	float cw = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX ();
	float fh = ImGui::GetFrameHeight (), bh = fh * 1.2f, gap = ui::Px (6);

	// a square tool button: an icon, a tooltip
	auto tool = [&](const char *icon, const char *tip, const char *anchor, bool on = false) {
		ImGui::PushStyleColor (ImGuiCol_Button, on ? ui::Col (ui::ACCENT, 0.2f) : ui::Col (ui::RAISED, 0.0f));
		ImGui::PushStyleColor (ImGuiCol_ButtonHovered, ui::Col (ui::RAISED_HOVER));
		ImGui::PushStyleColor (ImGuiCol_ButtonActive, ui::Col (ui::RAISED_ACTIVE));
		ImGui::PushStyleColor (ImGuiCol_Text, on ? ui::Col (ui::ACCENT) : ui::Col (ui::TEXT));
		bool r = ImGui::Button (icon, ImVec2 (bh, bh));
		ImGui::PopStyleColor (4);
		if (anchor) uianchor::SetItem (anchor);
		if (tip && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip ("%s", tip);
		return r;
	};

	// (SameLine returns to the y of the row's first item: set it per item)
	auto same = [](float y, float x = 0.0f, float sp = -1.0f) { ImGui::SameLine (x, sp); ImGui::SetCursorPosY (y); };

	// -- state: the badge, the replay time (to the live time) and the span
	float y0 = ImGui::GetCursorPosY ();
	ui::PushFont (f.medium, ui::SIZE_SMALL);
	float badgeH = ImGui::GetTextLineHeight () + 2 * ui::Px (3);
	ui::PopFont ();
	ImGui::SetCursorPosY (y0 + (bh - badgeH) * 0.5f);
	ui::Badge (ICON_LC_HISTORY " REPLAY", ui::WARNING);
	ui::PushFont (f.mono, ui::SIZE_BODY);
	same (y0 + (bh - ImGui::GetTextLineHeight ()) * 0.5f, 0, ui::Px (12));
	ImGui::TextUnformatted (RelTimeStr (t - t1).c_str());
	same (y0 + (bh - ImGui::GetTextLineHeight ()) * 0.5f, 0, ui::Px (4));
	ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_DIM));
	ImGui::Text ("/ %s", RelTimeStr (t0 - t1).c_str());
	ImGui::PopStyleColor ();
	ui::PopFont ();

	// -- transport, in the middle
	static const double speeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 100.0 };
	float pw = bh * 1.6f, sw = ui::Px (78);
	float tw = 4 * (bh + gap) + pw + gap + sw + gap + bh;
	ImGui::SameLine (x0 + (cw - tw) * 0.5f);
	ImGui::SetCursorPosY (y0);
	ImGui::BeginDisabled (exporting);
	if (tool (ICON_LC_SKIP_BACK, "To the start", "replay.start")) Seek (t0);
	same (y0, 0, gap);
	if (tool (ICON_LC_REWIND, "Back 10 s", "replay.back")) Seek (t - 10.0);
	same (y0, 0, gap);
	ImGui::PushStyleVar (ImGuiStyleVar_FrameRounding, bh * 0.5f);
	bool pp = ui::PrimaryButton (playing ? ICON_LC_PAUSE "##pp" : ICON_LC_PLAY "##pp", ImVec2 (pw, bh));
	ImGui::PopStyleVar ();
	uianchor::SetItem ("replay.play");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip (playing ? "Pause" : "Play");
	if (pp) {
		if (!playing && t >= t1) t = t0;
		playing = !playing;
	}
	same (y0, 0, gap);
	if (tool (ICON_LC_FAST_FORWARD, "Forward 10 s", "replay.forward")) Seek (t + 10.0);
	same (y0, 0, gap);
	if (tool (ICON_LC_SKIP_FORWARD, "To the end", "replay.end")) Seek (t1);
	same (y0, 0, gap);
	char sbuf[32];
	snprintf (sbuf, sizeof(sbuf), "%g\xC3\x97", speed);
	ImGui::SetNextItemWidth (sw);
	ImGui::SetCursorPosY (y0 + (bh - fh) * 0.5f);
	if (ImGui::BeginCombo ("##speed", sbuf)) {
		for (double s : speeds) {
			snprintf (sbuf, sizeof(sbuf), "%g\xC3\x97", s);
			if (ImGui::Selectable (sbuf, s == speed)) speed = s;
		}
		ImGui::EndCombo ();
	}
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("Playback speed");
	same (y0, 0, gap);
	if (tool (ICON_LC_REPEAT, loop ? "Loop: on" : "Loop: off", "replay.loop", loop)) loop = !loop;
	ImGui::EndDisabled ();

	// -- back to the simulation, at the right
	float lw = ui::Px (112);
	ImGui::SameLine (x0 + cw - lw);
	ImGui::SetCursorPosY (y0);
	bool exit = ui::SecondaryButton (ICON_LC_RADIO "  Live", ImVec2 (lw, bh));
	uianchor::SetItem ("replay.live");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("Back to the simulation (F5)");
	if (exit) {
		ImGui::End ();
		Exit ();
		return;
	}

	// -- timeline: the recorded span, the export range, camera keys, the replay time
	ImGui::Dummy (ImVec2 (0, ui::Px (4)));
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float lw2 = ImGui::GetContentRegionAvail().x, th = ui::Px (30);
	ImGui::InvisibleButton ("##timeline", ImVec2 (lw2, th));
	bool scrub = ImGui::IsItemActive (), thov = ImGui::IsItemHovered ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	auto X = [&](double tt) { return p.x + (float)((tt - t0) / std::max (1e-6, t1 - t0)) * lw2; };
	float yc = p.y + th * 0.5f, r = ui::Px (3);
	if (tIn < tOut && (tIn > t0 || tOut < t1)) // (the export range, when set: a band behind the track)
		dl->AddRectFilled (ImVec2 (X (std::max (tIn, t0)), yc - ui::Px (8)), ImVec2 (X (std::min (tOut, t1)), yc + ui::Px (8)),
			ui::Col32 (ui::ACCENT, 0.16f), ui::Px (4));
	dl->AddRectFilled (ImVec2 (p.x, yc - r), ImVec2 (p.x + lw2, yc + r), ui::Col32 (ui::RAISED_HOVER), r);
	dl->AddRectFilled (ImVec2 (p.x, yc - r), ImVec2 (X (t), yc + r), ui::Col32 (ui::ACCENT), r);
	if (thov && !scrub) { // (where a click goes)
		float hx = std::clamp (io.MousePos.x, p.x, p.x + lw2);
		dl->AddLine (ImVec2 (hx, p.y + ui::Px (4)), ImVec2 (hx, p.y + th - ui::Px (4)), ui::Col32 (ui::TEXT, 0.35f), std::max (1.0f, ui::Scale ()));
		ImGui::SetTooltip ("%s", RelTimeStr (t0 + (hx - p.x) / lw2 * (t1 - t0) - t1).c_str());
	}
	for (size_t i = 0; i < keys.size(); i++) {
		float x = X (keys[i].t), d = ui::Px (5), ky = yc - ui::Px (9);
		dl->AddQuadFilled (ImVec2 (x, ky - d), ImVec2 (x + d, ky), ImVec2 (x, ky + d), ImVec2 (x - d, ky), ui::Col32 (ui::CAUTION));
		char name[32];
		snprintf (name, sizeof(name), "replay.key%d", (int)i);
		uianchor::Set (name, x, ky);
	}
	dl->AddCircleFilled (ImVec2 (X (t), yc), ui::Px (8), ui::Col32 (ui::ACCENT), 20);
	dl->AddCircleFilled (ImVec2 (X (t), yc), ui::Px (5), ui::Col32 (ui::TEXT), 20);
	uianchor::Set ("replay.timeline.lo", p.x, yc);
	uianchor::Set ("replay.timeline.hi", p.x + lw2, yc);
	uianchor::Set ("replay.timeline.head", X (t), yc);
	if (scrub && !exporting) {
		float x = std::clamp (io.MousePos.x, p.x, p.x + lw2);
		Seek (t0 + (x - p.x) / lw2 * (t1 - t0));
	}

	// -- camera keys (left), the export (right), the status between
	ImGui::Dummy (ImVec2 (0, ui::Px (2)));
	float y1 = ImGui::GetCursorPosY ();
	ImGui::BeginDisabled (exporting);
	ImGui::SetCursorPosY (y1 + (bh - fh) * 0.5f);
	ui::Toggle ("Cinematic camera", &cinematic);
	uianchor::SetItem ("replay.cinematic");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("The camera follows the keys (a smooth path through them)");
	same (y1, 0, ui::Px (14));
	if (ui::SecondaryButton (ICON_LC_DIAMOND_PLUS "  Add key", ImVec2 (0, bh))) AddKey ();
	uianchor::SetItem ("replay.addkey");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("Keep the camera's view at this time");
	same (y1, 0, gap);
	ImGui::BeginDisabled (keys.empty());
	if (ui::SecondaryButton (ICON_LC_DIAMOND_MINUS "  Delete key", ImVec2 (0, bh))) {
		auto it = std::min_element (keys.begin(), keys.end(), [this](const CamKey &a, const CamKey &b) { return fabs (a.t - t) < fabs (b.t - t); });
		if (it != keys.end()) keys.erase (it);
	}
	uianchor::SetItem ("replay.deletekey");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip (keys.empty() ? "No keys" : "Delete the key nearest this time");
	ImGui::EndDisabled ();
	float leftEnd = ImGui::GetItemRectMax().x;

	// the export group's width, to place it at the right
	auto bwid = [&](const char *l) { return ImGui::CalcTextSize (l, nullptr, true).x + 2 * st.FramePadding.x; };
	const char *expl = ICON_LC_FILM "  Export video";
	float fw = ui::Px (86);
	float ew = bwid ("In") + gap + bwid ("Out") + gap + fw + gap + bwid (expl);
	float ex = x0 + cw - ew;

	// status / progress
	ImVec2 sp (leftEnd + ui::Px (16), ImGui::GetWindowPos().y + y1);
	float smax = ImGui::GetWindowPos().x + ex - ui::Px (16);
	if (exporting) {
		double frac = (t - std::max (t0, tIn)) / std::max (1e-6, std::min (t1, tOut) - std::max (t0, tIn));
		char buf[64];
		snprintf (buf, sizeof(buf), "Exporting frame %d", exportFrame);
		ImGui::SameLine ();
		ImGui::SetCursorScreenPos (ImVec2 (sp.x, sp.y + (bh - fh) * 0.5f));
		ImGui::ProgressBar ((float)std::clamp (frac, 0.0, 1.0), ImVec2 (std::max (ui::Px (60), smax - sp.x), 0), buf);
	} else {
		const char *msg = !status.empty() ? status.c_str() : "Drag the timeline to scrub. F5: back to the simulation.";
		ui::PushFont (f.body, ui::SIZE_SMALL);
		ImVec2 ms = ImGui::CalcTextSize (msg);
		ImGui::PushClipRect (sp, ImVec2 (smax, sp.y + bh), true);
		dl->AddText (ImVec2 (std::max (sp.x, (sp.x + smax - ms.x) * 0.5f), sp.y + (bh - ms.y) * 0.5f), ui::Col32 (ui::TEXT_DIM), msg);
		ImGui::PopClipRect ();
		ui::PopFont ();
	}

	same (y1, ex);
	if (ui::SecondaryButton ("In", ImVec2 (0, bh))) tIn = t, tOut = std::max (tOut, t);
	uianchor::SetItem ("replay.in");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("The export starts here");
	same (y1, 0, gap);
	if (ui::SecondaryButton ("Out", ImVec2 (0, bh))) tOut = t, tIn = std::min (tIn, t);
	uianchor::SetItem ("replay.out");
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("The export ends here");
	same (y1 + (bh - fh) * 0.5f, 0, gap);
	int fpsIdx = (exportFps == 30 ? 0 : 1);
	ImGui::SetNextItemWidth (fw);
	if (ImGui::Combo ("##fps", &fpsIdx, "30 fps\0" "60 fps\0")) exportFps = (fpsIdx == 0 ? 30 : 60);
	same (y1, 0, gap);
	if (ui::SecondaryButton (expl, ImVec2 (0, bh))) StartExport ();
	uianchor::SetItem ("replay.export");
	ImGui::EndDisabled ();
	ImGui::End ();
}
