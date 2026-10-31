// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
//                     ORBITER SOFTWARE DEVELOPMENT KIT
// MFDUI.h
// Widget toolkit for interactive MFD displays
// ======================================================================

/**
 * \file MFDUI.h
 * \brief Widgets for MFD displays the user can click, drag and scroll on.
 *
 * An immediate-mode toolkit: the MFD draws its widgets in every update,
 * and each widget function reports what the user did with it since the
 * last update (a button was clicked, a number was changed, a point of a
 * plot was picked). Pointer events reach the widgets automatically (see
 * MFD3); widgets are identified by a string unique within the display.
 *
 * \code
 * bool MyMFD::Update (oapi::Sketchpad *skp)
 * {
 *     mfdui::Context &ui = UI();
 *     ui.Begin (skp, W, H);
 *     mfdui::Rect r = ui.Area().Inset (ui.Unit()*0.5f);
 *     if (ui.Button ("go", r.Top (ui.Unit()*1.6f).Left (r.W()*0.3f), "Go")) Go();
 *     ui.Number ("alt", ..., &alt, spec);
 *     ui.End ();
 *     return true;
 * }
 * \endcode
 *
 * The widgets draw with the graphics client's anti-aliased functions where
 * available (oapi::Sketchpad version 4), else with the basic ones.
 */

#ifndef __MFDUI_H
#define __MFDUI_H

#include "OrbiterAPI.h"
#include "DrawAPI.h"
#include "MFDAPI.h"

namespace mfdui {

/// A rectangle in display pixels
struct Rect {
	float x0, y0, x1, y1;
	Rect (): x0(0), y0(0), x1(0), y1(0) {}
	Rect (float _x0, float _y0, float _x1, float _y1): x0(_x0), y0(_y0), x1(_x1), y1(_y1) {}
	float W () const { return x1 - x0; }
	float H () const { return y1 - y0; }
	float CX () const { return 0.5f * (x0 + x1); }
	float CY () const { return 0.5f * (y0 + y1); }
	bool Contains (float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
	Rect Inset (float d) const { return Rect (x0 + d, y0 + d, x1 - d, y1 - d); }
	Rect Inset (float dx, float dy) const { return Rect (x0 + dx, y0 + dy, x1 - dx, y1 - dy); }
	/// slices of the rectangle: the top, bottom, left or right part of the given size
	Rect Top (float h) const { return Rect (x0, y0, x1, y0 + h); }
	Rect Bottom (float h) const { return Rect (x0, y1 - h, x1, y1); }
	Rect Left (float w) const { return Rect (x0, y0, x0 + w, y1); }
	Rect Right (float w) const { return Rect (x1 - w, y0, x1, y1); }
	/// what remains after cutting off the top, bottom, left or right part
	Rect CutTop (float h) const { return Rect (x0, y0 + h, x1, y1); }
	Rect CutBottom (float h) const { return Rect (x0, y0, x1, y1 - h); }
	Rect CutLeft (float w) const { return Rect (x0 + w, y0, x1, y1); }
	Rect CutRight (float w) const { return Rect (x0, y0, x1 - w, y1); }
	/// i-th of n equal rows (top to bottom) or columns (left to right), with gaps between them
	Rect Row (int i, int n, float gap = 0.0f) const { float h = (H() - gap*(n-1)) / n; return Rect (x0, y0 + i*(h+gap), x1, y0 + i*(h+gap) + h); }
	Rect Col (int i, int n, float gap = 0.0f) const { float w = (W() - gap*(n-1)) / n; return Rect (x0 + i*(w+gap), y0, x0 + i*(w+gap) + w, y1); }
};

/// Colour roles of the display's theme (colours 0xAABBGGRR; alpha 0: opaque)
enum Role {
	TEXT,     ///< standard text and lines (MFD green)
	DIM,      ///< secondary text, grid lines
	ACCENT,   ///< highlighted values, selection (yellow)
	ALERT,    ///< warnings (red)
	INFO,     ///< auxiliary data (blue)
	BRIGHT,   ///< emphasis (white)
	FRAME,    ///< widget outlines
	FILL,     ///< widget backgrounds
	ACTIVE,   ///< background of pressed or selected widgets
	BACK,     ///< display background
	NROLES
};

/// Text alignment: one horizontal | one vertical flag
enum Align { ALIGN_LEFT = 0, ALIGN_CENTRE = 1, ALIGN_RIGHT = 2, ALIGN_TOP = 0, ALIGN_MIDDLE = 4, ALIGN_BOTTOM = 8 };

/// Text sizes
enum Font { FONT_SMALL, FONT_NORMAL, FONT_LARGE, FONT_MONO, NFONTS };

/// Number field parameters
struct NumberSpec {
	double step;       ///< change per wheel notch or drag step (in the stored unit)
	double lo, hi;     ///< range (lo >= hi: unlimited)
	const char *fmt;   ///< printf format of the displayed value (e.g. "%.1f")
	const char *unit;  ///< unit shown after the value (or NULL)
	double scale;      ///< displayed value = stored value * scale (e.g. 1e-3 for km from m)
	bool log;          ///< steps multiply the value by (1 + step) instead of adding
	NumberSpec (double _step = 1.0, double _lo = 0.0, double _hi = 0.0, const char *_fmt = "%g", const char *_unit = 0, double _scale = 1.0, bool _log = false)
		: step(_step), lo(_lo), hi(_hi), fmt(_fmt), unit(_unit), scale(_scale), log(_log) {}
};

/// Axis label formats of plots
enum AxisFormat {
	AXIS_NUMBER, ///< plain numbers
	AXIS_DATE,   ///< values are MJD: calendar dates
	AXIS_DAYS    ///< durations in days
};

/// Plot parameters
struct PlotSpec {
	double x0, x1, y0, y1;  ///< initial (and reset) view [data units]
	const char *xlabel;     ///< axis titles (or NULL)
	const char *ylabel;
	int xfmt, yfmt;         ///< AxisFormat of the tick labels
	bool zoom;              ///< the wheel zooms about the pointer (over an axis: that axis only)
	bool pan;               ///< dragging pans
	PlotSpec (double _x0 = 0, double _x1 = 1, double _y0 = 0, double _y1 = 1)
		: x0(_x0), x1(_x1), y0(_y0), y1(_y1), xlabel(0), ylabel(0), xfmt(AXIS_NUMBER), yfmt(AXIS_NUMBER), zoom(true), pan(true) {}
};

/// Colour maps of heatmaps
enum Colormap {
	CMAP_COST,  ///< blue (low) - cyan - green - yellow - red (high), for costs like delta-v
	CMAP_GREY
};

/// What happened to an Area (bit flags)
enum AreaFlag {
	AREA_HOVER   = 1,  ///< the pointer is over it
	AREA_PRESSED = 2,  ///< the left button was pressed on it and is held
	AREA_CLICKED = 4,  ///< clicked (pressed and released without dragging)
	AREA_DRAG    = 8,  ///< being dragged (the movement since the last frame in dx, dy)
	AREA_WHEEL   = 16  ///< the wheel turned over it (notches in wheel)
};

/// Marker shapes
enum Marker { MARK_CIRCLE, MARK_CROSS, MARK_DIAMOND, MARK_SQUARE, MARK_TRIANGLE };

/**
 * \brief The toolkit's state for one MFD display.
 * \note Obtained from MFD3::UI() for MFD modes of modules.
 */
class OAPIFUNC Context {
public:
	Context ();
	~Context ();

