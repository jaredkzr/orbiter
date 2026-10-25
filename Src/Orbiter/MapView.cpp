// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MapView.h"
#include "Orbiter.h"
#include "Camera.h"
#include "Psys.h"
#include "Celbody.h"
#include "Vessel.h"
#include "Trajectory.h"
#include "Maneuver.h"
#include "Astro.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

extern Orbiter *g_pOrbiter;
extern Camera *g_camera;
extern Vessel *g_focusobj;
extern PlanetarySystem *g_psys;
extern TimeData td;

namespace {
	const ImU32 COL_PATH      = IM_COL32( 60, 200, 255, 255); // trajectory before the first node
	const ImU32 COL_PATH_NODE = IM_COL32(255, 160,  40, 255); // after a node
	const ImU32 COL_TARGET    = IM_COL32(200,  90, 255, 255); // target vessel's path
	const ImU32 COL_ORBIT     = IM_COL32(150, 150, 160, 140); // orbits of bodies
	const ImU32 COL_TEXT      = IM_COL32(230, 230, 230, 255);
	const ImU32 COL_PRO       = IM_COL32(230, 220,  40, 255); // node handles
	const ImU32 COL_NML       = IM_COL32(220,  60, 230, 255);
	const ImU32 COL_RAD       = IM_COL32( 60, 220, 230, 255);
	const ImU32 COL_NODE      = IM_COL32( 80, 140, 255, 255);
	const float HANDLE_DIST   = 70.0f;  // [px] handles from the node
	const float PICK_DIST     = 8.0f;   // [px]

	enum { SHAPE_DIAMOND, SHAPE_CIRCLE, SHAPE_TRIANGLE, SHAPE_CROSS, SHAPE_SQUARE };

	std::string MapTimeStr (double dt)
	{
		char buf[64];
		const char *sgn = dt < 0.0 ? "-" : "+";
		dt = fabs (dt);
		long s = (long)dt;
		int d = (int)(s / 86400); s %= 86400;
		if (d > 0) snprintf (buf, sizeof(buf), "T%s%dd %02ld:%02ld:%02ld", sgn, d, s / 3600, (s / 60) % 60, s % 60);
		else snprintf (buf, sizeof(buf), "T%s%02ld:%02ld:%02ld", sgn, s / 3600, (s / 60) % 60, s % 60);
		return buf;
	}

	std::string MapDistStr (double d)
	{
		char buf[64];
		if (fabs (d) < 1e4) snprintf (buf, sizeof(buf), "%.0f m", d);
		else if (fabs (d) < 1e8) snprintf (buf, sizeof(buf), "%.1f km", d * 1e-3);
		else snprintf (buf, sizeof(buf), "%.0f km", d * 1e-3);
		return buf;
	}

	float Dist2 (const ImVec2 &a, const ImVec2 &b) { float x = a.x - b.x, y = a.y - b.y; return x*x + y*y; }

	// distance of p from the segment ab, and the parameter of the closest point
	float SegDist (const ImVec2 &p, const ImVec2 &a, const ImVec2 &b, float &u)
	{
		float dx = b.x - a.x, dy = b.y - a.y, l2 = dx*dx + dy*dy;
		u = l2 > 0.0f ? std::clamp (((p.x - a.x)*dx + (p.y - a.y)*dy) / l2, 0.0f, 1.0f) : 0.0f;
		return sqrtf (Dist2 (p, ImVec2 (a.x + u*dx, a.y + u*dy)));
	}

	const CelestialBody *ReferenceBody (const Vessel *v)
	{
		return v->ElRef();
	}
}

MapView::MapView (): ImGuiDialog ("Map view")
{
	active = true; // (the overlay is always drawn; the map only when on)
}

void MapView::Toggle ()
{
	SetOn (!on);
}

