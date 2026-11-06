// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "DlgKeyHelp.h"
#include "Orbiter.h"
#include "Keymap.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "imgui.h"
#include "Vessel.h"
#include "Config.h"
#include "Platform.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

extern Orbiter *g_pOrbiter;
extern HELPCONTEXT DefHelpContext;
extern Vessel *g_focusobj;

namespace {

	constexpr int FIXED = -1; // (a key not in the key map)

	struct Row {
		const char *label;
		int keys[4];          // logical keys (OAPI_LKEY_*), -2: end
		const char *fixed;    // FIXED: the key's label
	};
	struct Group {
		const char *title;
		const char *icon;
		std::vector<Row> rows;
	};

	#define K1(a) {a, -2, -2, -2}
	#define K2(a, b) {a, b, -2, -2}
	#define K4(a, b, c, d) {a, b, c, d}

	const std::vector<Group> &Groups ()
	{
		static const std::vector<Group> g = {
			{"General", ICON_LC_COMMAND, {
				{"Flight menu", K1(FIXED), "Esc"},
				{"Pause", K1(OAPI_LKEY_Pause)},
				{"Quick save", K1(OAPI_LKEY_Quicksave)},
				{"Settings", K1(OAPI_LKEY_DlgOptions)},
				{"Keyboard shortcuts", K1(OAPI_LKEY_DlgHelp)},
				{"Menu bar", K1(OAPI_LKEY_MainMenu)},
				{"Screen capture", K1(OAPI_LKEY_DlgCapture)},
				{"Quit to the launchpad", K1(OAPI_LKEY_Quit)},
			}},
			{"Time", ICON_LC_CLOCK, {
				{"Faster / slower", K2(OAPI_LKEY_IncSimSpeed, OAPI_LKEY_DecSimSpeed)},
				{"Time acceleration", K1(OAPI_LKEY_DlgSimspeed)},
				{"Instant replay", K1(OAPI_LKEY_Replay)},
				{"Record / play back", K1(OAPI_LKEY_ToggleRecPlay)},
				{"Flight recorder", K1(OAPI_LKEY_DlgRecorder)},
			}},
			{"Spacecraft", ICON_LC_ROCKET, {
				{"Select spacecraft", K1(OAPI_LKEY_DlgSelectVessel)},
				{"Previous spacecraft", K1(OAPI_LKEY_SelectPrevVessel)},
				{"Undock", K1(OAPI_LKEY_Undock)},
				{"Object information", K1(OAPI_LKEY_DlgInfo)},
				{"Custom functions", K1(OAPI_LKEY_DlgCustomCmd)},
			}},
			{"View", ICON_LC_EYE, {
				{"Cockpit / external", K1(OAPI_LKEY_ToggleCamInternal)},
				{"Camera mode (Shift: back)", K1(OAPI_LKEY_ToggleTrackMode)},
				{"Cockpit mode (glass, panel, 3D)", K1(OAPI_LKEY_TogglePanelMode)},
				{"Camera", K1(OAPI_LKEY_DlgCamera)},
				{"Map", K1(OAPI_LKEY_MapView)},
				{"Map dialog", K1(OAPI_LKEY_DlgMap)},
				{"Zoom in / out", K2(OAPI_LKEY_DecFOV, OAPI_LKEY_IncFOV)},
				{"Zoom in / out in steps", K2(OAPI_LKEY_StepDecFOV, OAPI_LKEY_StepIncFOV)},
				{"Orbit the target", K4(OAPI_LKEY_TrackRotateLeft, OAPI_LKEY_TrackRotateRight, OAPI_LKEY_TrackRotateUp, OAPI_LKEY_TrackRotateDown)},
				{"Closer / farther", K2(OAPI_LKEY_TrackAdvance, OAPI_LKEY_TrackRetreat)},
				{"Look around (cockpit)", K4(OAPI_LKEY_CockpitRotateLeft, OAPI_LKEY_CockpitRotateRight, OAPI_LKEY_CockpitRotateUp, OAPI_LKEY_CockpitRotateDown)},
				{"Look forward / camera behind", K1(OAPI_LKEY_CockpitResetCam)},
				{"Celestial markers", K1(OAPI_LKEY_TogglePlanetarium)},
				{"Surface labels", K1(OAPI_LKEY_ToggleLabels)},
				{"Visual helpers", K1(OAPI_LKEY_DlgVisHelper)},
			}},
			{"Head-up display", ICON_LC_SCAN, {
				{"Show / hide", K1(OAPI_LKEY_HUD)},
				{"Mode", K1(OAPI_LKEY_HUDMode)},
				{"Reference", K1(OAPI_LKEY_HUDReference)},
				{"Target", K1(OAPI_LKEY_HUDTarget)},
				{"Colour", K1(OAPI_LKEY_HUDColour)},
			}},
			{"Engines", ICON_LC_FLAME, {
				{"Main thrust up / down", K2(OAPI_LKEY_IncMainThrust, OAPI_LKEY_DecMainThrust)},
				{"Full main (held)", K1(OAPI_LKEY_FullMainThrust)},
				{"Full retro (held)", K1(OAPI_LKEY_FullRetroThrust)},
				{"Main and retro off", K1(OAPI_LKEY_KillMainRetro)},
				{"Hover thrust up / down", K2(OAPI_LKEY_IncHoverThrust, OAPI_LKEY_DecHoverThrust)},
				{"Elevator trim", K2(OAPI_LKEY_IncElevatorTrim, OAPI_LKEY_DecElevatorTrim)},
				{"Wheel brakes", K2(OAPI_LKEY_WheelbrakeLeft, OAPI_LKEY_WheelbrakeRight)},
			}},
			{"Attitude thrusters", ICON_LC_ROTATE_3D, {
				{"On / off", K1(OAPI_LKEY_RCSEnable)},
				{"Rotation / translation", K1(OAPI_LKEY_RCSMode)},
				{"Pitch", K2(OAPI_LKEY_RCSPitchUp, OAPI_LKEY_RCSPitchDown)},
				{"Yaw", K2(OAPI_LKEY_RCSYawLeft, OAPI_LKEY_RCSYawRight)},
				{"Bank", K2(OAPI_LKEY_RCSBankLeft, OAPI_LKEY_RCSBankRight)},
				{"Forward / back", K2(OAPI_LKEY_RCSForward, OAPI_LKEY_RCSBack)},
				{"Fine control", K1(FIXED), "Ctrl + key"},
			}},
			{"Autopilots", ICON_LC_NAVIGATION, {
				{"Kill rotation", K1(OAPI_LKEY_NMKillrot)},
				{"Prograde / retrograde", K2(OAPI_LKEY_NMPrograde, OAPI_LKEY_NMRetrograde)},
				{"Normal / antinormal", K2(OAPI_LKEY_NMNormal, OAPI_LKEY_NMAntinormal)},
				{"Level with the horizon", K1(OAPI_LKEY_NMHLevel)},
				{"Hold altitude", K1(OAPI_LKEY_NMHoldAltitude)},
			}},
		};
		return g;
	}

