// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "UiTheme.h"
#include <type_traits>
#include "imgui_internal.h"
#include "IconsLucide.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace ui {

namespace {

	// the palette (sRGB)
	const unsigned PALETTE[NCOLOUR] = {
		0x060A13, // BACKDROP
		0x0C1322, // WINDOW
		0x101A2C, // PANEL
		0x17223A, // RAISED
		0x1D2B49, // RAISED_HOVER
		0x23365C, // RAISED_ACTIVE
		0x26354F, // LINE
		0x1D2940, // DIVIDER
		0xE6ECF5, // TEXT
		0x8F9DB8, // TEXT_DIM
		0x56637D, // TEXT_FAINT
		0x4CC3FF, // ACCENT
		0x7BD3FF, // ACCENT_HOVER
		0x2EA6E6, // ACCENT_ACTIVE
		0x04131F, // ON_ACCENT
		0xFFB020, // CAUTION
		0xFF5A5A, // WARNING
		0x3DDC97, // SUCCESS
	};

	float resScale = 1.0f;  // from the window's height
	float userScale = 1.0f; // the user's interface size
	float applied = 0.0f;   // scale of the style in use
	Fonts fonts;

	float Linear (float c)
	{
		return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f);
	}

	void Build (float scale)
	{
		ImGuiStyle st; // (from the defaults: sizes scale from them)

		st.WindowPadding = ImVec2(16, 14);
		st.FramePadding = ImVec2(12, 6);
		st.CellPadding = ImVec2(8, 5);
		st.ItemSpacing = ImVec2(10, 8);
		st.ItemInnerSpacing = ImVec2(8, 6);
		st.TouchExtraPadding = ImVec2(0, 0);
		st.IndentSpacing = 18;
		st.ScrollbarSize = 12;
		st.GrabMinSize = 6; // (a slim slider handle: the value text stays readable)
		st.WindowRounding = 8;
		st.ChildRounding = 6;
		st.FrameRounding = 5;
		st.PopupRounding = 6;
		st.ScrollbarRounding = 6;
		st.GrabRounding = 3;
		st.TabRounding = 5;
		st.WindowBorderSize = 1;
		st.ChildBorderSize = 1;
		st.PopupBorderSize = 1;
		st.FrameBorderSize = 0;
		st.TabBorderSize = 0;
		st.TabBarBorderSize = 1;
		st.TabBarOverlineSize = 2;
		st.SeparatorTextBorderSize = 1;
		st.SeparatorTextPadding = ImVec2(0, 4);
		st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
		st.WindowMenuButtonPosition = ImGuiDir_None;
		st.WindowMinSize = ImVec2(120, 60);
		st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
		st.DisabledAlpha = 0.45f;

		ImVec4 *c = st.Colors;
		c[ImGuiCol_Text] = Col (TEXT);
		c[ImGuiCol_TextDisabled] = Col (TEXT_FAINT);
		c[ImGuiCol_WindowBg] = Col (WINDOW, 0.975f); // (blending is linear: a little lets a lot through)
		c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_PopupBg] = Col (PANEL, 0.98f);
		c[ImGuiCol_Border] = Col (LINE);
		c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_FrameBg] = Col (RAISED);
		c[ImGuiCol_FrameBgHovered] = Col (RAISED_HOVER);
		c[ImGuiCol_FrameBgActive] = Col (RAISED_ACTIVE);
		c[ImGuiCol_TitleBg] = Col (WINDOW, 0.96f);
		c[ImGuiCol_TitleBgActive] = Col (PANEL, 0.98f);
		c[ImGuiCol_TitleBgCollapsed] = Col (WINDOW, 0.80f);
		c[ImGuiCol_MenuBarBg] = Col (PANEL);
		c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ScrollbarGrab] = Col (LINE);
		c[ImGuiCol_ScrollbarGrabHovered] = Col (TEXT_FAINT);
		c[ImGuiCol_ScrollbarGrabActive] = Col (TEXT_DIM);
		c[ImGuiCol_CheckMark] = Col (ACCENT);
		c[ImGuiCol_SliderGrab] = Col (ACCENT);
		c[ImGuiCol_SliderGrabActive] = Col (ACCENT_HOVER);
		c[ImGuiCol_Button] = Col (RAISED);
		c[ImGuiCol_ButtonHovered] = Col (RAISED_HOVER);
		c[ImGuiCol_ButtonActive] = Col (RAISED_ACTIVE);
		c[ImGuiCol_Header] = Col (ACCENT, 0.13f);
		c[ImGuiCol_HeaderHovered] = Col (TEXT, 0.05f);
		c[ImGuiCol_HeaderActive] = Col (ACCENT, 0.20f);
		c[ImGuiCol_Separator] = Col (DIVIDER);
		c[ImGuiCol_SeparatorHovered] = Col (ACCENT, 0.6f);
		c[ImGuiCol_SeparatorActive] = Col (ACCENT);
		c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripHovered] = Col (ACCENT, 0.5f);
		c[ImGuiCol_ResizeGripActive] = Col (ACCENT);
		c[ImGuiCol_InputTextCursor] = Col (ACCENT);
		c[ImGuiCol_TabHovered] = Col (TEXT, 0.08f);
		c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TabSelected] = Col (RAISED);
		c[ImGuiCol_TabSelectedOverline] = Col (ACCENT);
		c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TabDimmedSelected] = Col (RAISED, 0.7f);
		c[ImGuiCol_TabDimmedSelectedOverline] = Col (ACCENT, 0.5f);
		c[ImGuiCol_PlotLines] = Col (ACCENT);
		c[ImGuiCol_PlotLinesHovered] = Col (ACCENT_HOVER);
		c[ImGuiCol_PlotHistogram] = Col (ACCENT);
		c[ImGuiCol_PlotHistogramHovered] = Col (ACCENT_HOVER);
		c[ImGuiCol_TableHeaderBg] = Col (PANEL);
		c[ImGuiCol_TableBorderStrong] = Col (LINE);
		c[ImGuiCol_TableBorderLight] = Col (DIVIDER);
		c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TableRowBgAlt] = Col (TEXT, 0.025f);
		c[ImGuiCol_TextLink] = Col (ACCENT);
		c[ImGuiCol_TextSelectedBg] = Col (ACCENT, 0.30f);
		c[ImGuiCol_TreeLines] = Col (DIVIDER);
		c[ImGuiCol_DragDropTarget] = Col (ACCENT);
		c[ImGuiCol_NavCursor] = Col (ACCENT);
		c[ImGuiCol_NavWindowingHighlight] = Col (ACCENT, 0.7f);
		c[ImGuiCol_NavWindowingDimBg] = Col (BACKDROP, 0.5f);
		c[ImGuiCol_ModalWindowDimBg] = Col (BACKDROP, 0.6f);

		st.ScaleAllSizes (scale);
		st.FontSizeBase = SIZE_BODY;
		st.FontScaleDpi = resScale;
		st.FontScaleMain = userScale;
		ImGui::GetStyle() = st;
		applied = scale;
	}

	ImFont *AddFont (const std::string &file, float size)
	{
		ImGuiIO &io = ImGui::GetIO();
		// (the text font's own glyphs in the private use area, Inter's
		// alternates, would hide the icons)
		static const ImWchar pua[] = { 0xe000, 0xf8ff, 0 };
		ImFontConfig tc;
		tc.GlyphExcludeRanges = pua;
		ImFont *f = io.Fonts->AddFontFromFileTTF (file.c_str(), size, &tc);
		if (!f) return nullptr;
		// icons: Lucide (the user interface), then Font Awesome (dialogs and
		// add-ons using IconsFontAwesome6.h); the two Font Awesome icons in
		// use that share Lucide's code points come from Font Awesome
		static const ImWchar exclude[] = { 0xe4be, 0xe4be, 0xe68f, 0xe68f, 0 };
		ImFontConfig lc;
		lc.MergeMode = true;
		lc.PixelSnapH = true;
		lc.GlyphExcludeRanges = exclude;
		lc.GlyphOffset = ImVec2(0, 1.5f);
		io.Fonts->AddFontFromFileTTF ("Fonts/Lucide.ttf", size, &lc);
		ImFontConfig fa;
		fa.MergeMode = true;
		fa.PixelSnapH = true;
		io.Fonts->AddFontFromFileTTF ("Fonts/fa-solid-900.ttf", size * 0.86f, &fa);
		return f;
	}

	/// The file of another weight of a font named "...-Regular.ttf"
	std::string Weight (const std::string &file, const char *weight)
	{
		size_t p = file.find ("-Regular");
		if (p == std::string::npos) return file;
		std::string w = file;
		w.replace (p + 1, 7, weight);
		FILE *f = fopen (w.c_str(), "rb");
		if (!f) return file;
		fclose (f);
		return w;
	}

} // namespace

