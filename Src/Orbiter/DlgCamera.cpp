// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Camera configuration dialog: the view (cockpit or external, tracking
// mode, field of view, direction), the camera target, ground observer
// positions and preset views.
// ======================================================================

#include "DlgCamera.h"
#include "OrbiterAPI.h"
#include "Orbiter.h"
#include "Celbody.h"
#include "Psys.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cctype>
#include <cstring>

extern PlanetarySystem *g_psys;
extern Camera *g_camera;
extern TimeData td;
extern Orbiter *g_pOrbiter;
extern Vessel *g_focusobj;

namespace {

	std::string Lower (const char *s)
	{
		std::string r (s);
		for (auto &c : r) c = (char)tolower ((unsigned char)c);
		return r;
	}

	/// A label in the left column of a form, the control after it
	void FormLabel (const char *label, float width)
	{
		ImGui::AlignTextToFramePadding ();
		float x = ImGui::GetCursorPosX ();
		ui::TextDim ("%s", label);
		ImGui::SameLine (x + width);
	}

	/// An input field with its unit drawn at the right end
	void UnitInput (const char *id, char *buf, size_t size, const char *unit, float width)
	{
		ImGui::SetNextItemWidth (width);
		ImGui::InputText (id, buf, size, ImGuiInputTextFlags_CharsDecimal);
		ImVec2 r0 = ImGui::GetItemRectMin (), r1 = ImGui::GetItemRectMax ();
		ImVec2 us = ImGui::CalcTextSize (unit);
		ImGui::GetWindowDrawList()->AddText (ImVec2(r1.x - us.x - ui::Px (8), (r0.y + r1.y - us.y) * 0.5f), ui::Col32 (ui::TEXT_FAINT), unit);
	}

	const char *ModeName (bool extcam, ExtCamMode m, bool lock)
	{
		if (!extcam) return "COCKPIT";
		switch (m) {
		case CAMERA_TARGETRELATIVE:   return "TARGET RELATIVE";
		case CAMERA_ABSDIRECTION:     return "ABSOLUTE DIRECTION";
		case CAMERA_GLOBALFRAME:      return "GLOBAL FRAME";
		case CAMERA_TARGETTOOBJECT:   return "TARGET TO OBJECT";
		case CAMERA_TARGETFROMOBJECT: return "TARGET FROM OBJECT";
		case CAMERA_GROUNDOBSERVER:   return lock ? "GROUND \xC2\xB7 LOCKED" : "GROUND OBSERVER";
		}
		return "";
	}

}

DlgCamera::DlgCamera() : ImGuiDialog(ICON_LC_VIDEO "  Camera", {440, 640}) {
}