void MapView::SetOn (bool o)
{
	if (o == on || !g_focusobj || !g_camera) return;
	if (o) {
		saved.target = g_camera->Target();
		saved.external = g_camera->IsExternal();
		saved.extmode = g_camera->GetExtMode();
		saved.dist = g_camera->Distance() / std::max (1.0, saved.target->Size());
		saved.phi = g_camera->Phi(), saved.theta = g_camera->Theta();

		mapRef = ReferenceBody (g_focusobj);
		if (!mapRef) return;
		// extent of the predicted trajectory (or of the current orbit) about the body
		Vector r (g_focusobj->GPos() - mapRef->GPos()), v (g_focusobj->GVel() - mapRef->GVel());
		double mu = Ggrav * mapRef->Mass();
		double energy = 0.5 * dotp (v, v) - mu / r.length();
		double rmax = r.length();
		if (energy < 0.0) rmax = std::max (rmax, std::min (-mu / energy, 4.0 * r.length())); // (apoapsis)
		trajectory::Predictor *pr = g_pOrbiter->Predictor();
		if (auto pred = pr ? pr->Latest() : nullptr) {
			for (const auto &seg : pred->segs) {
				Vector ofs (seg.ref->GPos() - mapRef->GPos());
				for (const auto &p : seg.pts) rmax = std::max (rmax, (ofs + p.r).length());
			}
		}
		g_pOrbiter->SetView ((Body*)mapRef, 1);
		g_camera->SetTrackMode (CAMERA_GLOBALFRAME);
		double fov = g_camera->TanAperture();
		g_camera->SetDistance (std::clamp (1.15 * rmax / (fov * mapRef->Size()), 2.5, 1e6));
		// looking down on the orbit plane, inclined by 35 degrees
		Vector n (crossp (v, r).unit()), u (n * cos (35.0*RAD) + r.unit() * sin (35.0*RAD));
		double phi = atan2 (u.x, -u.z), theta = asin (std::clamp (-u.y, -1.0, 1.0));
		g_camera->AddPhi (2.0 * (phi - g_camera->Phi()));
		g_camera->AddTheta (2.0 * (theta - g_camera->Theta()));
		on = true;
	} else {
		on = false;
		if (g_camera->Target() == mapRef && saved.target) {
			g_pOrbiter->SetView (saved.target, saved.external ? 1 : 0);
			if (saved.external) {
				g_camera->SetTrackMode (saved.extmode);
				g_camera->SetDistance (saved.dist);
				g_camera->AddPhi (2.0 * (saved.phi - g_camera->Phi()));     // (applies half)
				g_camera->AddTheta (2.0 * (saved.theta - g_camera->Theta()));
			}
		}
		drag = Drag::NONE;
	}
}

// =======================================================================

bool MapView::Project (const Vector &gp, ImVec2 &s) const
{
	Vector l (tmul (camRot, gp - camPos));
	if (l.z <= 0.0) return false;
	double asp = H / W;
	s.x = (float)(0.5 * W * (1.0 + l.x * asp / (tanAp * l.z)));
	s.y = (float)(0.5 * H * (1.0 - l.y / (tanAp * l.z)));
	return true;
}

bool MapView::Occluded (const Vector &gp) const
{
	Vector d (gp - camPos);
	double l2 = dotp (d, d);
	for (const auto &o : occluders) {
		Vector c (o.first - camPos);
		double u = dotp (c, d) / l2;
		if (u <= 0.0 || u >= 1.0) continue;     // (sphere not between camera and point)
		if ((gp - o.first).length() < o.second) continue; // (point inside: not hidden by it)
		if ((c - d * u).length() < o.second) return true;
	}
	return false;
}

void MapView::DrawPath (ImDrawList *dl, const std::vector<Vector> &pts, ImU32 col, float width, bool dashed)
{
	std::vector<ImVec2> run;
	auto flush = [&]() {
		if (run.size() > 1) {
			if (dashed) {
				for (size_t i = 0; i + 1 < run.size(); i += 2)
					dl->AddLine (run[i], run[i+1], col, width);
			} else dl->AddPolyline (run.data(), (int)run.size(), col, ImDrawFlags_None, width);
		}
		run.clear();
	};
	for (const Vector &p : pts) {
		ImVec2 s;
		if (!Project (p, s) || Occluded (p)) { flush (); continue; }
		if (s.x < -4*W || s.x > 5*W || s.y < -4*H || s.y > 5*H) { flush (); continue; }
		run.push_back (s);
	}
	flush ();
}