ImVec4 Srgb (unsigned rgb, float alpha)
{
	return ImVec4(Linear (((rgb >> 16) & 0xff) / 255.0f), Linear (((rgb >> 8) & 0xff) / 255.0f),
		Linear ((rgb & 0xff) / 255.0f), alpha);
}

ImVec4 Col (Colour c, float alpha)
{
	return Srgb (PALETTE[c], alpha);
}

ImU32 Col32 (Colour c, float alpha)
{
	return ImGui::ColorConvertFloat4ToU32 (Col (c, alpha));
}

void Apply ()
{
	Build (resScale * userScale);
}

void UpdateScale (float displayHeight)
{
	if (displayHeight <= 0.0f) return;
	float s = std::clamp (displayHeight / 1080.0f, 0.75f, 4.0f);
	if (std::fabs (s - resScale) > 0.005f || std::fabs (resScale * userScale - applied) > 0.005f) {
		resScale = s;
		Build (resScale * userScale);
	}
}

float Scale ()
{
	return resScale * userScale;
}

void SetUserScale (float s)
{
	userScale = std::clamp (s, 0.6f, 2.0f);
}

float UserScale ()
{
	return userScale;
}

const Fonts &LoadFonts (const char *bodyFile, const char *monoFile, float size)
{
	fonts.body = AddFont (bodyFile, size);
	fonts.medium = AddFont (Weight (bodyFile, "Medium"), size);
	fonts.strong = AddFont (Weight (bodyFile, "SemiBold"), size);
	if (!fonts.medium) fonts.medium = fonts.body;
	if (!fonts.strong) fonts.strong = fonts.medium;
	fonts.mono = ImGui::GetIO().Fonts->AddFontFromFileTTF (monoFile, size);
	if (!fonts.mono) fonts.mono = fonts.body;
	return fonts;
}

