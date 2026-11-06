// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "DlgFlightMenu.h"
#include "Orbiter.h"
#include "TimeData.h"
#include "DlgMgr.h"
#include "DlgOptions.h"
#include "DlgFocus.h"
#include "Psys.h"
#include "Vessel.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "imgui.h"
#include <cstring>
#include <string>

extern Orbiter *g_pOrbiter;
extern char ScenarioName[256];
extern TimeData td;
extern PlanetarySystem *g_psys;
extern Vessel *g_focusobj;
extern HELPCONTEXT DefHelpContext;

namespace {

	/// A row of the menu: icon, label and shortcut; true when chosen
	bool Entry (const char *icon, const char *label, const char *shortcut = nullptr, bool danger = false)
	{
		float w = ImGui::GetContentRegionAvail().x, h = ui::Px (40);
		ImGui::PushID (label);
		bool r = ImGui::InvisibleButton ("##entry", ImVec2(w, h));
		ui::ReportItem (label); // (its label for the playtest tool's widget list)
		bool hov = ImGui::IsItemHovered (), focus = ImGui::IsItemFocused ();
		ImGui::PopID ();
		ImVec2 p0 = ImGui::GetItemRectMin (), p1 = ImGui::GetItemRectMax ();
		ImDrawList *dl = ImGui::GetWindowDrawList ();
		if (hov || focus) dl->AddRectFilled (p0, p1, ui::Col32 (ui::TEXT, 0.07f), ui::Px (6));
		ui::Colour c = danger ? ui::WARNING : ui::TEXT;
		const ui::Fonts &f = ui::GetFonts ();
		ui::PushFont (f.medium, ui::SIZE_BODY + 1.0f);
		float th = ImGui::GetFontSize ();
		float y = (p0.y + p1.y - th) * 0.5f;
		dl->AddText (ImVec2(p0.x + ui::Px (12), y), danger ? ui::Col32 (c) : ui::Col32 (ui::TEXT_DIM), icon);
		dl->AddText (ImVec2(p0.x + ui::Px (44), y), ui::Col32 (c), label);
		ui::PopFont ();
		if (shortcut) {
			ui::PushFont (f.mono, ui::SIZE_SMALL);
			ImVec2 ss = ImGui::CalcTextSize (shortcut);
			dl->AddText (ImVec2(p1.x - ss.x - ui::Px (12), (p0.y + p1.y - ss.y) * 0.5f), ui::Col32 (ui::TEXT_FAINT), shortcut);
			ui::PopFont ();
		}
		return r;
	}

}

DlgFlightMenu::DlgFlightMenu (): ImGuiDialog ("Flight menu")
{
}

void DlgFlightMenu::FlightOver (const std::string &vessel, const std::string &reason)
{
	if (active) return; // (open already: as it is)
	Toggle ();
	over = true;
	overTitle = vessel + " destroyed";
	overReason = reason;
	if (!overReason.empty ()) overReason[0] = (char)toupper ((unsigned char)overReason[0]);
}

void DlgFlightMenu::Toggle ()
{
	if (active) {
		Close ();
		return;
	}
	over = false;
	active = true;
	saving = false;
	openFrame = ImGui::GetFrameCount ();
	wasRunning = g_pOrbiter->IsRunning ();
	if (wasRunning) g_pOrbiter->Pause (true);
}

void DlgFlightMenu::Close ()
{
	if (!active) return;
	active = false;
	OnClose ();
}

void DlgFlightMenu::OnClose ()
{
	if (wasRunning && !g_pOrbiter->IsRunning ()) g_pOrbiter->Pause (false);
	wasRunning = false;
}

