// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Focus vessel selection dialog: the spacecraft that can take the focus,
// all of them, those near the camera, or grouped by the body they orbit
// or by their class; a search field filters them by name or class.
// ======================================================================

#include "DlgFocus.h"
#include "Psys.h"
#include "OrbiterAPI.h"
#include "Orbiter.h"
#include "Camera.h"
#include "Astro.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <vector>

extern Camera *g_camera;
extern PlanetarySystem *g_psys;
extern Orbiter *g_pOrbiter;
extern Vessel *g_focusobj, *g_pfocusobj;

namespace {

	bool Contains (const char *s, const char *lowerNeedle)
	{
		if (!*lowerNeedle) return true;
		std::string h (s);
		for (auto &c : h) c = (char)tolower ((unsigned char)c);
		return h.find (lowerNeedle) != std::string::npos;
	}

	/// A distance with its unit ("850 m", "12.3 km", "1.52 AU")
	std::string Distance (double d)
	{
		const char *s = DistStr (d, 3);
		while (*s == ' ') s++;
		std::string r (s);
		size_t u = r.find_first_not_of ("0123456789.-");
		if (u == std::string::npos) return r + " m";
		if (r.compare (u, 2, "AU") == 0) return r.substr (0, u) + " AU";
		return r.substr (0, u) + " " + r.substr (u) + "m";
	}

	struct Entry {
		Vessel *v;
		double dist;      // from the camera [m]
		std::string group;
	};

}

DlgFocus::DlgFocus() : ImGuiDialog(ICON_LC_ROCKET "  Select spacecraft", {380, 460}) {
	SetHelp("html/orbiter.chm", "/selvessel.htm");
}

void DlgFocus::SetFocus(const std::string &name) {
	Vessel *vessel = g_psys->GetVessel (name.c_str(), true);
	if (vessel && vessel != g_focusobj) g_pOrbiter->SetFocusObject (vessel);
}