	std::string Lower (const char *s)
	{
		std::string r (s);
		for (auto &c : r) c = (char)tolower ((unsigned char)c);
		return r;
	}

	/// The keys of a row: "Ctrl+←", "Num 8 / Num 2"; arrows of one
	/// modifier are joined: "Ctrl+←→↑↓"
	std::string Keys (const Row &r)
	{
		if (r.keys[0] == FIXED) return r.fixed;
		const Keymap &km = g_pOrbiter->keymap;
		std::vector<std::string> k;
		for (int i = 0; i < 4 && r.keys[i] >= 0; i++) {
			std::string l = km.Label (r.keys[i]);
			if (!l.empty()) k.push_back (l);
		}
		if (k.size() == 4) { // (four directions: one modifier, the arrows)
			std::string mod = k[0].substr (0, k[0].find_last_of ('+') + 1);
			bool same = true;
			for (auto &s : k) same &= s.compare (0, mod.size(), mod) == 0 && s.size() - mod.size() == 3;
			if (same) return mod + k[0].substr (mod.size()) + k[1].substr (mod.size()) + k[2].substr (mod.size()) + k[3].substr (mod.size());
		}
		std::string s;
		for (size_t i = 0; i < k.size(); i++) s += (i ? "  " : "") + k[i];
		return s;
	}