const Fonts &GetFonts ()
{
	return fonts;
}

void PushFont (ImFont *f, float size)
{
	ImGui::PushFont (f, size);
}

// ======================================================================
// Widgets

bool SecondaryButton (const char *label, const ImVec2 &size);

bool PrimaryButton (const char *label, const ImVec2 &size)
{
	if (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) // (nothing to do: quiet, not a faded accent)
		return SecondaryButton (label, size);
	ImGui::PushStyleColor (ImGuiCol_Button, Col (ACCENT));
	ImGui::PushStyleColor (ImGuiCol_ButtonHovered, Col (ACCENT_HOVER));
	ImGui::PushStyleColor (ImGuiCol_ButtonActive, Col (ACCENT_ACTIVE));
	ImGui::PushStyleColor (ImGuiCol_Text, Col (ON_ACCENT));
	PushFont (fonts.strong ? fonts.strong : nullptr, ImGui::GetStyle().FontSizeBase);
	bool r = ImGui::Button (label, size);
	PopFont ();
	ImGui::PopStyleColor (4);
	return r;
}

bool SecondaryButton (const char *label, const ImVec2 &size)
{
	ImGui::PushStyleColor (ImGuiCol_Button, Col (RAISED, 0.0f));
	ImGui::PushStyleColor (ImGuiCol_ButtonHovered, Col (TEXT, 0.08f));
	ImGui::PushStyleColor (ImGuiCol_ButtonActive, Col (TEXT, 0.14f));
	ImGui::PushStyleColor (ImGuiCol_Border, Col (LINE));
	ImGui::PushStyleVar (ImGuiStyleVar_FrameBorderSize, std::max (1.0f, Scale ()));
	PushFont (fonts.medium ? fonts.medium : nullptr, ImGui::GetStyle().FontSizeBase);
	bool r = ImGui::Button (label, size);
	PopFont ();
	ImGui::PopStyleVar ();
	ImGui::PopStyleColor (4);
	return r;
}

void ReportItem (const char *label, int flags)
{
	ImGuiContext &g = *GImGui;
	if (g.TestEngineHookItems && label)
		ImGuiTestEngineHook_ItemInfo (&g, g.LastItemData.ID, label, g.LastItemData.StatusFlags | flags);
}