void DlgCamera::OnDraw() {
	extmode = g_camera->GetExtMode ();
	intmode = g_camera->GetIntMode ();
	extcam = (g_camera->GetMode () != CAM_COCKPIT);
	ground_lock = g_camera->GroundObserver_TargetLock();
	rot_is_tilt = (!extcam || (extmode == CAMERA_GROUNDOBSERVER && !ground_lock));

	if(followterrain[0]=='\0')
		snprintf (followterrain, sizeof(followterrain), "%0.0lf", g_camera->GroundObserver_TerrainLimit());

	DrawStatus ();
	if (ImGui::BeginTabBar("CameraTabBar"))
	{
		for(size_t i = 0; i < sizeof(tabs)/sizeof(tabs[0]); i++) {
			if(ImGui::BeginTabItem(tabs[i].name)) {
				SetHelp("html/orbiter.chm", tabs[i].helptopic);
				ImGui::Dummy (ImVec2(0, ui::Px (2)));
				(this->*tabs[i].func)();
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
}

// the camera target and mode
void DlgCamera::DrawStatus() {
	const ui::Fonts &f = ui::GetFonts ();
	Body *tgt = g_camera->Target ();
	ImGui::AlignTextToFramePadding ();
	ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::ACCENT));
	ImGui::TextUnformatted (extcam ? ICON_LC_EYE : ICON_LC_SCAN_EYE);
	ImGui::PopStyleColor ();
	ImGui::SameLine ();
	ui::PushFont (f.strong, ui::SIZE_BODY + 1.0f);
	ImGui::TextUnformatted (tgt ? tgt->Name () : "");
	ui::PopFont ();
	ImGui::SameLine (0, ui::Px (10));
	ImGui::SetCursorPosY (ImGui::GetCursorPosY () + ui::Px (1));
	ui::Badge (ModeName (extcam, extmode, ground_lock), ui::TEXT_DIM);
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
}

// ----------------------------------------------------------------------
// View: cockpit or external, tracking mode, field of view, direction

void DlgCamera::DrawView() {
	ui::SectionHeader ("Camera");
	static const char *views[] = { ICON_LC_SCAN_EYE "  Cockpit", ICON_LC_EYE "  External" };
	int view = extcam ? 1 : 0;
	if (ui::Segmented ("##view", views, 2, &view)) {
		if (view == 0) g_pOrbiter->SetView (g_focusobj, 0);
		else g_pOrbiter->SetView (g_camera->Target () ? g_camera->Target () : g_focusobj, 1);
	}
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip ("F1");

	if (extcam) {
		ImGui::Dummy (ImVec2(0, ui::Px (2)));
		ui::SectionHeader ("Tracking");
		static const char *track[] = { "Target relative", "Absolute dir.", "Global frame" };
		int t = extmode <= CAMERA_GLOBALFRAME ? (int)extmode : -1;
		if (ui::Segmented ("##track", track, 3, &t))
			g_camera->SetTrackMode ((ExtCamMode)t);
		if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip ("F2 cycles the tracking modes");
		if (extmode == CAMERA_TARGETTOOBJECT || extmode == CAMERA_TARGETFROMOBJECT)
			ui::TextDim ("Aligned with a reference object (chosen in Target).");
		else if (extmode == CAMERA_GROUNDOBSERVER)
			ui::TextDim ("Fixed to the ground (set in Ground).");
	}

	// field of view [deg]: the slider, and common values
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	ui::SectionHeader ("Field of view");
	float fov = (float)(oapiCameraAperture () / RAD * 2.0);
	ImGui::SetNextItemWidth (-FLT_MIN);
	if (ImGui::SliderFloat ("##fov", &fov, 10.0f, 90.0f, "%.0f\xC2\xB0"))
		oapiCameraSetAperture (std::clamp (fov, 10.0f, 90.0f) * RAD * 0.5);
	static const float fovs[] = { 20.0f, 40.0f, 60.0f, 90.0f };
	static const char *fovl[] = { "20\xC2\xB0", "40\xC2\xB0", "60\xC2\xB0", "90\xC2\xB0" };
	int fi = -1;
	for (int i = 0; i < 4; i++) if (fabs (fov - fovs[i]) < 0.5f) fi = i;
	if (ui::Segmented ("##fovs", fovl, 4, &fi)) oapiCameraSetAperture (fovs[fi] * RAD * 0.5);

	// direction: a pad of arrows (held down), and back to the forward view
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	ui::SectionHeader ("Direction");
	double dt = td.SysDT;
	float b = ImGui::GetFrameHeight () * 1.15f, g = ui::Px (3);
	ImVec2 o = ImGui::GetCursorScreenPos ();
	struct { ImGuiDir dir; int cx, cy; const char *id; } pad[] = {
		{ImGuiDir_Up, 1, 0, "##up"}, {ImGuiDir_Left, 0, 1, "##left"}, {ImGuiDir_Right, 2, 1, "##right"}, {ImGuiDir_Down, 1, 2, "##down"}
	};
	for (auto &a : pad) {
		ImGui::SetCursorScreenPos (ImVec2(o.x + a.cx * (b + g), o.y + a.cy * (b + g)));
		ImGui::ArrowButtonEx (a.id, a.dir, ImVec2(b, b), ImGuiButtonFlags_PressedOnClick);
		if (!ImGui::IsItemActive ()) continue;
		switch (a.dir) {
		case ImGuiDir_Up:    if (rot_is_tilt) g_camera->Rotate (0,  dt); else g_camera->ShiftTheta (-dt); break;
		case ImGuiDir_Down:  if (rot_is_tilt) g_camera->Rotate (0, -dt); else g_camera->ShiftTheta ( dt); break;
		case ImGuiDir_Left:  if (rot_is_tilt) g_camera->Rotate ( dt, 0); else g_camera->ShiftPhi (-dt);   break;
		case ImGuiDir_Right: if (rot_is_tilt) g_camera->Rotate (-dt, 0); else g_camera->ShiftPhi ( dt);   break;
		default: break;
		}
	}
	// (the centre of the pad)
	ImGui::GetWindowDrawList ()->AddCircleFilled (ImVec2(o.x + 1.5f * b + g, o.y + 1.5f * b + g), ui::Px (3), ui::Col32 (ui::TEXT_FAINT));
	ImGui::SetCursorScreenPos (ImVec2(o.x + 3 * b + 2 * g + ui::Px (16), o.y));
	ImGui::BeginGroup ();
	ImGui::PushTextWrapPos (ImGui::GetContentRegionMax ().x);
	ui::TextDim (rot_is_tilt ? "Hold an arrow to turn the camera." : "Hold an arrow to move the camera around its target.");
	ImGui::PopTextWrapPos ();
	if (!extcam) {
		ImGui::Dummy (ImVec2(0, ui::Px (2)));
		if (ui::SecondaryButton (ICON_LC_ROTATE_CCW "  Look forward")) g_camera->ResetCockpitDir ();
	}
	ImGui::EndGroup ();
	ImGui::SetCursorScreenPos (ImVec2(o.x, o.y + 3 * b + 2 * g));
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
}

// ----------------------------------------------------------------------
// Target: celestial bodies, spaceports and vessels

Body *DlgCamera::SelectedBody() const {
	if (m_SelectedTarget.empty()) return nullptr;
	Body *obj = g_psys->GetObj (m_SelectedTarget.c_str(), true);
	if (!obj) obj = g_psys->GetBase (m_SelectedTarget.c_str(), true);
	return obj;
}

bool DlgCamera::TargetRow(const char *name, const char *icon, int depth) {
	Body *tgt = g_camera->Target ();
	bool cur = tgt && !strcmp (tgt->Name (), name);
	float ind = depth * ui::Px (16);
	if (ind > 0) ImGui::Indent (ind);
	bool act = false;
	if (ui::ListRow (name, cur ? ICON_LC_EYE : icon, name, cur ? "viewing" : nullptr, m_SelectedTarget == name, &act,
		cur ? ui::ACCENT : ui::TEXT_FAINT))
		m_SelectedTarget = name;
	if (ind > 0) ImGui::Unindent (ind);
	if (act) m_SelectedTarget = name;
	return act;
}

void DlgCamera::AddCbodyNode(const CelestialBody *cbody, int depth) {
	const char *icon = cbody->Type () == OBJTP_STAR ? ICON_LC_SUN : depth >= 2 ? ICON_LC_MOON : ICON_LC_GLOBE;
	if (TargetRow (cbody->Name (), icon, depth)) {
		Body *obj = SelectedBody ();
		if (obj) g_pOrbiter->SetView (obj, 1);
	}
	for (int i = 0; i < cbody->nSecondary(); i++)
		AddCbodyNode (cbody->Secondary(i), depth + 1);
}

void DlgCamera::DrawTarget() {
	ui::SearchField ("##filter", "Search bodies, spaceports and vessels", m_Filter, sizeof(m_Filter));
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	std::string flt = Lower (m_Filter);
	while (!flt.empty() && flt.back() == ' ') flt.pop_back();

	float bh = ImGui::GetFrameHeight () * 1.25f, sp = ImGui::GetStyle().ItemSpacing.y;
	if (ui::BeginPanel ("##targets", ImVec2(0, m_FillTarget.Height (2 * bh + 2 * sp)), 0.6f, 0, true)) {
		bool view = false;
		if (!flt.empty()) {
			// matches, in one list
			int n = 0;
			for (int i = 0; i < g_psys->nGrav(); i++) {
				Body *b = g_psys->GetGravObj (i);
				if (Lower (b->Name ()).find (flt) == std::string::npos) continue;
				view |= TargetRow (b->Name (), b->Type () == OBJTP_STAR ? ICON_LC_SUN : ICON_LC_GLOBE, 0); n++;
			}
			for (int i = 0; i < g_psys->nGrav(); i++) {
				Body *obj = g_psys->GetGravObj (i);
				if (obj->Type() != OBJTP_PLANET) continue;
				Planet *planet = (Planet*)obj;
				for (int j = 0; j < g_psys->nBase(planet); j++) {
					const char *name = g_psys->GetBase (planet, j)->Name();
					if (Lower (name).find (flt) == std::string::npos) continue;
					view |= TargetRow (name, ICON_LC_MAP_PIN, 0); n++;
				}
			}
			for (int i = 0; i < g_psys->nVessel(); i++) {
				const char *name = g_psys->GetVessel(i)->Name();
				if (Lower (name).find (flt) == std::string::npos) continue;
				view |= TargetRow (name, ICON_LC_ROCKET, 0); n++;
			}
			if (!n) ui::TextDim ("Nothing matches the search.");
			if (view) {
				Body *obj = SelectedBody ();
				if (obj) g_pOrbiter->SetView (obj, 1);
			}
		} else {
			ui::SectionHeader ("Celestial bodies");
			for (int i = 0; i < g_psys->nStar(); i++)
				AddCbodyNode (g_psys->GetStar(i), 0);
			bool any = false;
			for (int i = 0; i < g_psys->nGrav(); i++) {
				Body *obj = g_psys->GetGravObj (i);
				if (obj->Type() != OBJTP_PLANET) continue;
				Planet *planet = (Planet*)obj;
				if (!g_psys->nBase(planet)) continue;
				if (!any) {
					ImGui::Dummy (ImVec2(0, ui::Px (6)));
					ui::SectionHeader ("Spaceports");
					any = true;
				}
				for (int j = 0; j < g_psys->nBase(planet); j++)
					view |= TargetRow (g_psys->GetBase (planet, j)->Name(), ICON_LC_MAP_PIN, 0);
			}
			if (g_psys->nVessel()) {
				ImGui::Dummy (ImVec2(0, ui::Px (6)));
				ui::SectionHeader ("Vessels");
				for (int i = 0; i < g_psys->nVessel(); i++)
					view |= TargetRow (g_psys->GetVessel(i)->Name(), ICON_LC_ROCKET, 0);
			}
			if (view) {
				Body *obj = SelectedBody ();
				if (obj) g_pOrbiter->SetView (obj, 1);
			}
		}
	}
	ui::EndPanel ();
	m_FillTarget.PanelEnd ();

	// the actions on the chosen object
	Body *obj = SelectedBody ();
	Body *tgt = g_camera->Target ();
	float w = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x;
	bool other = obj && obj != tgt;
	ImGui::BeginDisabled (!other);
	if (ui::SecondaryButton ("Keep behind camera", ImVec2((w - gap) * 0.5f, bh)) && other)
		g_camera->SetTrackMode (CAMERA_TARGETFROMOBJECT, obj);
	if (other && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip ("Look at %s from the direction of %s\n(e.g. the Sun behind the camera: the lit side)", tgt ? tgt->Name () : "", obj->Name ());
	ImGui::SameLine (0, gap);
	if (ui::SecondaryButton ("Keep behind target", ImVec2(-FLT_MIN, bh)) && other)
		g_camera->SetTrackMode (CAMERA_TARGETTOOBJECT, obj);
	if (other && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip ("Look at %s towards %s\n(e.g. the Sun behind the target: the unlit side)", tgt ? tgt->Name () : "", obj->Name ());
	ImGui::EndDisabled ();
	ImGui::BeginDisabled (!obj || (obj == tgt && extcam));
	std::string lbl = std::string (ICON_LC_EYE "  View ") + (obj ? obj->Name () : "");
	if (ui::PrimaryButton (lbl.c_str(), ImVec2(-FLT_MIN, bh)) && obj)
		g_pOrbiter->SetView (obj, 1);
	ImGui::EndDisabled ();
	m_FillTarget.FooterEnd ();
}

// ----------------------------------------------------------------------
// Ground: an observer at a fixed position on a planet's surface

void DlgCamera::DrawGround() {
	float bh = ImGui::GetFrameHeight () * 1.25f, sp = ImGui::GetStyle().ItemSpacing.y;
	float form = 3 * ImGui::GetFrameHeight () + 4 * sp + ImGui::GetTextLineHeight () + ui::Px (30) + bh + ui::Px (8);
	float listh = std::max (ImGui::GetFrameHeight () * 3, ImGui::GetContentRegionAvail().y + m_FillGround.Height (form));
	if (ui::BeginPanel ("##sites", ImVec2(0, listh), 0.6f, 0, true)) {
		bool any = false;
		for (int i = 0; i < g_psys->nGrav(); i++) {
			Body *obj = g_psys->GetGravObj (i);
			if (obj->Type() != OBJTP_PLANET) continue;
			Planet *planet = (Planet*)obj;
			if (!planet->nGroundObserver()) continue;
			if (any) ImGui::Dummy (ImVec2(0, ui::Px (4)));
			ui::SectionHeader (planet->Name());
			any = true;
			for (int j = 0; j < planet->nGroundObserver(); j++) {
				const GROUNDOBSERVERSPEC *go = planet->GetGroundObserver (j);
				std::string name = std::string(go->site) + " \xC2\xB7 " + go->addr;
				bool act = false;
				if (ui::ListRow (name.c_str(), ICON_LC_MAP_PIN, name.c_str(), nullptr, m_SelectedSite == name && m_SitePlanet == planet->Name(), &act) || act) {
					m_SelectedSite = name;
					m_SitePlanet = planet->Name();
					snprintf (longitude, sizeof(longitude), "%+0.6f", go->lng * DEG);
					snprintf (latitude, sizeof(latitude), "%+0.6f", go->lat * DEG);
					snprintf (altitude, sizeof(altitude), "%0.4g", go->alt);
				}
				if (act) ApplyObserver ();
			}
		}
		if (!any) ui::TextDim ("No observer sites are defined. Use the camera's position below.");
	}
	ui::EndPanel ();
	m_FillGround.PanelEnd ();

	// the position
	ImGui::Dummy (ImVec2(0, ui::Px (4)));
	char hdr[96];
	snprintf (hdr, sizeof(hdr), "Position%s%s", m_SitePlanet.empty() ? "" : " \xC2\xB7 ", m_SitePlanet.c_str());
	ui::SectionHeader (hdr);
	float lw = ui::Px (64), gap = ui::Px (16);
	float fw = (ImGui::GetContentRegionAvail().x - 2 * lw - gap) * 0.5f;
	float x0 = ImGui::GetCursorPosX ();
	FormLabel ("Longitude", lw); UnitInput ("##lng", longitude, sizeof(longitude), "\xC2\xB0", fw);
	ImGui::SameLine (x0 + lw + fw + gap);
	FormLabel ("Latitude", lw); UnitInput ("##lat", latitude, sizeof(latitude), "\xC2\xB0", fw);
	FormLabel ("Altitude", lw); UnitInput ("##alt", altitude, sizeof(altitude), "m", fw);
	ImGui::SameLine (x0 + lw + fw + gap);
	FormLabel ("Terrain", lw); UnitInput ("##terrain", followterrain, sizeof(followterrain), "m", fw);
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort))
		ImGui::SetTooltip ("The camera stays at least this high above the ground");
	if (ui::Toggle ("Keep the target in view", &m_TargetLock))
		g_camera->SetGroundObserver_TargetLock (m_TargetLock);

	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	float w = ImGui::GetContentRegionAvail().x, bg = ImGui::GetStyle().ItemSpacing.x;
	if (ui::SecondaryButton (ICON_LC_LOCATE "  Camera position", ImVec2((w - bg) * 0.45f, bh))) {
		const Planet *planet = g_psys->GetPlanet (m_SitePlanet.c_str());
		if (!planet) {
			planet = g_camera->ProxyPlanet();
			if (planet) m_SitePlanet = planet->Name();
		}
		if (planet) {
			double lng, lat, alt;
			planet->GlobalToEquatorial (*g_camera->GPosPtr(), lng, lat, alt);
			alt -= planet->Size() + planet->Elevation (lng, lat);
			snprintf (longitude, sizeof(longitude), "%+0.6f", lng * DEG);
			snprintf (latitude, sizeof(latitude), "%+0.6f", lat * DEG);
			snprintf (altitude, sizeof(altitude), "%0.4g", alt);
			m_SelectedSite.clear ();
		}
		SetCurrentGroundpos();
	}
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort))
		ImGui::SetTooltip ("Fix the camera to the ground where it is now");
	ImGui::SameLine (0, bg);
	ImGui::BeginDisabled (m_SitePlanet.empty());
	if (ui::PrimaryButton (ICON_LC_MAP_PIN "  Go to position", ImVec2(-FLT_MIN, bh)))
		ApplyObserver();
	ImGui::EndDisabled ();
	m_FillGround.FooterEnd ();
}