void MapView::DrawMarker (ImDrawList *dl, const Vector &gp, ImU32 col, const char *label, int shape)
{
	ImVec2 s;
	if (!Project (gp, s) || Occluded (gp)) return;
	const float r = 6.0f;
	switch (shape) {
	case SHAPE_DIAMOND:
		dl->AddQuadFilled (ImVec2 (s.x, s.y - r), ImVec2 (s.x + r, s.y), ImVec2 (s.x, s.y + r), ImVec2 (s.x - r, s.y), col);
		break;
	case SHAPE_CIRCLE:
		dl->AddCircle (s, r, col, 16, 2.0f);
		break;
	case SHAPE_TRIANGLE:
		dl->AddTriangleFilled (ImVec2 (s.x, s.y - r), ImVec2 (s.x + r, s.y + r*0.8f), ImVec2 (s.x - r, s.y + r*0.8f), col);
		break;
	case SHAPE_CROSS:
		dl->AddLine (ImVec2 (s.x - r, s.y - r), ImVec2 (s.x + r, s.y + r), col, 2.0f);
		dl->AddLine (ImVec2 (s.x - r, s.y + r), ImVec2 (s.x + r, s.y - r), col, 2.0f);
		break;
	case SHAPE_SQUARE:
		dl->AddRect (ImVec2 (s.x - r, s.y - r), ImVec2 (s.x + r, s.y + r), col, 0.0f, 0, 2.0f);
		break;
	}
	if (label && *label) {
		// (moved down while it overlaps labels placed before)
		ImVec2 sz = ImGui::CalcTextSize (label);
		ImVec2 p (s.x + r + 4.0f, s.y - 7.0f);
		for (int k = 0; k < 6; k++) {
			bool overlap = false;
			for (const ImVec4 &q : labels)
				if (p.x < q.z && p.x + sz.x > q.x && p.y < q.w && p.y + sz.y > q.y) { overlap = true; p.y = q.w + 1.0f; }
			if (!overlap) break;
		}
		labels.push_back (ImVec4 (p.x, p.y, p.x + sz.x, p.y + sz.y));
		dl->AddText (ImVec2 (p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 200), label);
		dl->AddText (p, col, label);
	}
}

Vector MapView::Anchor (const trajectory::Segment &seg) const
{
	if (seg.ref == mapRef) return mapRef->GPos();
	if (seg.ref->Primary() == mapRef) return mapRef->GPos() + (seg.refG - seg.parentG);
	return seg.ref->GPos();
}

Vector MapView::EventAnchor (const trajectory::Prediction *pred, const trajectory::Event &e) const
{
	if (e.seg >= 0 && e.seg < (int)pred->segs.size() && pred->segs[e.seg].ref == e.ref)
		return Anchor (pred->segs[e.seg]);
	return e.ref->GPos();
}

Vector MapView::NodeAnchor (const trajectory::Prediction *pred, size_t i) const
{
	const trajectory::NodeState &ns = pred->nodes[i];
	for (const auto &seg : pred->segs)
		if (seg.node == (int)i && seg.ref == ns.ref) return Anchor (seg);
	return ns.ref->GPos();
}

// =======================================================================

void MapView::Display ()
{
	if (!g_focusobj || !g_camera) return;
	trajectory::Predictor *pr = g_pOrbiter->Predictor();
	std::shared_ptr<const trajectory::Prediction> pred = pr ? pr->Latest() : nullptr;
	if (pred && pred->vessel != g_focusobj) pred.reset();

	// leave the map if the camera was moved elsewhere (e.g. to the cockpit)
	if (on && (g_camera->IsInternal() || g_camera->Target() != mapRef)) {
		on = false;
		drag = Drag::NONE;
	}

	ImGuiIO &io = ImGui::GetIO();
	W = io.DisplaySize.x, H = io.DisplaySize.y;
	if (W <= 0 || H <= 0) return;
	camPos = *g_camera->GPosPtr();
	camRot = g_camera->GRot();
	tanAp = g_camera->TanAperture();

	UpdateWarp ();
	labels.clear();
	if (on) DrawMap (pred.get());
	else DrawBurnCue (pred.get());
}

void MapView::UpdateWarp ()
{
	if (warpTo < 0.0) return;
	double rem = warpTo - td.SimT1;
	if (rem <= 0.0 || !g_focusobj || g_focusobj->ManeuverNodes().empty()) {
		g_pOrbiter->SetWarpFactor (1.0);
		warpTo = -1.0;
		return;
	}
	// the largest power of ten that takes at least 5 s (real time) to get there
	double w = 1.0;
	while (w < 100000.0 && rem / (w * 10.0) > 5.0) w *= 10.0;
	if (td.Warp() != w) g_pOrbiter->SetWarpFactor (w);
}