	/// \name Frame
	//@{
	/// Start drawing the display (w x h pixels) on a sketchpad
	void Begin (oapi::Sketchpad *skp, float w, float h);
	/// Finish drawing (draws open popups)
	void End ();
	oapi::Sketchpad *Skp () const;
	/// The whole display
	Rect Area () const;
	/// Height of a line of normal text [pixels]: the unit of layouts
	float Unit () const;
	/// Colour of a role of the theme
	DWORD Colour (int role) const;
	void SetColour (int role, DWORD col);
	//@}

	/// \name Input (passed in by Orbiter)
	//@{
	/// A pointer event; true if it concerns a widget (or an open popup)
	bool Pointer (const MFDPOINTER &p);
	/// A key (OAPI_KEY_xxx) while a field is being edited; true if consumed
	bool Key (DWORD key);
	/// A field is being edited: keys should go to Key
	bool Editing () const;
	/// Something changed that should be drawn soon (hover, drag, popup)
	bool Dirty () const;
	/// A drag is in progress (draw at the frame rate)
	bool Dragging () const;
	//@}

	/// \name Drawing
	//@{
	void Line (float x0, float y0, float x1, float y1, DWORD col, float width = 1.0f);
	void Polyline (const oapi::FVECTOR2 *pt, int n, DWORD col, float width = 1.0f, bool closed = false);
	void FillPolygon (const oapi::FVECTOR2 *pt, int n, DWORD col);
	void FillRect (const Rect &r, DWORD col);
	void FrameRect (const Rect &r, DWORD col, float width = 1.0f);
	void Circle (float cx, float cy, float rad, DWORD col, float width = 1.0f, bool fill = false);
	/// Text at (x,y) aligned (Align flags); returns its width
	float Text (float x, float y, const char *str, DWORD col, int align = ALIGN_LEFT|ALIGN_TOP, int font = FONT_NORMAL);
	/// Text in a rectangle, aligned within it, cut to fit
	void TextIn (const Rect &r, const char *str, DWORD col, int align = ALIGN_LEFT|ALIGN_MIDDLE, int font = FONT_NORMAL);
	float TextWidth (const char *str, int font = FONT_NORMAL);
	float TextHeight (int font = FONT_NORMAL) const;
	/// Image of w x h pixels (0xAABBGGRR, rows from the top) in a rectangle
	void Image (const DWORD *pix, int w, int h, const Rect &r, bool smooth = true);
	/// Restrict drawing to a rectangle (NULL: the whole display)
	void Clip (const Rect *r);
	//@}

