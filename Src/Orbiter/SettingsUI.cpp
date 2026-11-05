// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SettingsUI.h"
#include "UiAnchor.h"
#include "Orbiter.h"
#include "Config.h"
#include "Psys.h"
#include "Planet.h"
#include "GraphicsAPI.h"
#include "IconsLucide.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

extern PlanetarySystem *g_psys;

namespace {
	const char *PAGE_NAMES[] = { "Display", "Graphics", "Scenery", "Simulation", "Instruments", "Controls", "Visual helpers" };
	const char *PAGE_ICONS[] = { ICON_LC_MONITOR, ICON_LC_SPARKLES, ICON_LC_MOUNTAIN, ICON_LC_ORBIT, ICON_LC_GAUGE, ICON_LC_GAMEPAD_2, ICON_LC_LAYERS };
	const char *PRESETS[] = { "Low", "Medium", "High", "Ultra" };
}

SettingsUI::SettingsUI (Orbiter *o, Where w): orbiter (o), where (w)
{
}

const char *SettingsUI::PageName (Page p)
{
	return p >= 0 && p < NPAGE ? PAGE_NAMES[p] : "";
}

const char *SettingsUI::NextLaunch () const
{
	return where == FLIGHT ? "Applies at next launch" : nullptr;
}

// ---- layout -------------------------------------------------------------

void SettingsUI::Draw (float w, float h, bool panels)
{
	if (!Shown (page)) page = DISPLAY;
	float nw = std::min (ui::Px (230), w * 0.3f), gap = ui::Px (panels ? 20 : 14);
	ImVec2 p = ImGui::GetCursorScreenPos ();
	if (panels) {
		if (ui::BeginAutoPanel ("##setnav", nw, h)) DrawNav ();
		ui::EndPanel ();
	} else {
		ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		if (ImGui::BeginChild ("##setnav", ImVec2(nw, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) DrawNav ();
		ImGui::EndChild ();
		ImGui::PopStyleVar ();
		// (a divider between the list and the page)
		float x = p.x + nw + gap * 0.5f;
		ImGui::GetWindowDrawList ()->AddLine (ImVec2(x, p.y), ImVec2(x, p.y + h), ui::Col32 (ui::DIVIDER), std::max (1.0f, ui::Scale ()));
	}
	float cw = w - nw - gap;
	if (panels) cw = std::min (cw, ui::Px (920));
	ImGui::SetCursorScreenPos (ImVec2(p.x + nw + gap, p.y));
	if (panels) {
		if (ui::BeginAutoPanel ("##setpage", cw, h)) DrawPage ();
		ui::EndPanel ();
	} else {
		ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(ui::Px (6), 0));
		if (ImGui::BeginChild ("##setpage", ImVec2(cw, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground)) {
			DrawPage ();
			ui::ScrollFade ();
		}
		ImGui::EndChild ();
		ImGui::PopStyleVar ();
	}
}

void SettingsUI::DrawNav ()
{
	if (where == LAUNCHPAD) ui::SectionHeader ("Settings");
	for (int i = 0; i < NPAGE; i++) {
		if (!Shown ((Page)i)) continue;
		ImGui::PushID (i);
		if (ui::ListRow ("##page", PAGE_ICONS[i], PAGE_NAMES[i], nullptr, page == i, nullptr, page == i ? ui::ACCENT : ui::TEXT_DIM))
			SelectPage ((Page)i);
		static const char *anchors[] = { "settings.display", "settings.graphics", "settings.scenery", "settings.simulation",
			"settings.instruments", "settings.controls", "settings.helpers" };
		uianchor::SetItem (anchors[i]); // (tests)
		ImGui::PopID ();
	}
}

void SettingsUI::DrawPage ()
{
	if (scrollTop) ImGui::SetScrollY (0.0f), scrollTop = false;
	ui::PushFont (ui::GetFonts ().strong, ui::SIZE_TITLE);
	ImGui::TextUnformatted (PAGE_NAMES[page]);
	ui::PopFont ();
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	ngs = 0, gs = nullptr;
	switch (page) {
	case DISPLAY:     PageDisplay (); break;
	case GRAPHICS:    PageGraphics (); break;
	case SCENERY:     PageScenery (); break;
	case SIMULATION:  PageSimulation (); break;
	case INSTRUMENTS: PageInstruments (); break;
	case CONTROLS:    PageControls (); break;
	case OVERLAYS:    PageOverlays (); break;
	default: break;
	}
	ImGui::Dummy (ImVec2(0, ui::Px (8)));
}

float SettingsUI::ControlWidth () const
{
	return std::min (ui::Px (280), ImGui::GetContentRegionAvail().x * 0.42f);
}

void SettingsUI::Section (const char *title, const char *status)
{
	ImGui::Dummy (ImVec2(0, ui::Px (10)));
	if (status) { // (the note right-aligned on the header's line)
		ImVec2 p = ImGui::GetCursorScreenPos ();
		float w = ImGui::GetContentRegionAvail().x;
		ui::PushFont (ui::GetFonts ().medium, ui::SIZE_SMALL);
		ImVec2 ts = ImGui::CalcTextSize (status);
		ImGui::GetWindowDrawList ()->AddText (ImVec2(p.x + w - ts.x, p.y + ui::Px (4)), ui::Col32 (ui::CAUTION), status);
		ui::PopFont ();
	}
	ui::SectionHeader (title);
}

void SettingsUI::Note (const char *icon, const char *text, ui::Colour c)
{
	ImGui::Dummy (ImVec2(0, ui::Px (4)));
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float w = ImGui::GetContentRegionAvail().x;
	float pad = ui::Px (12), iw = ImGui::GetFontSize () + ui::Px (10);
	ImVec2 ts = ImGui::CalcTextSize (text, nullptr, false, w - 2 * pad - iw);
	float h = ts.y + 2 * ui::Px (9);
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	dl->AddRectFilled (p, ImVec2(p.x + w, p.y + h), ui::Col32 (c == ui::TEXT_DIM ? ui::RAISED : c, c == ui::TEXT_DIM ? 0.7f : 0.12f), ui::Px (6));
	dl->AddText (ImVec2(p.x + pad, p.y + ui::Px (9)), ui::Col32 (c == ui::TEXT_DIM ? ui::ACCENT : c), icon);
	ImGui::SetCursorScreenPos (ImVec2(p.x + pad + iw, p.y + ui::Px (9)));
	ImGui::PushTextWrapPos (p.x + w - pad - ImGui::GetWindowPos ().x + ImGui::GetScrollX ());
	ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_DIM));
	ImGui::TextUnformatted (text);
	ImGui::PopStyleColor ();
	ImGui::PopTextWrapPos ();
	ImGui::SetCursorScreenPos (ImVec2(p.x, p.y + h));
	ImGui::Dummy (ImVec2(w, ui::Px (4)));
}