bool IconButton (const char *icon, const char *tooltip, bool active)
{
	ImGui::PushStyleColor (ImGuiCol_Button, active ? Col (ACCENT, 0.18f) : Col (RAISED, 0.0f));
	ImGui::PushStyleColor (ImGuiCol_ButtonHovered, Col (TEXT, 0.10f));
	ImGui::PushStyleColor (ImGuiCol_ButtonActive, Col (TEXT, 0.16f));
	ImGui::PushStyleColor (ImGuiCol_Text, active ? Col (ACCENT) : Col (TEXT_DIM));
	bool r = ImGui::Button (icon);
	if (tooltip) ReportItem (tooltip); // (an icon: named by its tooltip)
	ImGui::PopStyleColor (4);
	if (tooltip && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip ("%s", tooltip);
	return r;
}

void SectionHeader (const char *text)
{
	ImGui::Dummy (ImVec2(0, Px (4)));
	PushFont (fonts.medium, SIZE_SMALL);
	ImGui::PushStyleColor (ImGuiCol_Text, Col (TEXT_DIM));
	// (small capitals: upper case, spaced)
	std::string s;
	for (const char *p = text; *p; p++) { // (spaces between characters, not inside UTF-8 sequences)
		unsigned char c = (unsigned char)*p;
		s += c < 0x80 ? (char)toupper (c) : (char)c;
		if (p[1] && ((unsigned char)p[1] & 0xC0) != 0x80) s += ' ';
	}
	ImGui::TextUnformatted (s.c_str());
	ImGui::PopStyleColor ();
	PopFont ();
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float w = ImGui::GetContentRegionAvail().x;
	ImGui::GetWindowDrawList()->AddLine (ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), Col32 (DIVIDER), std::max (1.0f, Scale ()));
	ImGui::Dummy (ImVec2(0, Px (4)));
}

void TextDim (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	ImGui::PushStyleColor (ImGuiCol_Text, Col (TEXT_DIM));
	ImGui::TextWrappedV (fmt, args);
	ImGui::PopStyleColor ();
	va_end (args);
}

void Property (const char *label, const char *fmt, ...)
{
	ImGui::PushStyleColor (ImGuiCol_Text, Col (TEXT_DIM));
	ImGui::TextUnformatted (label);
	ImGui::PopStyleColor ();
	ImGui::SameLine ();
	va_list args;
	va_start (args, fmt);
	ImGui::TextV (fmt, args);
	va_end (args);
}

void Badge (const char *text, Colour c)
{
	PushFont (fonts.medium, SIZE_SMALL);
	ImVec2 ts = ImGui::CalcTextSize (text);
	ImVec2 pad (Px (8), Px (3));
	ImVec2 p = ImGui::GetCursorScreenPos ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	ImVec2 q (p.x + ts.x + 2 * pad.x, p.y + ts.y + 2 * pad.y);
	dl->AddRectFilled (p, q, Col32 (c, 0.16f), Px (4));
	dl->AddText (ImVec2(p.x + pad.x, p.y + pad.y), Col32 (c), text);
	ImGui::Dummy (ImVec2(q.x - p.x, q.y - p.y));
	PopFont ();
}

void KeyCap (const char *key)
{
	PushFont (fonts.mono, SIZE_SMALL);
	ImVec2 ts = ImGui::CalcTextSize (key);
	ImVec2 pad (Px (6), Px (2));
	ImVec2 p = ImGui::GetCursorScreenPos ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	ImVec2 q (p.x + ts.x + 2 * pad.x, p.y + ts.y + 2 * pad.y);
	dl->AddRectFilled (p, q, Col32 (RAISED, ImGui::GetStyle().Alpha), Px (4));
	dl->AddRect (p, q, Col32 (LINE, ImGui::GetStyle().Alpha), Px (4));
	dl->AddText (ImVec2(p.x + pad.x, p.y + pad.y), Col32 (TEXT, ImGui::GetStyle().Alpha), key);
	ImGui::Dummy (ImVec2(q.x - p.x, q.y - p.y));
	PopFont ();
}

bool Toggle (const char *label, bool *v)
{
	ImGuiWindow *win = ImGui::GetCurrentWindow ();
	if (win->SkipItems) return false;
	float h = ImGui::GetFrameHeight () * 0.78f, w = h * 1.8f;
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float y0 = p.y + (ImGui::GetFrameHeight () - h) * 0.5f;
	ImGui::PushID (label);
	ImVec2 ts = ImGui::CalcTextSize (label, nullptr, true);
	bool pressed = ImGui::InvisibleButton ("##toggle", ImVec2(w + (ts.x > 0 ? ImGui::GetStyle().ItemInnerSpacing.x + ts.x : 0), ImGui::GetFrameHeight ()));
	ReportItem (label, ImGuiItemStatusFlags_Checkable | (*v ? ImGuiItemStatusFlags_Checked : 0));
	if (pressed) *v = !*v;
	bool hov = ImGui::IsItemHovered ();
	ImGui::PopID ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	float a = ImGui::GetStyle().Alpha; // (dimmed when disabled)
	ImU32 track = *v ? Col32 (hov ? ACCENT_HOVER : ACCENT, a) : Col32 (hov ? RAISED_ACTIVE : RAISED_HOVER, a);
	dl->AddRectFilled (ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), track, h * 0.5f);
	float r = h * 0.5f - Px (3);
	float cx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
	dl->AddCircleFilled (ImVec2(cx, y0 + h * 0.5f), r, *v ? Col32 (ON_ACCENT, a) : Col32 (TEXT_DIM, a));
	if (ts.x > 0) {
		ImGui::RenderText (ImVec2(p.x + w + ImGui::GetStyle().ItemInnerSpacing.x,
			p.y + ImGui::GetStyle().FramePadding.y), label);
	}
	return pressed;
}