void MapView::DrawMap (const trajectory::Prediction *pred)
{
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImGuiIO &io = ImGui::GetIO();

	// planets and moons that may hide the lines
	occluders.clear();
	for (size_t i = 0; i < g_psys->nGrav(); i++) {
		const CelestialBody *cb = g_psys->GetGravObj ((int)i);
		double d = (cb->GPos() - camPos).length();
		if (d > cb->Size() && cb->Size() / (d * tanAp) * H > 1.0)
			occluders.push_back ({cb->GPos(), cb->Size()});
	}

	DrawBodyOrbits (dl);

	// trajectory (each segment relative to its reference body's current position)
	double now = td.SimT1;
	hoverPath = false;
	float hoverDist = PICK_DIST;
	ImVec2 mouse = io.MousePos;
	bool mouseFree = !io.WantCaptureMouse || drag != Drag::NONE;
	if (pred) {
		for (const auto &seg : pred->segs) {
			std::vector<Vector> pts;
			std::vector<double> ts;
			Vector rg (Anchor (seg));
			if (seg.ref != mapRef && seg.ref->Primary() == mapRef && !seg.pts.empty() && seg.pts.back().t >= now) {
				// the moon at the encounter
				ImVec2 c;
				if (Project (rg, c)) {
					float rad = (float)(seg.ref->Size() / ((rg - camPos).length() * tanAp) * H * 0.5);
					dl->AddCircle (c, std::max (rad, 4.0f), COL_ORBIT, 32, 1.5f);
					char label[96];
					snprintf (label, sizeof(label), "%s at %s", seg.ref->Name(), MapTimeStr (seg.pts.front().t - now).c_str());
					DrawMarker (dl, rg, COL_ORBIT, label, -1);
				}
			}
			for (const auto &p : seg.pts)
				if (p.t >= now) pts.push_back (rg + p.r), ts.push_back (p.t);
			ImU32 col = (seg.node < 0 ? COL_PATH : COL_PATH_NODE);
			DrawPath (dl, pts, col, 2.0f);

			// hover: the closest point of the path to the mouse
			if (mouseFree && drag == Drag::NONE) {
				ImVec2 a, b;
				for (size_t i = 0; i + 1 < pts.size(); i++) {
					if (!Project (pts[i], a) || !Project (pts[i+1], b) || Occluded (pts[i])) continue;
					float u, d = SegDist (mouse, a, b, u);
					if (d < hoverDist) hoverDist = d, hoverPath = true, hoverT = ts[i] + u * (ts[i+1] - ts[i]);
				}
			}
		}
		for (const auto &seg : pred->target) {
			std::vector<Vector> pts;
			Vector rg (Anchor (seg));
			for (const auto &p : seg.pts) if (p.t >= now) pts.push_back (rg + p.r);
			DrawPath (dl, pts, COL_TARGET, 1.5f, true);
		}

		// events (the first apsides of each part of the trajectory, all others)
		std::vector<std::pair<int,int>> shown; // (node, type)
		for (const auto &e : pred->events) {
			if (e.t < now) continue;
			char label[128];
			std::string tstr = MapTimeStr (e.t - now);
			ImU32 col = (e.node < 0 ? COL_PATH : COL_PATH_NODE);
			int shape = SHAPE_DIAMOND;
			switch (e.type) {
			case trajectory::Event::PERIAPSIS:
			case trajectory::Event::APOAPSIS: {
				auto key = std::make_pair (e.node, (int)e.type);
				if (std::find (shown.begin(), shown.end(), key) != shown.end()) continue;
				shown.push_back (key);
				snprintf (label, sizeof(label), "%s %s", e.type == trajectory::Event::PERIAPSIS ? "Pe" : "Ap", MapDistStr (e.value).c_str());
				} break;
			case trajectory::Event::SOI_ENTER:
				snprintf (label, sizeof(label), "Enter %s  %s", e.ref->Name(), tstr.c_str());
				shape = SHAPE_CIRCLE;
				break;
			case trajectory::Event::SOI_EXIT:
				snprintf (label, sizeof(label), "Leave for %s  %s", e.ref->Name(), tstr.c_str());
				shape = SHAPE_CIRCLE;
				break;
			case trajectory::Event::IMPACT:
				snprintf (label, sizeof(label), "Impact %s  %s", e.ref->Name(), tstr.c_str());
				col = IM_COL32(255, 70, 60, 255);
				shape = SHAPE_CROSS;
				break;
			case trajectory::Event::CLOSEST:
				snprintf (label, sizeof(label), "Closest %s, %.1f m/s", MapDistStr (e.value).c_str(), e.speed);
				col = COL_TARGET;
				shape = SHAPE_SQUARE;
				break;
			}
			DrawMarker (dl, EventAnchor (pred, e) + e.r, col, label, shape);
		}
	}

	// vessel and target
	DrawMarker (dl, g_focusobj->GPos(), IM_COL32(255, 255, 255, 255), g_focusobj->Name(), SHAPE_TRIANGLE);
	if (const Body *tgt = g_pOrbiter->PredictTarget())
		if (tgt->Type() == OBJTP_VESSEL) DrawMarker (dl, tgt->GPos(), COL_TARGET, tgt->Name(), SHAPE_TRIANGLE);

	DrawNodes (dl, pred);

	// add a node: click on the trajectory
	if (hoverPath && drag == Drag::NONE) {
		ImGui::SetNextFrameWantCaptureMouse (true);
		ImGui::SetTooltip ("%s\nClick to add a manoeuvre node", MapTimeStr (hoverT - now).c_str());
		if (ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
			maneuver::Node n;
			n.mjd = td.MJD_ref + hoverT / 86400.0;
			n.dv.Set (0, 0, 0);
			selNode = (int)g_focusobj->AddManeuverNode (n);
		}
	} else if (drag == Drag::NONE && mouseFree && ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
		selNode = -1; // (click elsewhere)
	}

	DrawPanels (pred);
}