void SettingsUI::BeginRow (const char *label, const char *help, float ctrlW, const char *status, ui::Colour statusColour)
{
	const float padY = ui::Px (9), gap = ui::Px (24);
	ImVec2 p = ImGui::GetCursorScreenPos ();
	row.x0 = p.x, row.y0 = p.y;
	row.w = ImGui::GetContentRegionAvail().x;
	row.ctrlW = ctrlW;
	row.labelW = std::max (ui::Px (80), row.w - ctrlW - gap);
	float fh = ImGui::GetFrameHeight ();

	// the height: the label (and its status) over the help text
	ui::PushFont (ui::GetFonts ().medium, ui::SIZE_BODY);
	float lh = ImGui::GetTextLineHeight ();
	ui::PopFont ();
	float hh = 0.0f;
	if (help) {
		ui::PushFont (ui::GetFonts ().body, ui::SIZE_SMALL);
		hh = ImGui::CalcTextSize (help, nullptr, false, row.labelW).y + ui::Px (2);
		ui::PopFont ();
	}
	float th = lh + hh;
	row.h = std::max (fh, th) + 2 * padY;

	// label
	float ty = p.y + padY + std::max (0.0f, (fh - th) * 0.5f);
	ImGui::SetCursorScreenPos (ImVec2(p.x, ty));
	ui::PushFont (ui::GetFonts ().medium, ui::SIZE_BODY);
	ImGui::TextUnformatted (label);
	ui::PopFont ();
	if (status) {
		ImGui::SameLine (0, ui::Px (10));
		ui::PushFont (ui::GetFonts ().medium, ui::SIZE_SMALL);
		ImGui::SetCursorPosY (ImGui::GetCursorPosY () + (lh - ImGui::GetTextLineHeight ()) * 0.5f);
		ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (statusColour));
		ImGui::TextUnformatted (status);
		ImGui::PopStyleColor ();
		ui::PopFont ();
	}
	if (help) {
		ImGui::SetCursorScreenPos (ImVec2(p.x, ty + lh + ui::Px (2)));
		ui::PushFont (ui::GetFonts ().body, ui::SIZE_SMALL);
		ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_DIM));
		ImGui::PushTextWrapPos (p.x + row.labelW - ImGui::GetWindowPos ().x + ImGui::GetScrollX ());
		ImGui::TextUnformatted (help);
		ImGui::PopTextWrapPos ();
		ImGui::PopStyleColor ();
		ui::PopFont ();
	}
	// the control: right-aligned, centred on the row
	ImGui::SetCursorScreenPos (ImVec2(p.x + row.w - ctrlW, p.y + (row.h - fh) * 0.5f));
	ImGui::SetNextItemWidth (ctrlW);
}

void SettingsUI::EndRow ()
{
	float y = row.y0 + row.h;
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	dl->AddLine (ImVec2(row.x0, y), ImVec2(row.x0 + row.w, y), ui::Col32 (ui::DIVIDER, 0.6f), std::max (1.0f, ui::Scale ()));
	ImGui::SetCursorScreenPos (ImVec2(row.x0, y));
	ImGui::Dummy (ImVec2(row.w, 1.0f));
}

namespace {
	float ToggleWidth () { return ImGui::GetFrameHeight () * 0.78f * 1.8f; }
}

bool SettingsUI::ToggleRow (const char *label, const char *help, bool *v, const char *status)
{
	BeginRow (label, help, ToggleWidth (), status, ui::CAUTION);
	ImGui::PushID (label);
	bool r = ui::Toggle ("##t", v);
	ImGui::PopID ();
	EndRow ();
	return r;
}

bool SettingsUI::FlagRow (const char *label, const char *help, int *flags, int bit)
{
	bool v = (*flags & bit) != 0;
	if (!ToggleRow (label, help, &v)) return false;
	if (v) *flags |= bit; else *flags &= ~bit;
	return true;
}

bool SettingsUI::SliderRow (const char *label, const char *help, float *v, float vmin, float vmax, const char *format)
{
	BeginRow (label, help, ControlWidth ());
	ImGui::PushID (label);
	bool r = ui::Slider ("##s", v, vmin, vmax, format, row.ctrlW);
	ImGui::PopID ();
	EndRow ();
	return r;
}