bool NavTab (const char *label, bool active)
{
	PushFont (fonts.medium, SIZE_LARGE);
	ImVec2 ts = ImGui::CalcTextSize (label, nullptr, true);
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float pad = Px (4), line = Px (2);
	bool pressed = ImGui::InvisibleButton (label, ImVec2(ts.x, ts.y + 2 * pad + line));
	bool hov = ImGui::IsItemHovered ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	dl->AddText (ImVec2(p.x, p.y + pad), active ? Col32 (TEXT) : hov ? Col32 (TEXT) : Col32 (TEXT_DIM), label,
		ImGui::FindRenderedTextEnd (label));
	if (active)
		dl->AddRectFilled (ImVec2(p.x, p.y + ts.y + 2 * pad), ImVec2(p.x + ts.x, p.y + ts.y + 2 * pad + line), Col32 (ACCENT), line);
	PopFont ();
	return pressed;
}

bool BeginPanel (const char *id, const ImVec2 &size, float alpha, ImGuiChildFlags flags, bool scroll)
// (alpha: the blend is linear, so a panel lets much more of a bright scene
// through than its alpha suggests)
{
	ImGui::PushStyleColor (ImGuiCol_ChildBg, Col (WINDOW, alpha));
	ImGui::PushStyleColor (ImGuiCol_Border, Col (LINE, 0.9f));
	ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, Px (10));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(Px (18), Px (16)));
	bool r = ImGui::BeginChild (id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | flags,
		scroll ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar (2);
	ImGui::PopStyleColor (2);
	return r;
}

bool BeginAutoPanel (const char *id, float width, float maxHeight, float alpha)
{
	ImGui::SetNextWindowSizeConstraints (ImVec2(width, 0), ImVec2(width, maxHeight));
	return BeginPanel (id, ImVec2(width, 0), alpha, ImGuiChildFlags_AutoResizeY, true);
}

bool ModalDim (const char *id, float alpha)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport ();
	ImGui::SetNextWindowPos (vp->Pos);
	ImGui::SetNextWindowSize (vp->Size);
	ImGui::SetNextWindowViewport (vp->ID);
	ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin (id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::PopStyleVar (2);
	ImGui::BringWindowToDisplayFront (ImGui::GetCurrentWindow ());
	ImGui::GetWindowDrawList ()->AddRectFilled (vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), Col32 (BACKDROP, alpha));
	bool clicked = ImGui::IsWindowHovered () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
	ImGui::End ();
	return clicked;
}

void BringToFront ()
{
	ImGui::BringWindowToDisplayFront (ImGui::GetCurrentWindow ());
}

bool SearchField (const char *id, const char *hint, char *buf, size_t size, float width, bool compact)
{
	ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2(Px (32), ImGui::GetStyle().FramePadding.y + (compact ? 0.0f : Px (2))));
	ImGui::SetNextItemWidth (width);
	ImGui::SetNextItemAllowOverlap (); // (the clear button over its right end)
	bool r = ImGui::InputTextWithHint (id, hint, buf, size);
	ImGui::PopStyleVar ();
	ImVec2 r0 = ImGui::GetItemRectMin (), r1 = ImGui::GetItemRectMax ();
	float ih = ImGui::GetFontSize ();
	ImGui::GetWindowDrawList()->AddText (ImVec2(r0.x + Px (10), (r0.y + r1.y - ih) * 0.5f), Col32 (TEXT_DIM), ICON_LC_SEARCH);
	if (buf[0]) { // (a clear button at the right end)
		// (on the field's line, over it: no cursor moved back below the field
		// afterwards, which ImGui reports as extending the window)
		ImVec2 cp (r1.x - ImGui::GetFrameHeight (), r0.y);
		ImGui::SameLine (0, 0);
		ImGui::SetCursorScreenPos (cp);
		ImGui::PushID (id);
		if (ImGui::InvisibleButton ("##clear", ImVec2(r1.x - cp.x, r1.y - r0.y))) { buf[0] = '\0'; r = true; }
		ReportItem ("Clear");
		bool hov = ImGui::IsItemHovered ();
		ImGui::PopID ();
		ImVec2 xs = ImGui::CalcTextSize ("\xC3\x97");
		ImGui::GetWindowDrawList()->AddText (ImVec2((cp.x + r1.x - xs.x) * 0.5f, (r0.y + r1.y - xs.y) * 0.5f),
			Col32 (hov ? TEXT : TEXT_DIM), "\xC3\x97");
	}
	return r;
}