void MapView::DrawBodyOrbits (ImDrawList *dl)
{
	// the map body's moons, the body's own orbit, and the bodies around the Sun if it is the map body
	std::vector<const CelestialBody*> bodies;
	for (DWORD i = 0; i < mapRef->nSecondary(); i++) bodies.push_back (mapRef->Secondary (i));
	// (the body's own orbit only when the view is large enough for it)
	if (mapRef->Primary() && (camPos - mapRef->GPos()).length() > 0.02 * (mapRef->GPos() - mapRef->Primary()->GPos()).length())
		bodies.push_back (mapRef);
	for (const CelestialBody *b : bodies) {
		const CelestialBody *p = b->Primary();
		Vector r0 (b->GPos() - p->GPos()), v0 (b->GVel() - p->GVel());
		double mu = Ggrav * (p->Mass() + b->Mass());
		double energy = 0.5 * dotp (v0, v0) - mu / r0.length();
		if (energy >= 0.0) continue;
		double a = -mu / (2.0 * energy);
		double T = 2.0 * Pi * sqrt (a*a*a / mu);
		std::vector<Vector> pts;
		const int n = 180;
		for (int i = 0; i <= n; i++) {
			Vector r, v;
			maneuver::Kepler (r0, v0, mu, T * i / n, r, v);
			pts.push_back (p->GPos() + r);
		}
		DrawPath (dl, pts, COL_ORBIT, 1.0f);
		if (b != mapRef) DrawMarker (dl, b->GPos(), COL_ORBIT, b->Name(), SHAPE_CIRCLE);
	}
}