	/// A key cap
	void Cap (ImDrawList *dl, ImVec2 p, const char *text, const char *end)
	{
		ImVec2 ts = ImGui::CalcTextSize (text, end);
		ImVec2 pad (ui::Px (6), ui::Px (2));
		dl->AddRectFilled (p, ImVec2(p.x + ts.x + 2 * pad.x, p.y + ts.y + 2 * pad.y), ui::Col32 (ui::RAISED), ui::Px (4));
		dl->AddRect (p, ImVec2(p.x + ts.x + 2 * pad.x, p.y + ts.y + 2 * pad.y), ui::Col32 (ui::LINE), ui::Px (4));
		dl->AddText (ImVec2(p.x + pad.x, p.y + pad.y), ui::Col32 (ui::TEXT), text, end);
	}

	float CapsWidth (const std::string &keys)
	{
		float w = 0, pad = ui::Px (6), gap = ui::Px (4);
		size_t a = 0;
		while (a < keys.size()) {
			size_t b = keys.find ("  ", a);
			if (b == std::string::npos) b = keys.size();
			w += ImGui::CalcTextSize (keys.c_str() + a, keys.c_str() + b).x + 2 * pad + gap;
			a = b + 2;
		}
		return w - gap;
	}

	/// The focus vessel's own keys: Config/Vessels/<class>.keys, lines
	/// "<label> | <keys>" (keys apart by two spaces; ';' a comment)
	const std::vector<std::pair<std::string,std::string>> &VesselKeys (const char *classname)
	{
		static std::string cls;
		static std::vector<std::pair<std::string,std::string>> rows;
		if (!classname) classname = "";
		if (cls == classname) return rows;
		cls = classname;
		rows.clear ();
		if (!*classname) return rows;
		std::string name = std::string ("Vessels/") + classname;
		char path[512];
		snprintf (path, sizeof (path), "%s", g_pOrbiter->Cfg()->ConfigPath (name.c_str()));
		std::string sp (path);
		if (sp.size() > 4 && sp.compare (sp.size() - 4, 4, ".cfg") == 0) sp.resize (sp.size() - 4);
		std::ifstream ifs (orbiter::platform::ResolvePath ((sp + ".keys").c_str())); // (the file's case as it is)
		std::string line;
		auto trim = [](std::string t) {
			size_t a = t.find_first_not_of (" \t\r"), b = t.find_last_not_of (" \t\r");
			return a == std::string::npos ? std::string () : t.substr (a, b - a + 1);
		};
		while (std::getline (ifs, line)) {
			size_t c = line.find (';');
			if (c != std::string::npos) line.erase (c);
			size_t bar = line.find ('|');
			if (bar == std::string::npos) continue;
			std::string label = trim (line.substr (0, bar)), keys = trim (line.substr (bar + 1));
			if (!label.empty () && !keys.empty ()) rows.push_back ({label, keys});
		}
		return rows;
	}

	/// A row: the action, and its keys as caps at the right
	void DrawRow (const char *label, const std::string &keys, float width)
	{
		const ui::Fonts &f = ui::GetFonts ();
		ImVec2 p = ImGui::GetCursorScreenPos ();
		float h = ImGui::GetTextLineHeight () + ui::Px (7);
		ImDrawList *dl = ImGui::GetWindowDrawList ();
		dl->AddText (ImVec2(p.x, p.y + (h - ImGui::GetTextLineHeight ()) * 0.5f), ui::Col32 (ui::TEXT, 0.9f), label);
		ui::PushFont (f.mono, ui::SIZE_SMALL);
		float x = p.x + width - CapsWidth (keys);
		float ch = ImGui::GetTextLineHeight () + ui::Px (4);
		size_t a = 0;
		while (a < keys.size()) {
			size_t b = keys.find ("  ", a);
			if (b == std::string::npos) b = keys.size();
			Cap (dl, ImVec2(x, p.y + (h - ch) * 0.5f), keys.c_str() + a, keys.c_str() + b);
			x += ImGui::CalcTextSize (keys.c_str() + a, keys.c_str() + b).x + ui::Px (12) + ui::Px (4);
			a = b + 2;
		}
		ui::PopFont ();
		ImGui::Dummy (ImVec2(width, h));
	}

}