bool Segmented (const char *id, const char *const *labels, int n, int *sel)
{
	bool changed = false;
	ImGui::PushID (id);
	float avail = ImGui::GetContentRegionAvail().x, pad = Px (3);
	float h = ImGui::GetFrameHeight ();
	ImVec2 p = ImGui::GetCursorScreenPos ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	float al = ImGui::GetStyle().Alpha; // (dimmed when disabled)
	dl->AddRectFilled (p, ImVec2(p.x + avail, p.y + h), Col32 (RAISED, al), ImGui::GetStyle().FrameRounding);
	float w = (avail - 2 * pad) / n;
	PushFont (fonts.medium, ImGui::GetStyle().FontSizeBase - 1.0f);
	for (int i = 0; i < n; i++) {
		ImVec2 a (p.x + pad + i * w, p.y + pad), b (a.x + w, p.y + h - pad);
		ImGui::SetCursorScreenPos (a);
		ImGui::PushID (i);
		if (ImGui::InvisibleButton ("##seg", ImVec2(w, b.y - a.y)) && *sel != i) { *sel = i; changed = true; }
		ReportItem (labels[i], ImGuiItemStatusFlags_Checkable | (*sel == i ? ImGuiItemStatusFlags_Checked : 0));
		bool hov = ImGui::IsItemHovered ();
		ImGui::PopID ();
		if (*sel == i) dl->AddRectFilled (a, b, Col32 (ACCENT, 0.22f * al), std::max (0.0f, ImGui::GetStyle().FrameRounding - pad * 0.5f));
		else if (hov) dl->AddRectFilled (a, b, Col32 (TEXT, 0.06f * al), std::max (0.0f, ImGui::GetStyle().FrameRounding - pad * 0.5f));
		const char *end = ImGui::FindRenderedTextEnd (labels[i]);
		ImVec2 ts = ImGui::CalcTextSize (labels[i], end);
		ImGui::PushClipRect (a, b, true);
		dl->AddText (ImVec2(std::max (a.x + Px (4), (a.x + b.x - ts.x) * 0.5f), (a.y + b.y - ts.y) * 0.5f),
			Col32 (*sel == i ? ACCENT : hov ? TEXT : TEXT_DIM, al), labels[i], end);
		ImGui::PopClipRect ();
	}
	PopFont ();
	ImGui::SetCursorScreenPos (p);
	ImGui::Dummy (ImVec2(avail, h));
	ImGui::PopID ();
	return changed;
}