void MapView::DrawNodes (ImDrawList *dl, const trajectory::Prediction *pred)
{
	ImGuiIO &io = ImGui::GetIO();
	const auto &nodes = g_focusobj->ManeuverNodes();
	if (selNode >= (int)nodes.size()) selNode = -1;
	if (!pred) return;
	ImVec2 mouse = io.MousePos;

	for (size_t i = 0; i < nodes.size() && i < pred->nodes.size(); i++) {
		const trajectory::NodeState &ns = pred->nodes[i];
		if (!ns.ref) continue;
		Vector gp (NodeAnchor (pred, i) + ns.r);
		ImVec2 s;
		if (!Project (gp, s) || Occluded (gp)) continue;
		bool sel = ((int)i == selNode);
		dl->AddCircleFilled (s, sel ? 8.0f : 6.0f, COL_NODE, 20);
		dl->AddCircle (s, sel ? 8.0f : 6.0f, IM_COL32(255, 255, 255, 255), 20, 1.5f);
		char label[64];
		snprintf (label, sizeof(label), "%.1f m/s", nodes[i].dv.length());
		dl->AddText (ImVec2 (s.x + 12.0f, s.y + 4.0f), COL_TEXT, label);

		bool overNode = Dist2 (mouse, s) < PICK_DIST * PICK_DIST * 2.0f;
		if (overNode && drag == Drag::NONE) {
			ImGui::SetNextFrameWantCaptureMouse (true);
			hoverPath = false;
			if (ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
				selNode = (int)i;
				drag = Drag::NODE;
				dragOrigin = mouse;
			}
		}
		if (!sel) continue;

		// handles: prograde/retrograde, normal/antinormal, radial out/in
		Vector pro, nml, rad;
		maneuver::Frame (ns.r, ns.v, pro, nml, rad);
		const Vector axes[3] = { pro, nml, rad };
		const ImU32 cols[3] = { COL_PRO, COL_NML, COL_RAD };
		double eps = 1e-3 * (gp - camPos).length();
		for (int a = 0; a < 3; a++) {
			for (int sg = -1; sg <= 1; sg += 2) {
				ImVec2 s2;
				if (!Project (gp + axes[a] * (eps * sg), s2)) continue;
				ImVec2 d (s2.x - s.x, s2.y - s.y);
				float l = sqrtf (d.x*d.x + d.y*d.y);
				if (l < 1e-4f) continue;
				d.x /= l, d.y /= l;
				float hl = HANDLE_DIST * std::clamp (l / 0.05f, 0.35f, 1.0f); // (shorter when seen end-on)
				ImVec2 h (s.x + d.x * hl, s.y + d.y * hl);
				dl->AddLine (s, h, cols[a] & IM_COL32(255, 255, 255, 120), 1.5f);
				if (sg > 0) dl->AddCircleFilled (h, 7.0f, cols[a], 16);
				else        dl->AddCircle (h, 7.0f, cols[a], 16, 2.5f);
				bool over = Dist2 (mouse, h) < 100.0f;
				if (over && drag == Drag::NONE) {
					static const char *names[3][2] = {{"Retrograde", "Prograde"}, {"Antinormal", "Normal"}, {"Radial in", "Radial out"}};
					ImGui::SetNextFrameWantCaptureMouse (true);
					ImGui::SetTooltip ("%s: drag to change", names[a][sg > 0]);
					hoverPath = false;
					if (ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
						drag = Drag::HANDLE;
						dragAxis = a, dragSign = sg;
						dragDir = d;
						dragOrigin = mouse;
					}
				}
			}
		}
	}

	// dragging
	if (drag != Drag::NONE) {
		ImGui::SetNextFrameWantCaptureMouse (true);
		if (!ImGui::IsMouseDown (ImGuiMouseButton_Left) || selNode < 0) {
			drag = Drag::NONE;
			return;
		}
		maneuver::Node n = nodes[selNode];
		if (drag == Drag::HANDLE) {
			// the velocity change grows at a rate given by how far the handle is pulled
			float d = (mouse.x - dragOrigin.x) * dragDir.x + (mouse.y - dragOrigin.y) * dragDir.y;
			double f = d / 40.0;
			double rate = 20.0 * f * fabs (f); // [m/s per s]
			if (io.KeyShift) rate *= 0.1;
			if (io.KeyCtrl) rate *= 10.0;
			double ddv = dragSign * rate * io.DeltaTime;
			if (ddv != 0.0) {
				double *c = (dragAxis == 0 ? &n.dv.x : dragAxis == 1 ? &n.dv.y : &n.dv.z);
				*c += ddv;
				g_focusobj->SetManeuverNode ((DWORD)selNode, n);
			}
		} else if (drag == Drag::NODE && Dist2 (mouse, dragOrigin) > 9.0f) {
			// move the node along the trajectory: to the closest point to the mouse
			double now = td.SimT1, bestT = -1.0;
			float best = 1e9f;
			for (const auto &seg : pred->segs) {
				if (seg.node >= selNode) continue; // (before this node)
				Vector rg (Anchor (seg));
				for (size_t i = 0; i + 1 < seg.pts.size(); i++) {
					if (seg.pts[i].t < now) continue;
					ImVec2 a, b;
					if (!Project (rg + seg.pts[i].r, a) || !Project (rg + seg.pts[i+1].r, b)) continue;
					float u, dd = SegDist (mouse, a, b, u);
					if (dd < best) best = dd, bestT = seg.pts[i].t + u * (seg.pts[i+1].t - seg.pts[i].t);
				}
			}
			if (bestT > now && best < 60.0f) {
				n.mjd = td.MJD_ref + bestT / 86400.0;
				g_focusobj->SetManeuverNode ((DWORD)selNode, n);
				// (the node may have moved in the time order)
				const auto &nn = g_focusobj->ManeuverNodes();
				for (size_t i = 0; i < nn.size(); i++) if (nn[i].mjd == n.mjd) selNode = (int)i;
			}
		}
	}
}