void DlgCamera::ApplyObserver() {
	Planet *planet = g_psys->GetPlanet (m_SitePlanet.c_str());
	if (!planet) return;

	double lng, lat, alt = 1.7;
	bool ok;
	ok = (sscanf (longitude, "%lf", &lng) == 1);
	ok = ok && (sscanf (latitude, "%lf", &lat) == 1);
	ok = ok && (sscanf (altitude, "%lf", &alt) == 1);

	double altlimit;
	if (sscanf (followterrain, "%lf", &altlimit) == 1)
		g_camera->SetGroundObserver_TerrainLimit (std::max(1.0,altlimit));

	if (ok)
		g_camera->SetGroundMode (CAMERA_GROUNDOBSERVER, planet, lng*RAD, lat*RAD, alt, NULL);
}

void DlgCamera::SetCurrentGroundpos() {
	if (g_camera->GetExtMode() == CAMERA_GROUNDOBSERVER) return; // nothing to do
	ApplyObserver ();

	if (!m_TargetLock) {
		// force lock to target to initialise camera direction
		g_camera->SetGroundObserver_TargetLock (true);
		g_camera->SetGroundObserver_TargetLock (false);
	}
	double alt;
	if (sscanf (followterrain, "%lf", &alt) == 1)
		g_camera->SetGroundObserver_TerrainLimit (std::max(1.0,alt));
}