namespace {
	// (the slider: ImGui's for the interaction, drawn here)
	template <typename T>
	bool SliderT (const char *id, T *v, T vmin, T vmax, const char *format, float width)
	{
		if (width < 0.0f) width = ImGui::GetContentRegionAvail().x;
		char text[64];
		ImFormatString (text, sizeof(text), format, *v);
		ImGui::PushFont (fonts.mono, ImGui::GetStyle().FontSizeBase);
		char wide[64];
		ImFormatString (wide, sizeof(wide), format, std::fabs ((double)vmax) > std::fabs ((double)vmin) ? vmax : vmin);
		float vw = std::max (ImGui::CalcTextSize (wide).x, ImGui::CalcTextSize (text).x) + Px (4);
		ImGui::PopFont ();
		float gap = Px (12), sw = std::max (Px (40), width - vw - gap);
		float r = Px (7);
		float h = ImGui::GetFrameHeight ();
		ImVec2 p = ImGui::GetCursorScreenPos ();
		// (ImGui's grab: its centre r from the ends; GrabMinSize = 2r - 2 x its padding of 2)
		ImGui::PushStyleVar (ImGuiStyleVar_GrabMinSize, 2 * r - 4.0f);
		ImGui::PushStyleColor (ImGuiCol_FrameBg, IM_COL32 (0, 0, 0, 0));
		ImGui::PushStyleColor (ImGuiCol_FrameBgHovered, IM_COL32 (0, 0, 0, 0));
		ImGui::PushStyleColor (ImGuiCol_FrameBgActive, IM_COL32 (0, 0, 0, 0));
		ImGui::PushStyleColor (ImGuiCol_SliderGrab, IM_COL32 (0, 0, 0, 0));
		ImGui::PushStyleColor (ImGuiCol_SliderGrabActive, IM_COL32 (0, 0, 0, 0));
		ImGui::PushStyleColor (ImGuiCol_Text, IM_COL32 (0, 0, 0, 0));
		ImGui::SetNextItemWidth (sw);
		bool changed;
		if constexpr (std::is_same_v<T, int>) changed = ImGui::SliderInt (id, v, vmin, vmax, format, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
		else changed = ImGui::SliderFloat (id, v, vmin, vmax, format, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
		ImGui::PopStyleColor (6);
		ImGui::PopStyleVar ();
		bool hov = ImGui::IsItemHovered (), act = ImGui::IsItemActive ();
		bool disabled = (ImGui::GetItemFlags () & ImGuiItemFlags_Disabled) != 0;
		float t = vmax != vmin ? std::clamp ((float)(*v - vmin) / (float)(vmax - vmin), 0.0f, 1.0f) : 0.0f;
		ImDrawList *dl = ImGui::GetWindowDrawList ();
		float cy = p.y + h * 0.5f, th = Px (4);
		// (where ImGui puts its grab's centre: an integer slider's grab is wider for small ranges)
		float gsz = 2 * r - 4.0f;
		if constexpr (std::is_same_v<T, int>) gsz = std::min (std::max ((sw - 4.0f) / (float)(std::abs (vmax - vmin) + 1), gsz), sw - 4.0f);
		float e = 2.0f + gsz * 0.5f;
		float x0 = p.x + e, x1 = p.x + sw - e, kx = x0 + t * (x1 - x0);
		float alpha = ImGui::GetStyle().Alpha;
		dl->AddRectFilled (ImVec2(x0 - th * 0.5f, cy - th * 0.5f), ImVec2(x1 + th * 0.5f, cy + th * 0.5f), Col32 (RAISED_HOVER, alpha), th);
		dl->AddRectFilled (ImVec2(x0 - th * 0.5f, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), Col32 (disabled ? TEXT_FAINT : ACCENT, alpha), th);
		if (hov || act) dl->AddCircleFilled (ImVec2(kx, cy), r + Px (4), Col32 (ACCENT, 0.18f * alpha));
		dl->AddCircleFilled (ImVec2(kx, cy), r, Col32 (disabled ? TEXT_DIM : act ? ACCENT_ACTIVE : TEXT, alpha));
		// the value
		ImFormatString (text, sizeof(text), format, *v);
		ImGui::PushFont (fonts.mono, ImGui::GetStyle().FontSizeBase);
		ImVec2 ts = ImGui::CalcTextSize (text);
		dl->AddText (ImVec2(p.x + width - ts.x, cy - ts.y * 0.5f), Col32 (act ? TEXT : TEXT_DIM, alpha), text);
		ImGui::PopFont ();
		// (the value is outside the item, which stays the last one for IsItem* queries)
		ImGuiWindow *win = ImGui::GetCurrentWindow ();
		win->DC.CursorMaxPos.x = std::max (win->DC.CursorMaxPos.x, p.x + width);
		return changed;
	}
}

bool Slider (const char *id, float *v, float vmin, float vmax, const char *format, float width)
{
	return SliderT<float> (id, v, vmin, vmax, format, width);
}

bool Slider (const char *id, int *v, int vmin, int vmax, const char *format, float width)
{
	return SliderT<int> (id, v, vmin, vmax, format, width);
}

bool BeginCombo (const char *id, const char *preview, ImGuiComboFlags flags)
{
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(Px (6), Px (6)));
	ImGui::PushStyleVar (ImGuiStyleVar_PopupRounding, Px (8));
	bool open = ImGui::BeginCombo (id, preview, flags);
	ImGui::PopStyleVar (2);
	if (open) ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Px (2)));
	return open;
}

void EndCombo ()
{
	ImGui::PopStyleVar ();
	ImGui::EndCombo ();
}