void MapView::DrawPanels (const trajectory::Prediction *pred)
{
	double now = td.SimT1;
	ImGuiIO &io = ImGui::GetIO();
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

	// orbit and target
	ImGui::SetNextWindowPos (ImVec2 (12.0f, io.DisplaySize.y - 12.0f), ImGuiCond_Always, ImVec2 (0.0f, 1.0f));
	ImGui::SetNextWindowBgAlpha (0.75f);
	if (ImGui::Begin ("Map##info", nullptr, flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
		const CelestialBody *ref = ReferenceBody (g_focusobj);
		ImGui::Text ("%s orbiting %s", g_focusobj->Name(), ref ? ref->Name() : "-");
		if (ref) {
			Vector r (g_focusobj->GPos() - ref->GPos()), v (g_focusobj->GVel() - ref->GVel());
			double mu = Ggrav * ref->Mass(), rl = r.length();
			double energy = 0.5 * dotp (v, v) - mu / rl;
			Vector h (crossp (r, v));
			double p = dotp (h, h) / mu;
			double e = energy < 0.0 ? sqrt (std::max (0.0, 1.0 + 2.0 * energy * p / mu)) : 1.0;
			if (energy < 0.0) {
				double a = -mu / (2.0 * energy);
				ImGui::Text ("Pe %s   Ap %s   T %s", MapDistStr (a * (1.0 - e) - ref->Size()).c_str(),
					MapDistStr (a * (1.0 + e) - ref->Size()).c_str(), MapTimeStr (2.0 * Pi * sqrt (a*a*a / mu)).c_str() + 2);
			} else {
				ImGui::Text ("Escape trajectory (Pe %s)", MapDistStr (p / (1.0 + e) - ref->Size()).c_str());
			}
		}
		// target
		const Body *tgt = g_pOrbiter->PredictTarget();
		ImGui::SetNextItemWidth (ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo ("Target", tgt ? tgt->Name() : "None")) {
			if (ImGui::Selectable ("None", !tgt)) g_pOrbiter->SetPredictTarget (nullptr);
			ImGui::SeparatorText ("Bodies");
			for (size_t i = 0; i < g_psys->nGrav(); i++) {
				const CelestialBody *cb = g_psys->GetGravObj ((int)i);
				if (ImGui::Selectable (cb->Name(), cb == tgt)) g_pOrbiter->SetPredictTarget (cb);
			}
			ImGui::SeparatorText ("Vessels");
			for (DWORD i = 0; i < g_psys->nVessel(); i++) {
				const Vessel *v = g_psys->GetVessel (i);
				if (v == g_focusobj) continue;
				if (ImGui::Selectable (v->Name(), v == tgt)) g_pOrbiter->SetPredictTarget (v);
			}
			ImGui::EndCombo ();
		}
		if (!pred) ImGui::TextDisabled ("Predicting...");
		ImGui::TextDisabled ("Click the trajectory to add a node. M: close the map");
	}
	ImGui::End ();

	// selected node
	const auto &nodes = g_focusobj->ManeuverNodes();
	if (selNode < 0 && !nodes.empty()) selNode = 0;
	if (selNode < 0 || selNode >= (int)nodes.size()) return;
	maneuver::Node n = nodes[selNode];
	bool changed = false;
	ImGui::SetNextWindowPos (ImVec2 (io.DisplaySize.x - 12.0f, 60.0f), ImGuiCond_Appearing, ImVec2 (1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha (0.8f);
	if (ImGui::Begin ("Manoeuvre node", nullptr, flags)) {
		double tnode = (n.mjd - td.MJD_ref) * 86400.0;
		ImGui::Text ("Node %d of %d   %s", selNode + 1, (int)nodes.size(), MapTimeStr (now - tnode).c_str());
		ImGui::SameLine ();
		if (ImGui::SmallButton ("<") && selNode > 0) selNode--;
		ImGui::SameLine ();
		if (ImGui::SmallButton (">") && selNode + 1 < (int)nodes.size()) selNode++;

		ImGui::SeparatorText ("Velocity change [m/s]");
		float w = ImGui::GetFontSize() * 9.0f;
		const char *names[3] = { "Prograde", "Normal", "Radial" };
		double *comp[3] = { &n.dv.x, &n.dv.y, &n.dv.z };
		for (int a = 0; a < 3; a++) {
			ImGui::PushID (a);
			ImGui::SetNextItemWidth (w);
			changed |= ImGui::DragScalar (names[a], ImGuiDataType_Double, comp[a], 0.1f, nullptr, nullptr, "%.2f");
			ImGui::SameLine ();
			if (ImGui::SmallButton ("0")) *comp[a] = 0.0, changed = true;
			ImGui::PopID ();
		}
		ImGui::SeparatorText ("Time");
		double dt = 0.0;
		ImGui::SetNextItemWidth (w);
		if (ImGui::DragScalar ("Shift [s]", ImGuiDataType_Double, &dt, 1.0f, nullptr, nullptr, "%.0f")) {}
		if (ImGui::IsItemActive ()) dt = io.MouseDelta.x * (io.KeyShift ? 0.1 : io.KeyCtrl ? 100.0 : 5.0);
		if (dt != 0.0) n.mjd += dt / 86400.0, changed = true;
		// one orbit later / earlier
		if (pred && selNode < (int)pred->nodes.size() && pred->nodes[selNode].ref) {
			const auto &ns = pred->nodes[selNode];
			double mu = Ggrav * ns.ref->Mass();
			double energy = 0.5 * dotp (ns.v, ns.v) - mu / ns.r.length();
			if (energy < 0.0) {
				double a = -mu / (2.0 * energy), T = 2.0 * Pi * sqrt (a*a*a / mu);
				if (ImGui::SmallButton ("-1 orbit") && tnode - T > now) n.mjd -= T / 86400.0, changed = true;
				ImGui::SameLine ();
				if (ImGui::SmallButton ("+1 orbit")) n.mjd += T / 86400.0, changed = true;
			}
		}

		ImGui::Separator ();
		double dv = n.dv.length(), tb = g_focusobj->BurnDuration (dv);
		ImGui::Text ("Total %.2f m/s", dv);
		if (tb >= 0.0) ImGui::Text ("Burn %.1f s (main engines)", tb);
		else ImGui::TextDisabled ("No main engines");
		changed |= ImGui::Checkbox ("Burn autopilot", &n.autoburn);
		if (selNode == 0) {
			const auto &b = g_focusobj->Burn();
			if (b.phase == Vessel::BurnState::ALIGN) ImGui::TextColored (ImVec4 (1, 0.8f, 0.3f, 1), "Turning to the burn direction");
			else if (b.phase == Vessel::BurnState::BURN) ImGui::TextColored (ImVec4 (1, 0.5f, 0.2f, 1), "Burning: %.1f m/s left", b.dvg.length());
		}
		if (ImGui::Button ("Warp to node")) warpTo = tnode - std::max (0.0, tb) * 0.5 - 120.0;
		ImGui::SameLine ();
		if (ImGui::Button ("Delete")) {
			g_focusobj->DelManeuverNode ((DWORD)selNode);
			selNode = -1;
			changed = false;
		}
	}
	ImGui::End ();
	if (changed) {
		g_focusobj->SetManeuverNode ((DWORD)selNode, n);
		const auto &nn = g_focusobj->ManeuverNodes();
		for (size_t i = 0; i < nn.size(); i++) if (nn[i].mjd == n.mjd) selNode = (int)i;
	}
	if (selNode >= 0 && ImGui::IsKeyPressed (ImGuiKey_Delete) && !io.WantTextInput) {
		g_focusobj->DelManeuverNode ((DWORD)selNode);
		selNode = -1;
	}
}

// =======================================================================
// Burn cue (outside the map): the next node's time, velocity change and
// burn direction

void MapView::DrawBurnCue (const trajectory::Prediction *pred)
{
	const auto &nodes = g_focusobj->ManeuverNodes();
	if (nodes.empty()) return;
	const maneuver::Node &n = nodes[0];
	double now = td.SimT1;
	double tnode = (n.mjd - td.MJD_ref) * 86400.0;
	const auto &b = g_focusobj->Burn();
	double dv = (b.phase == Vessel::BurnState::BURN ? b.dvg.length() : n.dv.length());
	double tb = g_focusobj->BurnDuration (n.dv.length());
	double tstart = tnode - 0.5 * std::max (0.0, tb);

	// direction marker: the velocity change's direction, as seen from the camera
	Vector dvg;
	if (b.phase != Vessel::BurnState::NONE) dvg = b.dvg;
	else if (pred && !pred->nodes.empty() && pred->nodes[0].ref) dvg = pred->nodes[0].dvg;
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImVec2 s;
	if (dvg.length() > 0.0 && Project (camPos + dvg.unit() * 1e3, s)) {
		ImU32 col = IM_COL32(70, 140, 255, 255);
		dl->AddCircle (s, 14.0f, col, 24, 2.5f);
		dl->AddLine (ImVec2 (s.x - 22, s.y), ImVec2 (s.x - 8, s.y), col, 2.0f);
		dl->AddLine (ImVec2 (s.x + 8, s.y), ImVec2 (s.x + 22, s.y), col, 2.0f);
		dl->AddLine (ImVec2 (s.x, s.y - 22), ImVec2 (s.x, s.y - 8), col, 2.0f);
	}

	ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos (ImVec2 (io.DisplaySize.x * 0.5f, 8.0f), ImGuiCond_Always, ImVec2 (0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha (0.6f);
	if (ImGui::Begin ("##burncue", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
		ImGui::Text ("Node %s   %.1f m/s", MapTimeStr (now - tnode).c_str(), dv);
		if (tb >= 0.0) {
			ImGui::SameLine ();
			ImGui::TextDisabled ("  burn %.1f s, start %s", tb, MapTimeStr (now - tstart).c_str());
		}
		if (b.phase == Vessel::BurnState::ALIGN) ImGui::TextColored (ImVec4 (1, 0.8f, 0.3f, 1), "Autopilot: turning to the burn direction");
		else if (b.phase == Vessel::BurnState::BURN) {
			ImGui::TextColored (ImVec4 (1, 0.5f, 0.2f, 1), "Autopilot: burning");
			float f = (float)(1.0 - dv / std::max (1e-6, n.dv.length()));
			ImGui::ProgressBar (std::clamp (f, 0.0f, 1.0f), ImVec2 (ImGui::GetFontSize() * 16.0f, 0));
		} else if (n.autoburn) ImGui::TextDisabled ("Autopilot armed");
	}
	ImGui::End ();
}