bool SettingsUI::SliderRow (const char *label, const char *help, double *v, double vmin, double vmax, const char *format)
{
	float f = (float)*v;
	if (!SliderRow (label, help, &f, (float)vmin, (float)vmax, format)) return false;
	*v = f;
	return true;
}

bool SettingsUI::SliderRow (const char *label, const char *help, int *v, int vmin, int vmax, const char *format)
{
	BeginRow (label, help, ControlWidth ());
	ImGui::PushID (label);
	bool r = ui::Slider ("##s", v, vmin, vmax, format, row.ctrlW);
	ImGui::PopID ();
	EndRow ();
	return r;
}

bool SettingsUI::ComboRow (const char *label, const char *help, int *sel, const char *const *items, int n)
{
	BeginRow (label, help, ControlWidth ());
	ImGui::PushID (label);
	bool r = false;
	if (ui::BeginCombo ("##c", *sel >= 0 && *sel < n ? items[*sel] : "")) {
		for (int i = 0; i < n; i++)
			if (ui::ComboItem (items[i], *sel == i) && *sel != i) *sel = i, r = true;
		ui::EndCombo ();
	}
	ImGui::PopID ();
	EndRow ();
	return r;
}

// ---- Display --------------------------------------------------------------

void SettingsUI::PageDisplay ()
{
	CFG_DEVPRM &d = orbiter->Cfg()->CfgDevPrm;
	Section ("Window", NextLaunch ());
	ToggleRow ("Fullscreen", "At the desktop's resolution.", &d.bFullscreen);
	ImGui::BeginDisabled (d.bFullscreen);
	static const struct { int w, h; } sizes[] = {
		{1280, 720}, {1280, 800}, {1600, 900}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {3840, 2160}
	};
	char cur[32];
	snprintf (cur, sizeof(cur), "%u \xC3\x97 %u", d.WinW, d.WinH);
	BeginRow ("Window size", "In pixels, when not fullscreen.", ControlWidth ());
	if (ui::BeginCombo ("##winsize", cur)) {
		for (auto &s : sizes) {
			char lbl[32];
			snprintf (lbl, sizeof(lbl), "%d \xC3\x97 %d", s.w, s.h);
			if (ui::ComboItem (lbl, (int)d.WinW == s.w && (int)d.WinH == s.h)) d.WinW = s.w, d.WinH = s.h;
		}
		ui::EndCombo ();
	}
	EndRow ();
	BeginRow ("Custom size", "Width and height.", ControlWidth ());
	int wh[2] = { (int)d.WinW, (int)d.WinH };
	if (ImGui::InputInt2 ("##custom", wh, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit ()) {
		d.WinW = (DWORD)std::clamp (wh[0], 320, 16384);
		d.WinH = (DWORD)std::clamp (wh[1], 200, 16384);
	}
	EndRow ();
	ImGui::EndDisabled ();

	Section ("Interface");
	CFG_UIPRM &u = orbiter->Cfg()->CfgUIPrm;
	int pct = (int)std::lround (u.UIScale * 100.0f);
	BeginRow ("Interface size", "Text and controls, on top of the scaling with the window's resolution.", ControlWidth ());
	// (applied when the slider is released: the controls would move under the mouse)
	static int dragPct = -1;
	int shown = dragPct >= 0 ? dragPct : pct;
	if (ui::Slider ("##uiscale", &shown, 60, 200, "%d %%", row.ctrlW)) dragPct = shown;
	if (ImGui::IsItemDeactivated () && dragPct >= 0) {
		u.UIScale = (float)(std::lround (dragPct / 5.0) * 5) / 100.0f;
		dragPct = -1;
	}
	EndRow ();
	static const char *fpsPos[] = { "Off", "Top left", "Top right" };
	ComboRow ("Frame rate", "Frames per second, frame time and the 1% low, with a graph, in an info box at the top.", &u.FPS, fpsPos, 3);
#ifndef _WIN32
	ToggleRow ("Live scene behind the launchpad", "Off: a plain background, less work for the graphics card. Applies the next time the launchpad opens.",
		&u.bLaunchpadBackdrop);
#else
	static const char *focus[] = { "Click to focus", "Click for child windows", "Focus follows the mouse" };
	ComboRow ("Mouse focus", "How a window gets the keyboard's focus.", &u.MouseFocusMode, focus, 3);
#endif
}

// ---- Graphics -------------------------------------------------------------

int SettingsUI::MatchingPreset () const
{
	for (int p = 0; p < 4; p++) {
		bool all = true, any = false;
		for (int i = 0; i < ngs && all; i++) {
			const oapi::GraphicsSetting &s = gs[i];
			if (std::isnan (s.preset[p]) || s.lockedBy || s.unavailable) continue;
			any = true;
			if (std::fabs (s.value - s.preset[p]) > 1e-4f) all = false;
		}
		if (all && any) return p;
	}
	return -1;
}

void SettingsUI::SkyRows ()
{
	CFG_VISUALPRM &v = orbiter->Cfg()->CfgVisualPrm;
	if (ToggleRow ("Stars", "Stars drawn as points of light.", &v.bUseStarDots))
		orbiter->OnOptionChanged (OPTCAT_CELSPHERE, OPTITEM_CELSPHERE_ACTIVATESTARDOTS);
	ImGui::BeginDisabled (!v.bUseStarDots);
	StarRenderPrm &sp = v.StarPrm;
	double lo = std::clamp (sp.mag_lo, 3.0, 12.0);
	if (SliderRow ("Faintest stars", "Magnitude of the faintest star drawn; fainter ones join the diffuse light.", &lo, 3.0, 12.0, "magnitude %.1f")) {
		sp.mag_lo = lo;
		if (sp.mag_hi >= sp.mag_lo) sp.mag_hi = sp.mag_lo - 0.01;
		orbiter->OnOptionChanged (OPTCAT_CELSPHERE, OPTITEM_CELSPHERE_STARDISPLAYPARAM);
	}
	ImGui::EndDisabled ();
	if (ToggleRow ("Diffuse light", "The glow of the Milky Way and of the stars too faint to see one by one.", &v.bUseBgImage))
		orbiter->OnOptionChanged (OPTCAT_CELSPHERE, OPTITEM_CELSPHERE_ACTIVATEBGIMAGE);
	ImGui::BeginDisabled (!v.bUseBgImage);
	if (SliderRow ("Diffuse light brightness", "0.30: as measured.", &v.CSphereBgIntens, 0.01, 1.0, "%.2f"))
		orbiter->OnOptionChanged (OPTCAT_CELSPHERE, OPTITEM_CELSPHERE_BGIMAGEBRIGHTNESS);
	ImGui::EndDisabled ();
}

void SettingsUI::ClientRow (int i)
{
	oapi::GraphicsClient *gc = orbiter->GetGraphicsClient ();
	const oapi::GraphicsSetting &s = gs[i];
	// (a setting it requires)
	bool reqOk = true;
	if (s.requires) {
		std::string key = s.requires;
		int want = -1;
		size_t eq = key.find ('=');
		if (eq != std::string::npos) want = atoi (key.c_str () + eq + 1), key = key.substr (0, eq);
		for (int k = 0; k < ngs; k++)
			if (key == gs[k].key) {
				reqOk = want < 0 ? gs[k].value != 0.0f : (int)std::lround (gs[k].value) == want;
				if (gs[k].unavailable && want < 0) reqOk = false;
			}
	}
	char status[96] = "";
	ui::Colour sc = ui::CAUTION;
	if (s.lockedBy) snprintf (status, sizeof(status), ICON_LC_LOCK " %s", s.lockedBy), sc = ui::TEXT_FAINT;
	else if (s.apply != oapi::GraphicsSetting::LIVE && std::fabs (s.value - s.effective) > 1e-4f &&
		(where == FLIGHT || s.apply == oapi::GraphicsSetting::RESTART))
		snprintf (status, sizeof(status), "%s", s.apply == oapi::GraphicsSetting::RESTART ? "Applies after a restart" : "Applies at next launch");
	const char *help = s.unavailable ? s.unavailable : s.help;
	bool disabled = s.lockedBy || s.unavailable || !reqOk;
	ImGui::BeginDisabled (disabled);
	ImGui::PushID (s.key);
	float value = s.value;
	bool changed = false, save = false;
	switch (s.type) {
	case oapi::GraphicsSetting::TOGGLE: {
		bool v = value != 0.0f;
		BeginRow (s.label, help, ToggleWidth (), status[0] ? status : nullptr, sc);
		if (ui::Toggle ("##t", &v)) value = v ? 1.0f : 0.0f, changed = save = true;
		break; }
	case oapi::GraphicsSetting::CHOICE: {
		BeginRow (s.label, help, ControlWidth (), status[0] ? status : nullptr, sc);
		int sel = std::clamp ((int)std::lround (value), 0, std::max (0, s.nchoice - 1));
		if (ui::BeginCombo ("##c", s.nchoice ? s.choice[sel] : "")) {
			for (int c = 0; c < s.nchoice; c++)
				if (ui::ComboItem (s.choice[c], sel == c) && sel != c) value = (float)c, changed = save = true;
			ui::EndCombo ();
		}
		break; }
	case oapi::GraphicsSetting::SLIDER: {
		BeginRow (s.label, help, ControlWidth (), status[0] ? status : nullptr, sc);
		float k = s.display != 0.0f ? s.display : 1.0f;
		float d = value * k;
		if (ui::Slider ("##s", &d, s.vmin * k, s.vmax * k, s.format ? s.format : "%.2f", row.ctrlW)) {
			value = d / k;
			if (s.step > 0.0f) value = s.vmin + std::round ((value - s.vmin) / s.step) * s.step;
			changed = true;
		}
		if (ImGui::IsItemDeactivatedAfterEdit ()) save = true;
		break; }
	}
	if (uianchor::Enabled ()) uianchor::SetItem ((std::string ("gfx.") + s.key).c_str ()); // (tests: the control)
	ImGui::PopID ();
	EndRow ();
	ImGui::EndDisabled ();
	if (changed && gc) gc->clbkSetSetting (i, value);
	if (save && gc) gc->clbkSaveSettings ();
}

void SettingsUI::PageGraphics ()
{
	oapi::GraphicsClient *gc = orbiter->GetGraphicsClient ();
	ngs = gc ? gc->clbkGetSettings (&gs) : 0;
	if (!ngs) {
		Note (ICON_LC_INFO, "The graphics client has no settings of its own here.");
		Section ("Sky");
		SkyRows ();
		return;
	}
	// quality presets
	ImGui::Dummy (ImVec2(0, ui::Px (6)));
	int preset = MatchingPreset (), sel = preset;
	BeginRow ("Quality", preset < 0 ? "Custom: the settings below differ from the presets." : "Sets the lighting, effects and sky below.",
		std::min (ui::Px (380), ImGui::GetContentRegionAvail().x * 0.55f));
	bool pick = ui::Segmented ("##preset", PRESETS, 4, &sel);
	if (uianchor::Enabled ()) { // (tests: the presets' segments)
		ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
		static const char *names[] = { "gfx.preset.low", "gfx.preset.medium", "gfx.preset.high", "gfx.preset.ultra" };
		for (int k = 0; k < 4; k++) uianchor::Set (names[k], a.x + (b.x - a.x) * (k + 0.5f) / 4, (a.y + b.y) * 0.5f);
	}
	if (pick && sel >= 0) {
		std::vector<std::pair<int, float>> set;
		for (int i = 0; i < ngs; i++)
			if (!std::isnan (gs[i].preset[sel]) && !gs[i].lockedBy) set.push_back ({ i, gs[i].preset[sel] });
		for (auto &s : set) gc->clbkSetSetting (s.first, s.second);
		gc->clbkSaveSettings ();
		ngs = gc->clbkGetSettings (&gs);
	}
	EndRow ();

	const char *group = nullptr;
	bool sky = false;
	for (int i = 0; i < ngs; i++) {
		if (!group || strcmp (group, gs[i].group)) {
			group = gs[i].group;
			Section (group);
			if (!strcmp (group, "Sky")) SkyRows (), sky = true;
		}
		ClientRow (i);
	}
	if (!sky) {
		Section ("Sky");
		SkyRows ();
	}
}

// ---- Scenery --------------------------------------------------------------

void SettingsUI::PageScenery ()
{
	Config *cfg = orbiter->Cfg();
	CFG_VISUALPRM &v = cfg->CfgVisualPrm;
	if (where == FLIGHT) Note (ICON_LC_INFO, "These settings apply when a scenario is launched.");
	Section ("Planets");
	ToggleRow ("Cloud layers", nullptr, &v.bClouds);
	ImGui::BeginDisabled (!v.bClouds);
	ToggleRow ("Cloud shadows", "Clouds cast shadows on the ground.", &v.bCloudShadows);
	ImGui::EndDisabled ();
	ToggleRow ("Horizon haze", nullptr, &v.bHaze);
	ToggleRow ("Distance fog", nullptr, &v.bFog);
	ToggleRow ("Water reflections", "The Sun's glint on water.", &v.bWaterreflect);
	ImGui::BeginDisabled (!v.bWaterreflect);
	ToggleRow ("Ripples", "Waves on the water's surface.", &v.bSpecularRipple);
	ImGui::EndDisabled ();
	ToggleRow ("Night lights", "City lights on a planet's night side.", &v.bNightlights);
	ImGui::BeginDisabled (!v.bNightlights);
	SliderRow ("Night light brightness", nullptr, &v.LightBrightness, 0.0, 1.0, "%.2f");
	ImGui::EndDisabled ();
	bool elev = v.ElevMode != 0;
	if (ToggleRow ("Surface elevation", "Mountains and valleys from the elevation data.", &elev)) v.ElevMode = elev ? 2 : 0;
	ImGui::BeginDisabled (!elev);
	int mode = std::max (0, v.ElevMode - 1);
	static const char *modes[] = { "Linear", "Cubic" };
	if (ComboRow ("Elevation interpolation", "Between the elevation data's points.", &mode, modes, 2) && elev) v.ElevMode = mode + 1;
	ToggleRow ("Procedural terrain detail", "Relief and craters finer than the elevation data (also for ground contact).", &v.bTerrainDetail);
	ImGui::EndDisabled ();
	int lvl = (int)v.PlanetMaxLevel;
	if (SliderRow ("Surface resolution", "The finest level of the surface tiles.", &lvl, 1, SURF_MAX_PATCHLEVEL2, "level %d")) v.PlanetMaxLevel = (DWORD)lvl;
	float bias = (float)cfg->CfgPRenderPrm.ResolutionBias;
	if (SliderRow ("Detail distance", "Higher: finer surface meshes farther from the camera (slower).", &bias, -2.0f, 2.0f, "%+.1f"))
		cfg->CfgPRenderPrm.ResolutionBias = bias;

	Section ("Vessels and objects");
	ToggleRow ("Vessel shadows", nullptr, &v.bVesselShadows);
	ToggleRow ("Object shadows", "Shadows of buildings and other surface objects.", &v.bShadows);
	ToggleRow ("Specular reflections", "Shine on vessels and objects.", &v.bSpecular);
	ToggleRow ("Re-entry flames", nullptr, &v.bReentryFlames);
	ToggleRow ("Particle streams", "Exhaust and smoke of add-on vessels.", &v.bParticleStreams);
	ToggleRow ("Local light sources", "Spotlights and engine light.", &v.bLocalLight);
	int amb = (int)v.AmbientLevel;
	if (SliderRow ("Ambient light", "Light on the night side and in shadows (0: none).", &amb, 0, 255, "%d")) cfg->SetAmbientLevel ((DWORD)amb);
}

// ---- Simulation -----------------------------------------------------------

void SettingsUI::PageSimulation ()
{
	Config *cfg = orbiter->Cfg();
	CFG_PHYSICSPRM &p = cfg->CfgPhysicsPrm;
	CFG_LOGICPRM &l = cfg->CfgLogicPrm;
	Section ("Vessels");
	if (ToggleRow ("Limited fuel", "Off: the tanks never run dry.", &l.bLimitedFuel))
		orbiter->OnOptionChanged (OPTCAT_VESSEL, OPTITEM_VESSEL_LIMITEDFUEL);
	if (ToggleRow ("Refuel on pads", "Vessels landed on a pad are refuelled.", &l.bPadRefuel))
		orbiter->OnOptionChanged (OPTCAT_VESSEL, OPTITEM_VESSEL_PADREFUEL);
	static const char *fmodel[] = { "Simple", "Realistic" };
	ComboRow ("Flight model", "Vessels that have two flight models use the one chosen.", &l.FlightModelLevel, fmodel, 2);
	static const char *damage[] = { "No damage", "Damage and failures" };
	ComboRow ("Damage", "Vessels can be damaged by overloads, heat and impacts.", &l.DamageSetting, damage, 2);

	Section ("Physics engine", NextLaunch ());
	ToggleRow ("Nonspherical gravity", "The gravity fields of oblate planets (for precise orbits).", &p.bNonsphericalGrav);
	ToggleRow ("Gravity-gradient torque", "Tidal torques on long vessels.", &p.bDistributedMass);
	ToggleRow ("Radiation pressure", "The push of sunlight.", &p.bRadiationPressure);
	ToggleRow ("Atmospheric wind", "Winds move vessels in the air.", &p.bAtmWind);

	Section ("Docking");
	SliderRow ("Max. closing speed", "Faster approaches are refused.", &p.DockSpeedLimit, 0.05, 5.0, "%.2f m/s");
	double a = p.DockAngleLimit * DEG;
	if (SliderRow ("Max. misalignment", "Larger angles between the ports are refused.", &a, 1.0, 45.0, "%.1f\xC2\xB0"))
		p.DockAngleLimit = a * RAD;
}

// ---- Instruments ----------------------------------------------------------

void SettingsUI::PageInstruments ()
{
	CFG_INSTRUMENTPRM &ins = orbiter->Cfg()->CfgInstrumentPrm;
	CFG_LOGICPRM &l = orbiter->Cfg()->CfgLogicPrm;
	Section ("Multifunctional displays");
	if (SliderRow ("Refresh interval", "Time between the displays' updates.", &l.InstrUpdDT, 0.01, 2.0, "%.2f s"))
		orbiter->OnOptionChanged (OPTCAT_INSTRUMENT, OPTITEM_INSTRUMENT_MFDUPDATEINTERVAL);
	if (SliderRow ("Glass cockpit display size", nullptr, &l.MFDSize, 1, 10, "%d"))
		orbiter->OnOptionChanged (OPTCAT_INSTRUMENT, OPTITEM_INSTRUMENT_MFDGENERICSIZE);
	if (ToggleRow ("Transparent glass cockpit displays", nullptr, &l.bMfdTransparent))
		orbiter->OnOptionChanged (OPTCAT_INSTRUMENT, OPTITEM_INSTRUMENT_MFDGENERICTRANSP);
	static const char *vcsize[] = { "256 \xC3\x97 256", "512 \xC3\x97 512", "1024 \xC3\x97 1024" };
	int sel = ins.VCMFDSize >= 1024 ? 2 : ins.VCMFDSize >= 512 ? 1 : 0;
	if (ComboRow ("Virtual cockpit display resolution", "Texture size of the displays in 3D cockpits.", &sel, vcsize, 3)) {
		ins.VCMFDSize = 256 << sel;
		orbiter->OnOptionChanged (OPTCAT_INSTRUMENT, OPTITEM_INSTRUMENT_MFDVCSIZE);
	}

	Section ("Instrument panels");
	if (SliderRow ("Panel scroll speed", "Negative: the panel moves the other way.", &l.PanelScrollSpeed, -100.0, 100.0, "%.0f"))
		orbiter->OnOptionChanged (OPTCAT_INSTRUMENT, OPTITEM_INSTRUMENT_PANELSCROLLSPEED);
	if (SliderRow ("2D panel scale", nullptr, &l.PanelScale, 0.25, 4.0, "%.2f"))
		orbiter->OnOptionChanged (OPTCAT_INSTRUMENT, OPTITEM_INSTRUMENT_PANELSCALE);
}

// ---- Controls -------------------------------------------------------------

void SettingsUI::PageControls ()
{
	Config *cfg = orbiter->Cfg();
	CFG_JOYSTICKPRM &j = cfg->CfgJoystickPrm;
	DIDEVICEINSTANCE *list = nullptr;
	DWORD ndev = 0;
#ifdef _WIN32
	orbiter->GetDInput()->GetJoysticks (&list, &ndev);
#else
	orbiter->GetInput()->GetJoysticks (&list, &ndev); // (reported by the graphics client)
#endif
	Section ("Game controller");
	const char *preview = (j.Joy_idx > 0 && j.Joy_idx <= ndev) ? list[j.Joy_idx-1].tszProductName : "Disabled";
	BeginRow ("Device", ndev ? nullptr : "No controllers connected.", ControlWidth ());
	if (ui::BeginCombo ("##joy", preview)) {
		DWORD idx = j.Joy_idx;
		if (ui::ComboItem ("Disabled", idx == 0)) idx = 0;
		for (DWORD i = 0; i < ndev; i++) {
			ImGui::PushID ((int)i);
			if (ui::ComboItem (list[i].tszProductName, idx == i + 1)) idx = i + 1;
			ImGui::PopID ();
		}
		ui::EndCombo ();
		if (idx != j.Joy_idx) {
			j.Joy_idx = idx;
			orbiter->OnOptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_DEVICE);
		}
	}
	EndRow ();

#ifndef _WIN32
	if (orbiter->GetInput()->Gamepad ()) {
		Section ("Gamepad controls");
		static const char *rows[][2] = {
			{"Left stick", "Pitch and bank"}, {"Shoulder buttons", "Yaw"},
			{"Triggers", "Main engine (right: increase, left: decrease)"},
			{"Right stick", "Rotate camera"}, {"A", "Kill rotation"}, {"B", "RCS mode"},
			{"X", "HUD mode"}, {"Y", "Cockpit / external view"}, {"D-pad up/down", "Time acceleration"},
			{"D-pad left/right", "Prograde / retrograde"}, {"Left stick button", "Level with horizon"},
			{"Right stick button", "Reset cockpit camera"}, {"Start", "Pause"}, {"Back", "Main menu"},
		};
		ImGui::PushStyleVar (ImGuiStyleVar_CellPadding, ImVec2(ui::Px (10), ui::Px (5)));
		if (ImGui::BeginTable ("##pad", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
			for (auto &r : rows) {
				ImGui::TableNextRow ();
				ImGui::TableNextColumn (); ImGui::TextUnformatted (r[0]);
				ImGui::TableNextColumn (); ui::TextDim ("%s", r[1]);
			}
			ImGui::EndTable ();
		}
		ImGui::PopStyleVar ();
		ImGui::BeginDisabled (j.Joy_idx == 0);
		if (SliderRow ("Dead zone", "Stick movement ignored around the centre.", &j.Deadzone, 0, 10000, "%d"))
			orbiter->OnOptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_PARAM);
		ImGui::EndDisabled ();
		return;
	}
#endif
	ImGui::BeginDisabled (j.Joy_idx == 0);
	Section ("Main engine control");
	static const char *axes[] = { "Keyboard only", "Z-axis", "Slider 0", "Slider 1" };
	int ax = (int)std::min<DWORD> (j.ThrottleAxis, 3);
	if (ComboRow ("Throttle axis", nullptr, &ax, axes, 4)) {
		j.ThrottleAxis = (DWORD)ax;
		orbiter->OnOptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_PARAM);
	}
	ToggleRow ("Ignore throttle on launch", "The throttle's position counts only once it has moved.", &j.bThrottleIgnore);
	Section ("Calibration");
	if (SliderRow ("Throttle saturation", "Throttle movement near the ends that counts as full or none.", &j.ThrottleSaturation, 0, 10000, "%d"))
		orbiter->OnOptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_PARAM);
	if (SliderRow ("Dead zone", "Stick movement ignored around the centre.", &j.Deadzone, 0, 10000, "%d"))
		orbiter->OnOptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_PARAM);
	ImGui::EndDisabled ();
}