void DlgFlightMenu::Display ()
{
	if (!active) return;
	const ImGuiViewport *vp = ImGui::GetMainViewport ();
	const ui::Fonts &f = ui::GetFonts ();

	// the view and the dialogs dimmed behind the menu
	ui::ModalDim ("##flightdim", 0.8f);

	ImGui::SetNextWindowPos (ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize (ImVec2(ui::Px (380), 0));
	if (ImGui::GetFrameCount () <= openFrame + 1) ImGui::SetNextWindowFocus ();
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(ui::Px (16), ui::Px (18)));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, ui::Px (12));
	ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2(ui::Px (8), ui::Px (2)));
	ImGui::SetNextWindowBgAlpha (1.0f); // (opaque: the blend is linear, a little would show the instruments)
	bool open = ImGui::Begin ("##flightmenu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar (3);
	ui::BringToFront ();
	if (!open) {
		ImGui::End ();
		return;
	}

	// heading: paused (or the flight over), the scenario
	ImGui::SetCursorPosX (ImGui::GetCursorPosX () + ui::Px (12));
	ui::PushFont (f.medium, ui::SIZE_SMALL);
	ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (over ? ui::WARNING : ui::CAUTION));
	ImGui::TextUnformatted (over ? ICON_LC_TRIANGLE_ALERT "  F L I G H T   O V E R" : ICON_LC_PAUSE "  P A U S E D");
	ImGui::PopStyleColor ();
	ui::PopFont ();
	const char *scn = ScenarioName;
	for (const char *c = ScenarioName; *c; c++) if (*c == '\\' || *c == '/') scn = c + 1;
	ImGui::SetCursorPosX (ImGui::GetCursorPosX () + ui::Px (12));
	ui::PushFont (f.strong, ui::SIZE_LARGE + 2.0f);
	ImGui::TextUnformatted (over ? overTitle.c_str() : *scn ? scn : "Orbiter");
	ui::PopFont ();
	if (over && !overReason.empty ()) {
		ImGui::SetCursorPosX (ImGui::GetCursorPosX () + ui::Px (12));
		ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_DIM));
		ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + ImGui::GetContentRegionAvail().x - ui::Px (12));
		ImGui::TextUnformatted (overReason.c_str());
		ImGui::PopTextWrapPos ();
		ImGui::PopStyleColor ();
	}
	ImGui::Dummy (ImVec2(0, ui::Px (10)));

	bool close = false;
	// (what next: the scenario again, the latest quicksave, another vessel)
	std::string quick = g_pOrbiter->LatestQuicksave ();
	bool other = false;
	for (DWORD i = 0; i < g_psys->nVessel () && !other; i++) {
		const Vessel *v = g_psys->GetVessel (i);
		if (v != g_focusobj && v->GetEnableFocus () && !v->Destroyed ()) other = true;
	}
	if (over) {
		if (!g_pOrbiter->RootScenario ().empty () && Entry (ICON_LC_ROTATE_CCW, "Restart scenario")) {
			close = true;
			g_pOrbiter->RequestRelaunch (g_pOrbiter->RootScenario ()); // (the one chosen, not a quicksave loaded since)
		}
		if (!quick.empty () && Entry (ICON_LC_HISTORY, "Load latest quicksave", quick.c_str() + 10)) {
			close = true;
			g_pOrbiter->RequestRelaunch (quick, true);
		}
		if (other && Entry (ICON_LC_ARROW_LEFT_RIGHT, "Switch vessel", "F3")) {
			close = true;
			g_pOrbiter->DlgMgr ()->EnsureEntry<DlgFocus> ();
		}
		if (Entry (ICON_LC_EYE, "Keep watching", "Esc")) close = true;
	} else if (Entry (ICON_LC_PLAY, "Resume", "Esc")) close = true;
	if (!saving) {
		if (Entry (ICON_LC_SAVE, "Save scenario\xE2\x80\xA6")) {
			saving = true;
			snprintf (saveName, sizeof(saveName), "%s", scn);
		}
	} else {
		// the name, saved in the scenario directory
		ImGui::Dummy (ImVec2(0, ui::Px (4)));
		ImGui::SetCursorPosX (ImGui::GetCursorPosX () + ui::Px (12));
		ImGui::SetNextItemWidth (ImGui::GetContentRegionAvail().x - ui::Px (90));
		if (ImGui::IsWindowAppearing () || ImGui::GetFrameCount () <= openFrame + 2) ImGui::SetKeyboardFocusHere ();
		bool enter = ImGui::InputTextWithHint ("##savename", "Scenario name", saveName, sizeof(saveName), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine ();
		if ((ui::PrimaryButton ("Save", ImVec2(-FLT_MIN, 0)) || enter) && saveName[0]) {
			char desc[256];
			snprintf (desc, sizeof(desc), "Orbiter saved state at T = %0.0f", td.SimT0);
			if (g_pOrbiter->SaveScenario (saveName, desc, 0))
				oapiAddNotification (OAPINOTIF_SUCCESS, "Scenario saved", saveName);
			else
				oapiAddNotification (OAPINOTIF_ERROR, "Could not save the scenario", saveName);
			saving = false;
		}
		ImGui::Dummy (ImVec2(0, ui::Px (4)));
	}
	if (Entry (ICON_LC_DOWNLOAD, "Quick save", "Ctrl+S")) {
		g_pOrbiter->Quicksave ();
		close = true;
	}
	if (Entry (ICON_LC_SETTINGS, "Settings", "F6")) {
		g_pOrbiter->DlgMgr ()->EnsureEntry<DlgOptions> ();
		close = true;
	}
	if (Entry (ICON_LC_KEYBOARD, "Keyboard shortcuts", "Alt+F1")) {
		g_pOrbiter->OpenKeyHelp ();
		close = true;
	}
#ifdef _WIN32
	if (Entry (ICON_LC_CIRCLE_HELP, "User manual")) {
		DefHelpContext.topic = (char*)"/mainmenu.htm";
		g_pOrbiter->OpenHelp (&DefHelpContext);
		close = true;
	}
#endif
	if (!over) {
		if (!g_pOrbiter->RootScenario ().empty () && Entry (ICON_LC_ROTATE_CCW, "Restart scenario")) {
			close = true;
			g_pOrbiter->RequestRelaunch (g_pOrbiter->RootScenario ()); // (the one chosen, not a quicksave loaded since)
		}
		if (!quick.empty () && Entry (ICON_LC_HISTORY, "Load latest quicksave", quick.c_str() + 10)) {
			close = true;
			g_pOrbiter->RequestRelaunch (quick, true);
		}
	}
	ImGui::Dummy (ImVec2(0, ui::Px (6)));
	ImVec2 p = ImGui::GetCursorScreenPos ();
	ImGui::GetWindowDrawList ()->AddLine (ImVec2(p.x + ui::Px (12), p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x - ui::Px (12), p.y),
		ui::Col32 (ui::DIVIDER), std::max (1.0f, ui::Scale ()));
	ImGui::Dummy (ImVec2(0, ui::Px (6)));
#ifdef _WIN32
	if (Entry (ICON_LC_LOG_OUT, "Quit to launchpad", "Ctrl+Q")) {
		close = true;
		PostMessage (g_pOrbiter->GetRenderWnd (), WM_CLOSE, 0, 0);
	}
#else
	if (Entry (ICON_LC_LOG_OUT, "Quit to launchpad")) {
		close = true;
		g_pOrbiter->PlatformCloseRequest ();
	}
	if (Entry (ICON_LC_POWER, "Exit Orbiter", nullptr, true)) {
		close = true;
		g_pOrbiter->RequestExit ();
	}
#endif

	// Esc (not the press that opened the menu) resumes
	if (ImGui::GetFrameCount () > openFrame + 1 && !ImGui::GetIO ().WantTextInput && ImGui::IsKeyPressed (ImGuiKey_Escape, false))
		close = true;
	if (saving && ImGui::GetIO ().WantTextInput && ImGui::IsKeyPressed (ImGuiKey_Escape, false)) saving = false;
	ImGui::End ();
	if (close) Close ();
}