bool ComboItem (const char *label, bool selected)
{
	float h = ImGui::GetFrameHeight ();
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float w = ImGui::GetContentRegionAvail().x;
	ImGui::PushID (label);
	ImGui::PushStyleColor (ImGuiCol_Header, Col (ACCENT, 0.16f));
	ImGui::PushStyleColor (ImGuiCol_HeaderHovered, Col (TEXT, 0.08f));
	ImGui::PushStyleColor (ImGuiCol_HeaderActive, Col (TEXT, 0.12f));
	bool r = ImGui::Selectable ("##item", selected, 0, ImVec2(w, h));
	ImGui::PopStyleColor (3);
	ImGui::PopID ();
	if (selected && ImGui::IsWindowAppearing ()) ImGui::SetItemDefaultFocus (), ImGui::SetScrollHereY ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	const char *end = ImGui::FindRenderedTextEnd (label);
	float ty = p.y + (h - ImGui::GetTextLineHeight ()) * 0.5f;
	dl->AddText (ImVec2(p.x + Px (8), ty), Col32 (selected ? ACCENT : TEXT), label, end);
	if (selected) {
		ImVec2 cs = ImGui::CalcTextSize (ICON_LC_CHECK);
		dl->AddText (ImVec2(p.x + w - cs.x - Px (8), ty), Col32 (ACCENT), ICON_LC_CHECK);
	}
	return r;
}

bool ListRow (const char *id, const char *icon, const char *label, const char *detail, bool selected, bool *activated, Colour iconColour)
{
	float h = ImGui::GetFrameHeight () + Px (4);
	ImGui::PushID (id);
	ImVec2 p = ImGui::GetCursorScreenPos ();
	float w = ImGui::GetContentRegionAvail().x;
	bool clicked = ImGui::InvisibleButton ("##row", ImVec2(w, h));
	ReportItem (label, selected ? ImGuiItemStatusFlags_Checkable | ImGuiItemStatusFlags_Checked : 0);
	bool hov = ImGui::IsItemHovered ();
	if (activated) *activated = hov && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left);
	if (ImGui::IsItemFocused () && ImGui::IsKeyPressed (ImGuiKey_Enter, false) && activated) *activated = true;
	ImGui::PopID ();
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	ImVec2 q (p.x + w, p.y + h);
	if (selected) {
		dl->AddRectFilled (p, q, Col32 (ACCENT, 0.13f), Px (5));
		dl->AddRectFilled (p, ImVec2(p.x + Px (3), q.y), Col32 (ACCENT), Px (2));
	} else if (hov) dl->AddRectFilled (p, q, Col32 (TEXT, 0.05f), Px (5));
	float ty = p.y + (h - ImGui::GetFontSize ()) * 0.5f, x = p.x + Px (10);
	if (icon) {
		dl->AddText (ImVec2(x, ty), Col32 (iconColour), icon);
		x += ImGui::GetFontSize () * 1.6f;
	}
	float dw = 0;
	if (detail && *detail) {
		PushFont (fonts.mono, SIZE_SMALL);
		ImVec2 ds = ImGui::CalcTextSize (detail);
		dw = ds.x + Px (10);
		dl->AddText (ImVec2(q.x - ds.x - Px (10), p.y + (h - ds.y) * 0.5f), Col32 (TEXT_DIM), detail);
		PopFont ();
	}
	ImGui::PushClipRect (ImVec2(x, p.y), ImVec2(q.x - dw - Px (6), q.y), true);
	dl->AddText (ImVec2(x, ty), Col32 (selected ? TEXT : TEXT, selected ? 1.0f : 0.88f), label);
	ImGui::PopClipRect ();
	return clicked;
}

void ScrollFade ()
{
	ImGuiWindow *win = ImGui::GetCurrentWindow ();
	float sy = win->Scroll.y, my = win->ScrollMax.y;
	if (my <= 0.0f) return;
	const ImRect &r = win->InnerClipRect;
	float h = std::min (Px (28), r.GetHeight () * 0.25f);
	ImDrawList *dl = win->DrawList;
	ImU32 clear = Col32 (WINDOW, 0.0f), solid = Col32 (WINDOW, 0.95f);
	if (sy > 0.5f) // (more above)
		dl->AddRectFilledMultiColor (r.Min, ImVec2(r.Max.x, r.Min.y + h), solid, solid, clear, clear);
	if (sy < my - 0.5f) // (more below)
		dl->AddRectFilledMultiColor (ImVec2(r.Min.x, r.Max.y - h), r.Max, clear, clear, solid, solid);
}

void EndPanel ()
{
	ScrollFade ();
	ImGui::EndChild ();
}

} // namespace ui
