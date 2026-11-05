// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Custom function selection dialog
// ======================================================================

#include "DlgFunction.h"
#include "Orbiter.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "imgui.h"
#include <cctype>
#include <string>

extern Orbiter *g_pOrbiter;

DlgFunction::DlgFunction() : ImGuiDialog(ICON_LC_PUZZLE "  Custom functions", {360, 320}) {
	SetHelp("html/orbiter.chm", "/customcmd.htm");
}

namespace {
	bool Contains (const char *s, const std::string &flt)
	{
		if (flt.empty()) return true;
		if (!s) return false;
		std::string t (s);
		for (auto &c : t) c = (char)tolower ((unsigned char)c);
		return t.find (flt) != std::string::npos;
	}
}

void DlgFunction::OnDraw() {
	int n = (int)g_pOrbiter->ncustomcmd;
	const ui::Fonts &f = ui::GetFonts();

	// none: say where they come from
	if (!n) {
		float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
		ImGui::Dummy (ImVec2(0, std::max (0.0f, h * 0.5f - ui::Px (60))));
		ui::PushFont (f.body, ui::SIZE_TITLE);
		ImVec2 is = ImGui::CalcTextSize (ICON_LC_PUZZLE);
		ImGui::SetCursorPosX (ImGui::GetCursorPosX () + (w - is.x) * 0.5f);
		ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_FAINT));
		ImGui::TextUnformatted (ICON_LC_PUZZLE);
		ImGui::PopStyleColor ();
		ui::PopFont ();
		ImGui::Dummy (ImVec2(0, ui::Px (4)));
		const char *t = "No custom functions";
		ImGui::SetCursorPosX (ImGui::GetCursorPosX () + (w - ImGui::CalcTextSize (t).x) * 0.5f);
		ImGui::TextUnformatted (t);
		ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + w);
		ui::TextDim ("Plugins add their functions here (a scenario editor, a flight data monitor, ...) "
			"when they are enabled on the launchpad.");
		ImGui::PopTextWrapPos ();
		return;
	}

	if (n > 6) {
		ui::SearchField ("##filter", "Search functions", m_Filter, sizeof(m_Filter));
		ImGui::Dummy (ImVec2(0, ui::Px (2)));
	}
	std::string flt (n > 6 ? m_Filter : "");
	for (auto &c : flt) c = (char)tolower ((unsigned char)c);

	// a row per function: its name, its description dimmed below; a click runs it
	float pad = ui::Px (10);
	float lh = ImGui::GetTextLineHeight ();
	ui::PushFont (f.body, ui::SIZE_SMALL);
	float sh = ImGui::GetTextLineHeight ();
	ui::PopFont ();
	float rh = 2 * pad + lh + ui::Px (2) + sh;
	bool any = false;
	for (int i = 0; i < n; i++) {
		const auto &cmd = g_pOrbiter->customcmd[i];
		if (!Contains (cmd.label, flt) && !Contains (cmd.desc, flt)) continue;
		any = true;
		ImGui::PushID (i);
		ImVec2 p = ImGui::GetCursorScreenPos ();
		float w = ImGui::GetContentRegionAvail().x;
		if (ImGui::InvisibleButton ("##fn", ImVec2(w, rh)))
			cmd.func (cmd.context);
		bool hov = ImGui::IsItemHovered (), act = ImGui::IsItemActive ();
		ImDrawList *dl = ImGui::GetWindowDrawList ();
		ImVec2 q (p.x + w, p.y + rh);
		dl->AddRectFilled (p, q, ui::Col32 (act ? ui::RAISED_ACTIVE : hov ? ui::RAISED_HOVER : ui::RAISED, hov || act ? 1.0f : 0.6f), ui::Px (6));
		float x = p.x + pad;
		dl->AddText (ImVec2(x, p.y + pad + (lh - ImGui::GetFontSize ()) * 0.5f), ui::Col32 (hov ? ui::ACCENT : ui::TEXT_DIM), ICON_LC_SQUARE_FUNCTION);
		x += ImGui::GetFontSize () * 1.7f;
		ImGui::PushClipRect (ImVec2(x, p.y), ImVec2(q.x - pad, q.y), true);
		ui::PushFont (f.medium, ImGui::GetStyle().FontSizeBase);
		dl->AddText (ImVec2(x, p.y + pad), ui::Col32 (ui::TEXT), cmd.label ? cmd.label : "");
		ui::PopFont ();
		if (cmd.desc && *cmd.desc) {
			ui::PushFont (f.body, ui::SIZE_SMALL);
			dl->AddText (ImVec2(x, p.y + pad + lh + ui::Px (2)), ui::Col32 (ui::TEXT_DIM), cmd.desc);
			ui::PopFont ();
		}
		ImGui::PopClipRect ();
		if (hov && cmd.desc && *cmd.desc && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayNormal)) {
			ImGui::BeginTooltip ();
			ImGui::PushTextWrapPos (ImGui::GetFontSize () * 30.0f);
			ImGui::TextUnformatted (cmd.desc);
			ImGui::PopTextWrapPos ();
			ImGui::EndTooltip ();
		}
		ImGui::PopID ();
	}
	if (!any) ui::TextDim ("No function matches the search.");
}