// ----------------------------------------------------------------------
// Presets: stored views

void DlgCamera::DrawPreset() {
	float bh = ImGui::GetFrameHeight () * 1.25f, sp = ImGui::GetStyle().ItemSpacing.y;
	int n = (int)g_camera->nPreset();
	if (m_SelectedPreset >= n) m_SelectedPreset = n - 1;
	if (ui::BeginPanel ("##presets", ImVec2(0, m_FillPreset.Height (bh + sp)), 0.6f, 0, true)) {
		for (int i = 0; i < n; i++) {
			char buf[256];
			g_camera->GetPreset(i)->GetDescr (buf, 256);
			char id[16], num[16];
			snprintf (id, sizeof(id), "##p%d", i);
			snprintf (num, sizeof(num), "%d", i + 1);
			bool act = false;
			if (ui::ListRow (id, ICON_LC_BOOKMARK, buf, num, m_SelectedPreset == i, &act)) m_SelectedPreset = i;
			if (act) {
				m_SelectedPreset = i;
				g_camera->RecallPreset (i);
			}
		}
		if (!n) {
			ImGui::Dummy (ImVec2(0, ui::Px (20)));
			ui::TextDim ("No preset views. Add the current view to come back to it later.");
		}
	}
	ui::EndPanel ();
	m_FillPreset.PanelEnd ();

	if (ui::SecondaryButton (ICON_LC_PLUS "  Add view", ImVec2(0, bh)))
		m_SelectedPreset = (int)g_camera->AddPreset();
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("Store the current view");
	ImGui::SameLine ();
	bool sel = m_SelectedPreset >= 0 && m_SelectedPreset < n;
	ImGui::BeginDisabled (!sel);
	if (ui::SecondaryButton (ICON_LC_TRASH_2 "##del", ImVec2(bh, bh)) && sel) g_camera->DelPreset (m_SelectedPreset);
	if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("Delete the preset");
	ImGui::EndDisabled ();
	ImGui::SameLine ();
	ImGui::BeginDisabled (!n);
	ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::WARNING));
	if (ui::SecondaryButton ("Clear all", ImVec2(0, bh))) {
		g_camera->ClearPresets();
		m_SelectedPreset = -1;
	}
	ImGui::PopStyleColor ();
	ImGui::EndDisabled ();
	ImGui::SameLine ();
	ImGui::BeginDisabled (!sel);
	if (ui::PrimaryButton (ICON_LC_PLAY "  Recall", ImVec2(-FLT_MIN, bh)) && sel)
		g_camera->RecallPreset (m_SelectedPreset);
	ImGui::EndDisabled ();
	m_FillPreset.FooterEnd ();
}