// ---- Visual helpers (in flight) ------------------------------------------

void SettingsUI::AddBodyNode (const CelestialBody *body)
{
	ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (featureBody == body->Name ()) fl |= ImGuiTreeNodeFlags_Selected;
	if (body->nSecondary ()) {
		if (!strcmp (body->Name (), "Sun")) fl |= ImGuiTreeNodeFlags_DefaultOpen;
		bool open = ImGui::TreeNodeEx (body->Name (), fl);
		if (ImGui::IsItemClicked () && !ImGui::IsItemToggledOpen ()) featureBody = body->Name (), ImGui::CloseCurrentPopup ();
		if (open) {
			for (int i = 0; i < body->nSecondary (); i++) AddBodyNode (body->Secondary (i));
			ImGui::TreePop ();
		}
	} else {
		ImGui::TreeNodeEx (body->Name (), fl | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
		if (ImGui::IsItemClicked ()) featureBody = body->Name (), ImGui::CloseCurrentPopup ();
	}
}

void SettingsUI::PageOverlays ()
{
	if (!g_psys) return;
	CFG_VISHELPPRM &prm = orbiter->Cfg()->CfgVisHelpPrm;
	static const char *tabs[] = { "Planetarium", "Labels", "Forces", "Axes" };
	ImGui::Dummy (ImVec2(0, ui::Px (4)));
	ui::Segmented ("##overlay", tabs, 4, &overlayTab);
	ImGui::Dummy (ImVec2(0, ui::Px (4)));

	switch (overlayTab) {
	case 0: {
		bool ch = FlagRow ("Planetarium mode", "Grids, constellations and markers on the sky (F9).", &prm.flagPlanetarium, PLN_ENABLE);
		ImGui::BeginDisabled (!(prm.flagPlanetarium & PLN_ENABLE));
		Section ("Grids and circles");
		ch |= FlagRow ("Celestial grid", nullptr, &prm.flagPlanetarium, PLN_CGRID);
		ch |= FlagRow ("Ecliptic grid", nullptr, &prm.flagPlanetarium, PLN_EGRID);
		ch |= FlagRow ("Galactic grid", nullptr, &prm.flagPlanetarium, PLN_GGRID);
		ch |= FlagRow ("Local horizon grid", nullptr, &prm.flagPlanetarium, PLN_HGRID);
		ch |= FlagRow ("Target's equator", nullptr, &prm.flagPlanetarium, PLN_EQU);
		Section ("Constellations");
		ch |= FlagRow ("Names", nullptr, &prm.flagPlanetarium, PLN_CNSTLABEL);
		ImGui::BeginDisabled (!(prm.flagPlanetarium & PLN_CNSTLABEL));
		ch |= FlagRow ("Full names", "Off: abbreviations.", &prm.flagPlanetarium, PLN_CNSTLONG);
		ImGui::EndDisabled ();
		ch |= FlagRow ("Boundaries", nullptr, &prm.flagPlanetarium, PLN_CNSTBND);
		ch |= FlagRow ("Patterns", "Lines between the stars of a constellation.", &prm.flagPlanetarium, PLN_CONST);
		Section ("Celestial markers");
		ch |= FlagRow ("Markers", nullptr, &prm.flagPlanetarium, PLN_CCMARK);
		ImGui::BeginDisabled (!(prm.flagPlanetarium & PLN_CCMARK));
		std::vector<oapi::GraphicsClient::LABELLIST> &list = g_psys->LabelList ();
		bool lch = false;
		for (auto &lbl : list) lch |= ToggleRow (lbl.name.c_str (), nullptr, &lbl.active);
		if (lch) {
			std::ifstream fcfg (orbiter->Cfg()->ConfigPath (g_psys->Name ().c_str ()));
			g_psys->ScanLabelLists (fcfg);
		}
		ImGui::EndDisabled ();
		ImGui::EndDisabled ();
		if (ch) orbiter->OnOptionChanged (OPTCAT_PLANETARIUM, OPTITEM_PLANETARIUM_DISPFLAG);
		break; }
	case 1: {
		bool ch = FlagRow ("Labels", "Names of vessels, bodies, bases and surface features (Alt+F9).", &prm.flagMarkers, MKR_ENABLE);
		ImGui::BeginDisabled (!(prm.flagMarkers & MKR_ENABLE));
		Section ("Objects");
		ch |= FlagRow ("Vessels", nullptr, &prm.flagMarkers, MKR_VMARK);
		ch |= FlagRow ("Celestial bodies", nullptr, &prm.flagMarkers, MKR_CMARK);
		ch |= FlagRow ("Surface bases", nullptr, &prm.flagMarkers, MKR_BMARK);
		ch |= FlagRow ("Radio beacons", "VOR transmitters.", &prm.flagMarkers, MKR_RMARK);
		Section ("Surface features");
		ch |= FlagRow ("Features", "Mountains, craters, cities and more.", &prm.flagMarkers, MKR_LMARK);
		ImGui::BeginDisabled (!(prm.flagMarkers & MKR_LMARK));
		BeginRow ("Body", "Whose features are listed below.", ControlWidth ());
		if (ui::BeginCombo ("##body", featureBody.c_str (), ImGuiComboFlags_HeightLarge)) {
			for (int i = 0; i < g_psys->nStar (); i++) AddBodyNode (g_psys->GetStar (i));
			ui::EndCombo ();
		}
		EndRow ();
		if (Planet *planet = g_psys->GetPlanet (featureBody.c_str (), true)) {
			if (planet->LabelFormat () < 2) {
				int n;
				oapi::GraphicsClient::LABELLIST *list = planet->LabelList (&n);
				for (int i = 0; i < n; i++)
					if (ToggleRow (list[i].name.c_str (), nullptr, &list[i].active)) {
						std::ifstream fcfg (orbiter->Cfg()->ConfigPath (planet->Name ()));
						planet->ScanLabelLists (fcfg);
					}
			} else {
				int n = planet->NumLabelLegend ();
				const oapi::GraphicsClient::LABELTYPE *lt = planet->LabelLegend ();
				for (int i = 0; i < n; i++) {
					bool on = lt[i].active;
					if (ToggleRow (lt[i].name, nullptr, &on)) planet->SetLabelActive (i, on);
				}
			}
		}
		ImGui::EndDisabled ();
		ImGui::EndDisabled ();
		if (ch) g_psys->ActivatePlanetLabels ((prm.flagMarkers & MKR_ENABLE) && (prm.flagMarkers & MKR_LMARK));
		break; }
	case 2: {
		FlagRow ("Force vectors", "Arrows for the forces on the focus vessel.", &prm.flagBodyForce, BFV_ENABLE);
		ImGui::BeginDisabled (!(prm.flagBodyForce & BFV_ENABLE));
		Section ("Forces");
		FlagRow ("Weight", nullptr, &prm.flagBodyForce, BFV_WEIGHT);
		FlagRow ("Thrust", nullptr, &prm.flagBodyForce, BFV_THRUST);
		FlagRow ("Lift", nullptr, &prm.flagBodyForce, BFV_LIFT);
		FlagRow ("Drag", nullptr, &prm.flagBodyForce, BFV_DRAG);
		FlagRow ("Side force", nullptr, &prm.flagBodyForce, BFV_SIDEFORCE);
		FlagRow ("Total", "The sum of the forces.", &prm.flagBodyForce, BFV_TOTAL);
		FlagRow ("Torque", nullptr, &prm.flagBodyForce, BFV_TORQUE);
		Section ("Display");
		int log = (prm.flagBodyForce & BFV_LOGSCALE) ? 1 : 0;
		static const char *scale[] = { "Linear", "Logarithmic" };
		if (ComboRow ("Scale", "Logarithmic: small and large forces both visible.", &log, scale, 2))
			prm.flagBodyForce = log ? (prm.flagBodyForce | BFV_LOGSCALE) : (prm.flagBodyForce & ~BFV_LOGSCALE);
		SliderRow ("Length", nullptr, &prm.scaleBodyForce, 0.25f, 4.0f, "%.2f");
		SliderRow ("Opacity", nullptr, &prm.opacBodyForce, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled ();
		break; }
	case 3: {
		FlagRow ("Frame axes", "The x, y and z axes of objects.", &prm.flagFrameAxes, FAV_ENABLE);
		ImGui::BeginDisabled (!(prm.flagFrameAxes & FAV_ENABLE));
		Section ("Objects");
		FlagRow ("Vessels", nullptr, &prm.flagFrameAxes, FAV_VESSEL);
		FlagRow ("Celestial bodies", nullptr, &prm.flagFrameAxes, FAV_CELBODY);
		FlagRow ("Surface bases", nullptr, &prm.flagFrameAxes, FAV_BASE);
		Section ("Display");
		FlagRow ("Negative axes", "Also the axes' negative halves.", &prm.flagFrameAxes, FAV_NEGATIVE);
		SliderRow ("Length", nullptr, &prm.scaleFrameAxes, 0.25f, 4.0f, "%.2f");
		SliderRow ("Opacity", nullptr, &prm.opacFrameAxes, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled ();
		break; }
	}
}