void DlgFocus::OnDraw() {
	if (m_SelectedShip.empty() && g_focusobj) m_SelectedShip = g_focusobj->Name();

	ui::SearchField ("##filter", "Search by name or class", m_Filter, sizeof(m_Filter));
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	static const char *modes[] = { "All", "Nearby", "By body", "By class" };
	ui::Segmented ("##mode", modes, IM_ARRAYSIZE(modes), &m_Mode);
	if (m_Mode == NEARBY) {
		ImGui::Dummy (ImVec2(0, ui::Px (2)));
		ImGui::AlignTextToFramePadding ();
		ui::TextDim ("Within");
		ImGui::SameLine ();
		ImGui::SetNextItemWidth (-FLT_MIN);
		ImGui::SliderFloat ("##range", &m_Range, 1.0f, 1e6f, "%.0f km", ImGuiSliderFlags_Logarithmic);
	}
	ImGui::Dummy (ImVec2(0, ui::Px (4)));

	float bh0 = ImGui::GetFrameHeight () * 1.25f;
	if (ui::BeginPanel ("##vessels", ImVec2(0, m_Fill.Height (bh0 + ImGui::GetStyle().ItemSpacing.y)), 0.6f, 0, true)) DrawList ();
	ui::EndPanel ();
	m_Fill.PanelEnd ();

	// the actions: back to the previous focus; switch to the chosen vessel
	float bh = ImGui::GetFrameHeight () * 1.25f, gap = ImGui::GetStyle().ItemSpacing.x;
	float w = ImGui::GetContentRegionAvail().x;
	bool hasPrev = g_pfocusobj && g_pfocusobj != g_focusobj;
	ImGui::BeginDisabled (!hasPrev);
	if (ui::SecondaryButton (ICON_LC_HISTORY "  Previous", ImVec2((w - gap) * 0.4f, bh)) && hasPrev) {
		m_SelectedShip = g_pfocusobj->Name();
		g_pOrbiter->SetFocusObject (g_pfocusobj);
	}
	ImGui::EndDisabled ();
	if (hasPrev && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip ("Back to %s", g_pfocusobj->Name());
	ImGui::SameLine (0, gap);
	bool isFocus = g_focusobj && m_SelectedShip == g_focusobj->Name();
	ImGui::BeginDisabled (isFocus || m_SelectedShip.empty());
	if (ui::PrimaryButton (isFocus ? ICON_LC_CROSSHAIR "  In focus" : ICON_LC_CROSSHAIR "  Switch focus", ImVec2(-FLT_MIN, bh)))
		SetFocus (m_SelectedShip);
	ImGui::EndDisabled ();
	m_Fill.FooterEnd ();
}

void DlgFocus::DrawList() {
	std::string flt (m_Filter);
	for (auto &c : flt) c = (char)tolower ((unsigned char)c);
	while (!flt.empty() && flt.back() == ' ') flt.pop_back();

	const Vector &campos = g_camera->GPos();
	std::vector<Entry> list;
	for (int i = 0; i < g_psys->nVessel(); i++) {
		Vessel *v = g_psys->GetVessel(i);
		if (!v->GetEnableFocus()) continue;
		if (!Contains (v->Name(), flt.c_str()) && !Contains (v->ClassName() ? v->ClassName() : "", flt.c_str())) continue;
		double d = campos.dist (v->GPos());
		if (m_Mode == NEARBY && d > m_Range * 1000.0) continue;
		std::string g;
		if (m_Mode == BY_BODY) g = v->ElRef() ? v->ElRef()->Name() : "";
		else if (m_Mode == BY_CLASS) g = v->ClassName() ? v->ClassName() : "(no class)";
		list.push_back ({v, d, g});
	}
	if (m_Mode == NEARBY)
		std::stable_sort (list.begin(), list.end(), [](const Entry &a, const Entry &b) { return a.dist < b.dist; });
	else if (m_Mode != ALL)
		std::stable_sort (list.begin(), list.end(), [](const Entry &a, const Entry &b) { return a.group < b.group; });

	if (list.empty()) {
		ImGui::Dummy (ImVec2(0, ui::Px (24)));
		const char *msg = !flt.empty() ? "No spacecraft match the search" :
			m_Mode == NEARBY ? "No spacecraft within range" : "No spacecraft can take the focus";
		float tw = ImGui::CalcTextSize (msg).x;
		ImGui::SetCursorPosX (std::max (ImGui::GetCursorPosX (), (ImGui::GetWindowWidth () - tw) * 0.5f));
		ui::TextDim ("%s", msg);
		return;
	}

	std::string group = "\x01";
	for (size_t i = 0; i < list.size(); i++) {
		const Entry &e = list[i];
		if (m_Mode == BY_BODY || m_Mode == BY_CLASS) {
			if (e.group != group) {
				group = e.group;
				int n = 0;
				for (size_t j = i; j < list.size() && list[j].group == group; j++) n++;
				if (i) ImGui::Dummy (ImVec2(0, ui::Px (4)));
				char hdr[160];
				snprintf (hdr, sizeof(hdr), "%s  \xC2\xB7  %d", group.c_str(), n);
				ui::SectionHeader (hdr);
			}
		}
		bool focus = (e.v == g_focusobj);
		std::string detail;
		if (m_Mode == NEARBY || m_Mode == BY_CLASS) detail = focus && e.dist < e.v->Size() * 10 ? "" : Distance (e.dist);
		else detail = e.v->ClassName() ? e.v->ClassName() : "";
		bool act = false;
		if (ui::ListRow (e.v->Name(), focus ? ICON_LC_CROSSHAIR : ICON_LC_ROCKET, e.v->Name(), detail.c_str(),
			m_SelectedShip == e.v->Name(), &act, focus ? ui::ACCENT : ui::TEXT_FAINT))
			m_SelectedShip = e.v->Name();
		if (focus && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip ("The focus vessel");
		if (act) {
			m_SelectedShip = e.v->Name();
			SetFocus (m_SelectedShip);
		}
	}
}