	/// \name Widgets
	/// Each returns true when the user changed it (or clicked it).
	//@{
	bool Button (const char *id, const Rect &r, const char *label, bool selected = false, bool enabled = true);
	/// Check box with a label
	bool Toggle (const char *id, const Rect &r, const char *label, bool *v);
	/// A row of tabs; *sel: the selected one
	bool Tabs (const char *id, const Rect &r, const char *const *labels, int n, int *sel);
	/// A field showing items[*sel]; a click opens a list to choose from
	bool Choice (const char *id, const Rect &r, const char *const *items, int n, int *sel);
	/// A number: wheel or horizontal drag change it by steps, a click opens a keypad
	bool Number (const char *id, const Rect &r, double *v, const NumberSpec &spec);
	/// A scrolling list; *sel: the selected item (-1: none)
	bool List (const char *id, const Rect &r, const char *const *items, int n, int *sel);
	/// An interactive area the MFD draws itself (graphs, markers). Areas
	/// registered later are on top of earlier ones. Returns AreaFlag bits.
	int Area (const char *id, const Rect &r, float *dx = 0, float *dy = 0, float *wheel = 0);
	/// The pointer position (false if the pointer isn't over the display)
	bool PointerPos (float *x, float *y) const;
	/// Position of the last click (of an Area that returned AREA_CLICKED)
	void ClickPos (float *x, float *y) const;
	/// Was the last widget hovered by the pointer?
	bool Hovered () const;
	/// Tooltip shown while the last widget is hovered
	void Tooltip (const char *text);
	/// Tooltip shown at the pointer in this frame (for Areas)
	void ShowTooltip (const char *text);
	//@}

	/// \name Plots
	/// Between BeginPlot and EndPlot, the Plot functions draw in data coordinates.
	//@{
	bool BeginPlot (const char *id, const Rect &r, const PlotSpec &spec);
	void PlotLine (const double *x, const double *y, int n, DWORD col, float width = 1.0f);
	void PlotMarker (double x, double y, DWORD col, int shape = MARK_CIRCLE, const char *label = 0);
	/// A filled rectangle in data coordinates (bars)
	void PlotRect (double x0, double y0, double x1, double y1, DWORD col);
	/// Grid of values z[j*nx+i] at x = x0 + i*(x1-x0)/(nx-1), y = y0 + j*(y1-y0)/(ny-1),
	/// coloured from zlo to zhi (values above zhi, or not finite: not drawn)
	void PlotHeatmap (const float *z, int nx, int ny, double x0, double x1, double y0, double y1, float zlo, float zhi, int cmap = CMAP_COST);
	/// Contour lines of the same grid at the given levels
	void PlotContours (const float *z, int nx, int ny, double x0, double x1, double y0, double y1, const float *levels, int nlev, DWORD col, float width = 1.0f);
	/// The plot was clicked (not dragged): the point in data coordinates
	bool PlotClicked (double *x, double *y);
	/// The pointer is over the plot: its position in data coordinates
	bool PlotHover (double *x, double *y);
	/// Data to display coordinates (current plot)
	void PlotToScreen (double x, double y, float *sx, float *sy) const;
	/// The current view of the plot [data units]
	void PlotView (double *x0, double *x1, double *y0, double *y1) const;
	void EndPlot ();
	/// Return a plot to its PlotSpec view
	void ResetPlot (const char *id);
	//@}

	/// \name Tests
	/// Widgets of the last frame by their ids (recorded while enabled), so
	/// that scripted input tests can aim at them.
	//@{
	void RecordIds (bool on);
	int WidgetCount () const;
	const char *WidgetId (int i) const;
	Rect WidgetRect (int i) const;
	//@}

	/// Colour of a value t (0..1) in a colour map
	static DWORD MapColour (float t, int cmap = CMAP_COST);
	/// Format a value like an axis label (AxisFormat)
	static void FormatValue (char *buf, int len, double v, int fmt, double span);

private:
	struct Impl;
	Impl *d;
};

} // namespace mfdui

#endif // !__MFDUI_H