DlgKeyHelp::DlgKeyHelp (): ImGuiDialog ("Keyboard shortcuts")
{
}

void DlgKeyHelp::Toggle ()
{
	if (active) {
		Close ();
		return;
	}
	active = true;
	openFrame = ImGui::GetFrameCount ();
	filter[0] = '\0';
}

void DlgKeyHelp::Close ()
{
	active = false;
}

void DlgKeyHelp::Display ()
{
	if (!active) return;
	const ImGuiViewport *vp = ImGui::GetMainViewport ();
	const ui::Fonts &f = ui::GetFonts ();

	bool close = ui::ModalDim ("##keyhelpdim", 0.6f); // (a click outside closes the sheet)
	float w = std::min (vp->Size.x - ui::Px (80), ui::Px (1080));
	float h = std::min (vp->Size.y - ui::Px (80), ui::Px (900));
	ImGui::SetNextWindowPos (ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize (ImVec2(w, h), ImGuiCond_Always);
	if (ImGui::GetFrameCount () <= openFrame + 1) ImGui::SetNextWindowFocus ();
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(ui::Px (24), ui::Px (20)));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, ui::Px (12));
	ImGui::SetNextWindowBgAlpha (1.0f); // (opaque: the blend is linear, other windows would show)
	bool open = ImGui::Begin ("##keyhelp", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar (2);
	ui::BringToFront ();
	if (!open) {
		ImGui::End ();
		return;
	}

	// title, search, close
	ui::PushFont (f.strong, ui::SIZE_TITLE - 2.0f);
	ImGui::TextUnformatted (ICON_LC_KEYBOARD "  Keyboard shortcuts");
	ui::PopFont ();
	float right = ImGui::GetWindowContentRegionMax ().x;
	ImGui::SameLine (right - ui::Px (300));
	ImGui::SetCursorPosY (ImGui::GetCursorPosY () + ui::Px (2));
	ImGui::BeginGroup ();
	ImGui::PushItemWidth (ui::Px (300) - ImGui::GetFrameHeight () - ui::Px (8));
	ImGui::BeginChild ("##search", ImVec2(ui::Px (300) - ImGui::GetFrameHeight () - ui::Px (8), ImGui::GetFrameHeight () + ui::Px (4)), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
	if (ImGui::GetFrameCount () <= openFrame + 2) ImGui::SetKeyboardFocusHere ();
	ui::SearchField ("##filter", "Search", filter, sizeof(filter));
	ImGui::EndChild ();
	ImGui::PopItemWidth ();
	ImGui::SameLine (0, ui::Px (8));
	close |= ui::IconButton (ICON_LC_X, "Close (Esc)");
	ImGui::EndGroup ();
	ImGui::Dummy (ImVec2(0, ui::Px (6)));

	// the groups in three columns (each group whole, the shortest column next)
	std::string flt = Lower (filter);
	while (!flt.empty() && flt.back() == ' ') flt.pop_back();
	const auto &groups = Groups ();
	const int ncol = w > ui::Px (900) ? 3 : 2;
	float footer = ImGui::GetTextLineHeightWithSpacing () + ui::Px (12);
	ImGui::BeginChild ("##body", ImVec2(0, -footer), 0);
	float gap = ui::Px (28);
	float cw = (ImGui::GetContentRegionAvail().x - gap * (ncol - 1)) / ncol;
	ImVec2 origin = ImGui::GetCursorScreenPos ();
	std::vector<float> colY (ncol, origin.y);
	int shown = 0;
	// (the focus vessel's own keys first, then Orbiter's)
	struct Shown { std::string title; const char *icon; std::vector<std::pair<std::string,std::string>> rows; };
	std::vector<Shown> all;
	if (g_focusobj && !g_focusobj->Destroyed ()) {
		const auto &vk = VesselKeys (g_focusobj->ClassName ());
		if (!vk.empty ()) all.push_back ({std::string (g_focusobj->ClassName ()) + " (this vessel)", ICON_LC_ROCKET, vk});
	}
	for (auto &g : groups) {
		Shown sh{g.title, g.icon, {}};
		for (auto &r : g.rows) {
			std::string k = Keys (r);
			if (!k.empty()) sh.rows.push_back ({r.label, k});
		}
		all.push_back (std::move (sh));
	}
	for (auto &g : all) {
		std::vector<std::pair<std::string, std::string>> rows;
		bool titleMatch = !flt.empty() && Lower (g.title.c_str()).find (flt) != std::string::npos;
		for (auto &r : g.rows)
			if (flt.empty() || titleMatch || Lower (r.first.c_str()).find (flt) != std::string::npos || Lower (r.second.c_str()).find (flt) != std::string::npos)
				rows.push_back (r);
		if (rows.empty()) continue;
		int c = (int)(std::min_element (colY.begin(), colY.end()) - colY.begin());
		ImGui::SetCursorScreenPos (ImVec2(origin.x + c * (cw + gap), colY[c]));
		ImGui::BeginGroup ();
		{
			// the heading: icon, spaced capitals, a line
			ImVec2 hp = ImGui::GetCursorScreenPos ();
			ImDrawList *dl = ImGui::GetWindowDrawList ();
			ui::PushFont (f.medium, ui::SIZE_SMALL);
			float lh = ImGui::GetTextLineHeight ();
			dl->AddText (hp, ui::Col32 (ui::ACCENT), g.icon);
			std::string t;
			for (const char *p = g.title.c_str(); *p; p++) { t += (char)toupper ((unsigned char)*p); if (p[1]) t += ' '; }
			dl->AddText (ImVec2(hp.x + ui::Px (22), hp.y), ui::Col32 (ui::TEXT_DIM), t.c_str());
			ui::PopFont ();
			float ly = hp.y + lh + ui::Px (5);
			dl->AddLine (ImVec2(hp.x, ly), ImVec2(hp.x + cw, ly), ui::Col32 (ui::DIVIDER), std::max (1.0f, ui::Scale ()));
			ImGui::Dummy (ImVec2(cw, lh + ui::Px (8)));
		}
		for (auto &r : rows) DrawRow (r.first.c_str(), r.second, cw);
		ImGui::EndGroup ();
		colY[c] = ImGui::GetItemRectMax ().y + ui::Px (14);
		shown++;
	}
	if (!shown) {
		ImGui::SetCursorScreenPos (ImVec2(origin.x, origin.y + ui::Px (30)));
		ui::TextDim ("No shortcut matches \"%s\".", filter);
	} else {
		ImGui::SetCursorScreenPos (ImVec2(origin.x, *std::max_element (colY.begin(), colY.end())));
		ImGui::Dummy (ImVec2(0, 0));
	}
	ImGui::EndChild ();

	// footer: where the keys are set
	ImGui::Dummy (ImVec2(0, ui::Px (4)));
	ui::PushFont (f.body, ui::SIZE_SMALL);
	ui::TextDim ("The keys are set in keymap.cfg in the Orbiter folder; a vessel's own keys in Config/Vessels/<class>.keys.");
	ui::PopFont ();

	if (ImGui::GetFrameCount () > openFrame + 1 && ImGui::IsKeyPressed (ImGuiKey_Escape, false)) {
		if (filter[0] && ImGui::GetIO ().WantTextInput) filter[0] = '\0';
		else close = true;
	}
	ImGui::End ();
	if (close) Close ();
}
