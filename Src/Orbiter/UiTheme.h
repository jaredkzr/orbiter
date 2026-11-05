// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The look of Orbiter's user interface (ImGui): the "mission control"
// palette (navy and graphite panels, one cyan accent; amber and red only
// for cautions and warnings), sizes scaled with the window's resolution
// and the user's interface size, the fonts, and widgets used across the
// launchpad and the dialogs.
//
// Colours are given in sRGB and converted: the user interface is drawn
// into an sRGB render target, which takes linear values.
// =======================================================================

#ifndef __UITHEME_H
#define __UITHEME_H

#include "imgui.h"

namespace ui {

	// ---- palette -------------------------------------------------------

	enum Colour {
		BACKDROP,      // behind everything (no scene)
		WINDOW,        // window background (translucent over the scene)
		PANEL,         // a panel or child region
		RAISED,        // inputs, neutral buttons
		RAISED_HOVER,
		RAISED_ACTIVE,
		LINE,          // borders
		DIVIDER,       // separators
		TEXT,
		TEXT_DIM,      // secondary text, labels
		TEXT_FAINT,    // disabled
		ACCENT,        // cyan: selection, focus, primary action
		ACCENT_HOVER,
		ACCENT_ACTIVE,
		ON_ACCENT,     // text on an accent fill
		CAUTION,       // amber
		WARNING,       // red
		SUCCESS,       // green
		NCOLOUR
	};

	/// A palette colour as a linear ImGui colour, with alpha
	ImVec4 Col (Colour c, float alpha = 1.0f);
	ImU32 Col32 (Colour c, float alpha = 1.0f);
	/// An sRGB colour (0xRRGGBB) as a linear ImGui colour
	ImVec4 Srgb (unsigned rgb, float alpha = 1.0f);

	// ---- style and scale -----------------------------------------------

	/// Set the palette and sizes (once the ImGui context exists)
	void Apply ();
	/// Each frame, before ImGui::NewFrame: the scale follows the window's
	/// height (1 at 1080 lines) times the user's interface size
	void UpdateScale (float displayHeight);
	/// The current scale (1 at 1080 lines and interface size 100 %)
	float Scale ();
	/// A length [pixels at 1080 lines] at the current scale
	inline float Px (float px) { return px * Scale (); }
	/// The user's interface size (1: 100 %), from the configuration
	void SetUserScale (float s);
	float UserScale ();

	// ---- fonts ---------------------------------------------------------

	struct Fonts {
		ImFont *body = nullptr;     // Inter Regular (+ icons)
		ImFont *medium = nullptr;   // Inter Medium (+ icons): labels, buttons
		ImFont *strong = nullptr;   // Inter SemiBold (+ icons): titles, headings
		ImFont *mono = nullptr;     // JetBrains Mono: numbers, readouts
	};
	/// Load the fonts (body: the configured default font file; mono: the
	/// configured monospaced font). Returns the fonts, also kept for Font().
	const Fonts &LoadFonts (const char *bodyFile, const char *monoFile, float size);
	const Fonts &GetFonts ();

	/// Text sizes [pixels at 1080 lines, before the user's size]
	constexpr float SIZE_SMALL = 12.5f, SIZE_BODY = 15.0f, SIZE_LARGE = 18.0f, SIZE_TITLE = 24.0f, SIZE_DISPLAY = 34.0f;
	/// Push a font at a size [pixels at 1080 lines]
	void PushFont (ImFont *f, float size = SIZE_BODY);
	inline void PopFont () { ImGui::PopFont (); }

	// ---- widgets -------------------------------------------------------

	/// The main action of a view (accent fill)
	bool PrimaryButton (const char *label, const ImVec2 &size = ImVec2(0, 0));
	/// A secondary action (outlined)
	bool SecondaryButton (const char *label, const ImVec2 &size = ImVec2(0, 0));
	/// A borderless icon button with a tooltip
	bool IconButton (const char *icon, const char *tooltip, bool active = false);
	/// A small caps heading over a group of controls
	void SectionHeader (const char *text);
	/// Secondary text
	void TextDim (const char *fmt, ...) IM_FMTARGS(1);
	/// A label and a value on one row (label dimmed)
	void Property (const char *label, const char *fmt, ...) IM_FMTARGS(2);
	/// A small rounded tag
	void Badge (const char *text, Colour c);
	/// A switch (an on/off control with a label to its right)
	bool Toggle (const char *label, bool *v);
	/// A text tab: returns true when clicked; drawn with an accent underline when active
	bool NavTab (const char *label, bool active);
	/// A panel: a child region with the panel colour, rounded and bordered
	/// (flags: e.g. ImGuiChildFlags_AutoResizeY with size.y 0; scroll: a
	/// scroll bar when the content is larger)
	bool BeginPanel (const char *id, const ImVec2 &size, float alpha = 0.95f, ImGuiChildFlags flags = 0, bool scroll = false);
	/// A panel as high as its content, up to maxHeight (then it scrolls)
	bool BeginAutoPanel (const char *id, float width, float maxHeight, float alpha = 0.95f);
	void EndPanel ();
	/// A modal backdrop: dims the view and every window under it (call
	/// before the modal window's Begin, then BringToFront after it).
	/// Returns true when it was clicked (outside the modal window)
	bool ModalDim (const char *id, float alpha = 0.7f);
	/// Draw the current window over all others (after its Begin)
	void BringToFront ();
	/// A search field with an icon and a clear button (full width by
	/// default; compact: as high as the other controls, for a toolbar)
	bool SearchField (const char *id, const char *hint, char *buf, size_t size, float width = -FLT_MIN, bool compact = false);
	/// A segmented control: one of n options, the chosen one filled (full width)
	bool Segmented (const char *id, const char *const *labels, int n, int *sel);
	/// A row of a list: icon, label, a detail right-aligned (mono, dimmed),
	/// an accent bar when selected. Returns true when clicked; *activated
	/// (if given): double-clicked or Enter
	bool ListRow (const char *id, const char *icon, const char *label, const char *detail, bool selected,
		bool *activated = nullptr, Colour iconColour = TEXT_DIM);
	/// A slider: a thin track filled up to a round knob, the value printed
	/// to its right (width: the whole control, value included)
	bool Slider (const char *id, float *v, float vmin, float vmax, const char *format, float width = -FLT_MIN);
	bool Slider (const char *id, int *v, int vmin, int vmax, const char *format, float width = -FLT_MIN);
	/// A combo box whose list has room around its items (ComboItem); call
	/// EndCombo if it returns true
	bool BeginCombo (const char *id, const char *preview, ImGuiComboFlags flags = 0);
	void EndCombo ();
	/// An item of a combo box's list: as high as a control, the label
	/// inset, a check mark when selected. Returns true when clicked.
	bool ComboItem (const char *label, bool selected);
	/// Fades the top and bottom of the current scrolling window where there
	/// is more to scroll to (call before its End/EndChild)
	void ScrollFade ();

	/// A panel that fills the window down to the items drawn below it (a
	/// footer): their height is measured as they are drawn, for the next
	/// frame. Size the panel with Height(), call PanelEnd() after it and
	/// FooterEnd() after the footer's last item.
	struct Fill {
		float footer = 0.0f, bottom = 0.0f;
		float Height (float guess) const { return -(footer > 0.0f ? footer : guess); }
		void PanelEnd () { bottom = ImGui::GetItemRectMax().y; }
		void FooterEnd () { footer = (float)(int)(ImGui::GetItemRectMax().y - bottom + 0.999f); }
	};

}

#endif // !__UITHEME_H
