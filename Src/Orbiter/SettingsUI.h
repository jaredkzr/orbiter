// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The settings screen: the same pages on the launchpad and in flight
// (the Settings dialog, DlgOptions). Each setting is a row: its name and
// a sentence about it on the left, its control on the right. Settings
// that only apply when a scenario starts are marked in flight.
// The Graphics page lists the graphics client's own settings
// (GraphicsClient::clbkGetSettings) with quality presets.
// =======================================================================

#ifndef __SETTINGSUI_H
#define __SETTINGSUI_H

#include "UiTheme.h"
#include <string>
#include <utility>
#include <vector>

class Orbiter;
class CelestialBody;
namespace oapi { struct GraphicsSetting; }

class SettingsUI {
public:
	enum Where { LAUNCHPAD, FLIGHT };
	enum Page { DISPLAY, GRAPHICS, SCENERY, SIMULATION, INSTRUMENTS, CONTROLS, OVERLAYS, NPAGE };

	SettingsUI (Orbiter *orbiter, Where where);

	/// Draw the page list and the selected page into a w x h region at
	/// the cursor. panels: each on a panel of its own (over a backdrop),
	/// else on the window's background.
	void Draw (float w, float h, bool panels);

	void SelectPage (Page p) { page = p; scrollTop = true; }
	Page CurrentPage () const { return page; }
	static const char *PageName (Page p);

private:
	bool Shown (Page p) const { return p != OVERLAYS || where == FLIGHT; }
	void DrawNav ();
	void DrawPage ();

	void PageDisplay ();
	void PageGraphics ();
	void PageScenery ();
	void PageSimulation ();
	void PageInstruments ();
	void PageControls ();
	void PageOverlays ();

	// ---- rows -----------------------------------------------------------
	/// A row: its label and help text on the left, the control on the
	/// right (drawn by the caller between BeginRow and EndRow, ctrlW wide).
	/// status: a note after the label (e.g. "Applies at next launch").
	void BeginRow (const char *label, const char *help, float ctrlW, const char *status = nullptr, ui::Colour statusColour = ui::TEXT_DIM);
	void EndRow ();
	float ControlWidth () const;
	bool ToggleRow (const char *label, const char *help, bool *v, const char *status = nullptr);
	bool FlagRow (const char *label, const char *help, int *flags, int bit);
	bool SliderRow (const char *label, const char *help, float *v, float vmin, float vmax, const char *format);
	bool SliderRow (const char *label, const char *help, double *v, double vmin, double vmax, const char *format);
	bool SliderRow (const char *label, const char *help, int *v, int vmin, int vmax, const char *format);
	bool ComboRow (const char *label, const char *help, int *sel, const char *const *items, int n);
	/// A note over a page or section (an icon and dimmed text)
	void Note (const char *icon, const char *text, ui::Colour c = ui::TEXT_DIM);
	void Section (const char *title, const char *status = nullptr);
	/// "Applies at next launch" in flight
	const char *NextLaunch () const;

	// ---- the graphics client's settings ----------------------------------
	void ClientRow (int idx);
	int MatchingPreset () const;

	// ---- overlays --------------------------------------------------------
	void SkyRows ();
	void AddBodyNode (const CelestialBody *body);

	Orbiter *orbiter;
	Where where;
	Page page = DISPLAY;
	bool scrollTop = false;
	int overlayTab = 0;
	std::string featureBody = "Earth"; // labels: the body whose surface features are listed

	struct Row { float x0, y0, w, labelW, ctrlW, h; } row{};
	const oapi::GraphicsSetting *gs = nullptr; // the client's settings (this frame)
	int ngs = 0;
};

#endif // !__SETTINGSUI_H
