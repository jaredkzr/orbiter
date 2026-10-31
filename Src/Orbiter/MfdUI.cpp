// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Widget toolkit for interactive MFD displays (see MFDUI.h)
//
// Immediate mode with displays that are drawn only now and then: the
// widgets drawn in a frame are recorded with their rectangles, pointer
// events between frames are resolved against them (hover, press, drag,
// wheel), and the widget functions of the next frame pick up the results.
// =======================================================================

#define OAPI_IMPLEMENTATION
#include "MFDUI.h"
#include "Orbiter.h"
#include "Astro.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern Orbiter *g_pOrbiter;

using oapi::FVECTOR2;

namespace mfdui {

namespace {

	enum Kind { K_BUTTON, K_DRAG, K_LIST, K_PLOT, K_BLOCK };

	uint32_t Hash (const char *s, uint32_t h = 2166136261u)
	{
		for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
		return h ? h : 1;
	}

	uint32_t Hash2 (uint32_t a, uint32_t b)
	{
		uint32_t h = a;
		for (int i = 0; i < 4; i++, b >>= 8) h = (h ^ (b & 0xFF)) * 16777619u;
		h ^= h >> 15, h *= 0x2c1b3c6dU, h ^= h >> 12;
		return h ? h : 1;
	}

	// colour with opacity: alpha byte 0 means opaque
	DWORD WithAlpha (DWORD col, float a)
	{
		return (col & 0xFFFFFF) | ((DWORD)std::clamp ((int)(a * 255.0f + 0.5f), 1, 255) << 24);
	}

	DWORD Scale (DWORD col, float f) // darker/brighter, keeps the alpha
	{
		auto ch = [&](int s) { return (DWORD)std::clamp ((int)(((col >> s) & 0xFF) * f + 0.5f), 0, 255) << s; };
		return ch (0) | ch (8) | ch (16) | (col & 0xFF000000);
	}

	double NiceStep (double span, int nmax)
	{
		if (!(span > 0.0) || nmax < 1) return 1.0;
		double raw = span / nmax;
		double p = std::pow (10.0, std::floor (std::log10 (raw)));
		for (double m : { 1.0, 2.0, 2.5, 5.0, 10.0 })
			if (m * p >= raw) return m * p;
		return 10.0 * p;
	}

	const char *MONTH[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

	double MonthStart (int year, int mon) // MJD of the 1st of a month (mon 1..12, may overflow)
	{
		while (mon > 12) mon -= 12, year++;
		while (mon < 1) mon += 12, year--;
		struct tm t = {};
		t.tm_year = year - 1900, t.tm_mon = mon, t.tm_mday = 1;
		return date2mjd (&t);
	}

} // namespace

struct Context::Impl {
	oapi::Sketchpad *skp = nullptr;
	oapi::GraphicsClient *gc = nullptr;
	float w = 0.0f, h = 0.0f;
	bool v4 = false;
	bool drawing = false;

	// fonts, created for the display height
	oapi::Font *font[NFONTS] = {};
	float fontPx[NFONTS] = {};
	float cellH[NFONTS] = {};
	float fontForH = 0.0f;

	DWORD col[NROLES];

	struct Item { uint32_t id; Rect r; int kind; };
	std::vector<Item> items, prev;   // this frame's, last frame's widgets
	bool recordIds = false;
	std::vector<std::pair<std::string, Rect>> names, prevNames; // (tests)

	// pointer
	float px = -1.0f, py = -1.0f;    // position
	bool inside = false;
	uint32_t hover = 0, active = 0;  // widget under the pointer, pressed widget
	float pressX = 0.0f, pressY = 0.0f;
	float lastX = 0.0f, lastY = 0.0f;  // position of the last event
	bool dragging = false;           // moved far enough from the press position
	float dragDX = 0.0f, dragDY = 0.0f; // drag not yet taken by the widget
	uint32_t clicked = 0;            // released over the pressed widget
	float clickX = 0.0f, clickY = 0.0f;
	float lastClickX = 0.0f, lastClickY = 0.0f; // of the last click taken by an Area
	uint32_t wheelId = 0;
	float wheel = 0.0f;
	bool dirty = false;
	uint32_t last = 0;               // last widget drawn

	// widget state that persists between frames
	struct State {
		double vx0 = 0, vx1 = 1, vy0 = 0, vy1 = 1; // plot view
		double hx0 = 0, hx1 = 0, hy0 = 0, hy1 = 0; // plot home view
		bool init = false;
		float scroll = 0.0f;                         // list scroll [pixels]
		float dragRem = 0.0f;                        // number: drag not yet a whole step
	};
	std::unordered_map<uint32_t, State> state;

	// popup: number keypad or choice list
	enum { POP_NONE, POP_KEYPAD, POP_CHOICE } popup = POP_NONE;
	uint32_t popOwner = 0;
	Rect popAnchor;
	std::string editBuf, editUnit, editTitle;
	std::vector<std::string> choiceItems;
	int choiceSel = -1;
	// results of popups for their owners, taken in the next frame
	uint32_t resultOwner = 0;
	double resultValue = 0.0;
	int resultIndex = -1;

	std::string tooltip;

	// current plot
	struct Plot {
		uint32_t id = 0;
		Rect area;                       // the data area
		double x0, x1, y0, y1;           // view
		int xfmt, yfmt;
		bool open = false;
	} plot;

	Impl ()
	{
		col[TEXT]   = 0x00FF00;
		col[DIM]    = 0x008000;
		col[ACCENT] = 0x00FFFF;
		col[ALERT]  = 0x3030FF;
		col[INFO]   = 0xFF8040;
		col[BRIGHT] = 0xFFFFFF;
		col[FRAME]  = 0x00A000;
		col[FILL]   = 0x002000;
		col[ACTIVE] = 0x005800;
		col[BACK]   = 0x000000;
	}

	~Impl () { ReleaseFonts (); }

	void ReleaseFonts ()
	{
		for (int i = 0; i < NFONTS; i++)
			if (font[i]) { if (gc) gc->clbkReleaseFont (font[i]); font[i] = nullptr; }
	}

	void MakeFonts ()
	{
		if (fontForH == h && font[0]) return;
		ReleaseFonts ();
		fontForH = h;
		float base = std::max (9.0f, h * 9.0f / 200.0f); // as the standard MFD font
		fontPx[FONT_SMALL] = std::max (8.0f, base * 0.8f);
		fontPx[FONT_NORMAL] = base;
		fontPx[FONT_LARGE] = base * 1.4f;
		fontPx[FONT_MONO] = base;
		if (!gc) return;
		font[FONT_SMALL] = gc->clbkCreateFont (-(int)std::lround (fontPx[FONT_SMALL]), true, "Sans");
		font[FONT_NORMAL] = gc->clbkCreateFont (-(int)std::lround (fontPx[FONT_NORMAL]), true, "Sans");
		font[FONT_LARGE] = gc->clbkCreateFont (-(int)std::lround (fontPx[FONT_LARGE]), true, "Sans", FONT_BOLD);
		font[FONT_MONO] = gc->clbkCreateFont (-(int)std::lround (fontPx[FONT_MONO]), false, "Fixed");
		for (int i = 0; i < NFONTS; i++) {
			cellH[i] = fontPx[i] * 1.2f;
			if (skp && font[i]) {
				skp->SetFont (font[i]);
				DWORD cs = skp->GetCharSize ();
				if (cs & 0xFFFF) cellH[i] = (float)(cs & 0xFFFF);
			}
		}
	}

	State &St (uint32_t id) { return state[id]; }

	void Add (uint32_t id, const Rect &r, int kind, const char *name = nullptr)
	{
		items.push_back ({ id, r, kind });
		last = id;
		if (recordIds && name) names.emplace_back (name, r);
	}

	// widget under a point, from the last frame (topmost first)
	uint32_t HitTest (float x, float y) const
	{
		for (auto it = prev.rbegin(); it != prev.rend(); ++it)
			if (it->r.Contains (x, y)) return it->id;
		return 0;
	}

	const Item *Find (uint32_t id) const
	{
		for (const Item &i : prev) if (i.id == id) return &i;
		return nullptr;
	}

	bool TakeClick (uint32_t id, float *x = nullptr, float *y = nullptr)
	{
		if (clicked != id || !id) return false;
		clicked = 0;
		if (x) *x = clickX;
		if (y) *y = clickY;
		return true;
	}

	float TakeWheel (uint32_t id)
	{
		if (wheelId != id || !id) return 0.0f;
		float w = wheel;
		wheel = 0.0f, wheelId = 0;
		return w;
	}

	bool TakeDrag (uint32_t id, float &dx, float &dy)
	{
		if (active != id || !dragging) return false;
		dx = dragDX, dy = dragDY;
		dragDX = dragDY = 0.0f;
		return true;
	}

	// --- drawing primitives

	void Pen (DWORD c, float width) { skp->QuickPen (c, width); }

	void PolyLine (const FVECTOR2 *p, int n, DWORD c, float width, bool closed)
	{
		if (n < 2) return;
		Pen (c, width);
		if (v4) {
			skp->PolylineF (p, n, closed);
		} else {
			std::vector<oapi::IVECTOR2> ip (n + (closed ? 1 : 0));
			for (int i = 0; i < n; i++) ip[i] = { (long)std::floor (p[i].x), (long)std::floor (p[i].y) };
			if (closed) ip[n] = ip[0];
			skp->Polyline (ip.data(), (int)ip.size());
		}
	}

	void FillPoly (const FVECTOR2 *p, int n, DWORD c)
	{
		if (n < 3) return;
		skp->SetPen (nullptr);
		skp->QuickBrush (c);
		if (v4) {
			skp->PolygonF (p, n);
		} else {
			std::vector<oapi::IVECTOR2> ip (n);
			for (int i = 0; i < n; i++) ip[i] = { (long)std::lround (p[i].x), (long)std::lround (p[i].y) };
			skp->Polygon (ip.data(), n);
		}
		skp->SetBrush (nullptr);
	}

	void Fill (const Rect &r, DWORD c)
	{
		if (r.W() <= 0.0f || r.H() <= 0.0f) return;
		if (v4) {
			FVECTOR2 p[4] = { { r.x0, r.y0 }, { r.x1, r.y0 }, { r.x1, r.y1 }, { r.x0, r.y1 } };
			FillPoly (p, 4, c);
		} else {
			skp->SetPen (nullptr);
			skp->QuickBrush (c);
			skp->Rectangle ((int)std::lround (r.x0), (int)std::lround (r.y0), (int)std::lround (r.x1), (int)std::lround (r.y1));
			skp->SetBrush (nullptr);
		}
	}

	void Frame (const Rect &r, DWORD c, float width)
	{
		float hw = 0.5f * width;
		FVECTOR2 p[4] = { { r.x0 + hw, r.y0 + hw }, { r.x1 - hw, r.y0 + hw }, { r.x1 - hw, r.y1 - hw }, { r.x0 + hw, r.y1 - hw } };
		PolyLine (p, 4, c, width, true);
	}

	float TextW (const char *s, int f)
	{
		if (!skp || !s || !*s) return 0.0f;
		if (font[f]) skp->SetFont (font[f]);
		return (float)skp->GetTextWidth (s, (int)strlen (s));
	}

	float Txt (float x, float y, const char *s, DWORD c, int align, int f)
	{
		if (!skp || !s || !*s) return 0.0f;
		f = std::clamp (f, 0, NFONTS - 1);
		if (font[f]) skp->SetFont (font[f]);
		float tw = (float)skp->GetTextWidth (s, (int)strlen (s));
		if (align & ALIGN_CENTRE) x -= 0.5f * tw;
		else if (align & ALIGN_RIGHT) x -= tw;
		if (align & ALIGN_MIDDLE) y -= 0.5f * cellH[f];
		else if (align & ALIGN_BOTTOM) y -= cellH[f];
		skp->SetTextAlign (oapi::Sketchpad::LEFT, oapi::Sketchpad::TOP);
		skp->SetTextColor (c);
		skp->SetBackgroundMode (oapi::Sketchpad::BK_TRANSPARENT);
		skp->Text ((int)std::lround (x), (int)std::lround (y), s, (int)strlen (s));
		return tw;
	}

	// text cut to a width (with an ellipsis)
	std::string Fit (const char *s, float wmax, int f)
	{
		std::string t = s ? s : "";
		if (TextW (t.c_str(), f) <= wmax) return t;
		while (!t.empty() && TextW ((t + "..").c_str(), f) > wmax) t.pop_back ();
		return t + "..";
	}

	void ClipTo (const Rect *r)
	{
		if (!skp) return;
		if (r) {
			RECT rc = { (LONG)std::floor (r->x0), (LONG)std::floor (r->y0), (LONG)std::ceil (r->x1), (LONG)std::ceil (r->y1) };
			skp->ClipRect (&rc);
		} else {
			skp->ClipRect (nullptr);
		}
	}

	float Unit () const { return cellH[FONT_NORMAL] * 1.15f; }

	// --- popups (drawn in End, on top of everything)
	void DrawPopups ();
	void DrawKeypad ();
	void DrawChoice ();
	void KeypadInput (const char *k);
	void CloseEdit (bool accept);
};

// =======================================================================

Context::Context (): d(new Impl) {}
Context::~Context () { delete d; }

void Context::Begin (oapi::Sketchpad *skp, float w, float h)
{
	d->skp = skp;
	d->gc = g_pOrbiter ? g_pOrbiter->GetGraphicsClient () : nullptr;
	d->w = w, d->h = h;
	d->v4 = skp && skp->GetVersion () >= 4;
	d->drawing = true;
	d->items.clear ();
	d->names.clear ();
	d->tooltip.clear ();
	d->last = 0;
	d->dirty = false;
	d->MakeFonts ();
	if (skp) skp->SetBackgroundMode (oapi::Sketchpad::BK_TRANSPARENT);
}

void Context::End ()
{
	if (d->plot.open) EndPlot ();
	if (d->skp) {
		d->ClipTo (nullptr);
		d->DrawPopups ();
		if (!d->tooltip.empty() && d->inside) {
			float u = d->Unit ();
			float tw = d->TextW (d->tooltip.c_str(), FONT_SMALL) + u * 0.6f;
			float th = d->cellH[FONT_SMALL] + u * 0.3f;
			float x = std::clamp (d->px + u * 0.6f, 0.0f, std::max (0.0f, d->w - tw));
			float y = d->py + u * 0.9f;
			if (y + th > d->h) y = d->py - th - u * 0.3f;
			Rect r (x, y, x + tw, y + th);
			d->Fill (r, 0x101010);
			d->Frame (r, d->col[DIM], 1.0f);
			d->Txt (r.x0 + u * 0.3f, r.CY(), d->tooltip.c_str(), d->col[BRIGHT], ALIGN_MIDDLE, FONT_SMALL);
		}
		d->skp->SetPen (nullptr);
		d->skp->SetBrush (nullptr);
	}
	d->prev.swap (d->items);
	d->prevNames.swap (d->names);
	// a click on a widget that is no longer drawn is dropped
	if (d->clicked && !d->Find (d->clicked)) d->clicked = 0;
	if (d->resultOwner && !d->Find (d->resultOwner) && d->popup == Impl::POP_NONE) d->resultOwner = 0;
	d->drawing = false;
	d->skp = nullptr;
}

oapi::Sketchpad *Context::Skp () const { return d->skp; }
Rect Context::Area () const { return Rect (0.0f, 0.0f, d->w, d->h); }
float Context::Unit () const { return d->Unit (); }
DWORD Context::Colour (int role) const { return (role >= 0 && role < NROLES) ? d->col[role] : 0; }
void Context::SetColour (int role, DWORD col) { if (role >= 0 && role < NROLES) d->col[role] = col; }

// =======================================================================
// Input

bool Context::Pointer (const MFDPOINTER &p)
{
	Impl &s = *d;
	s.px = p.x, s.py = p.y;
	uint32_t prevHover = s.hover;
	switch (p.event) {
	case MFD_POINTER_MOVE:
		s.inside = true;
		s.hover = s.HitTest (p.x, p.y);
		if (s.active) {
			float dx = p.x - s.pressX, dy = p.y - s.pressY;
			if (!s.dragging && dx*dx + dy*dy > 16.0f) {
				s.dragging = true;
				s.dragDX += dx, s.dragDY += dy; // the movement so far
			} else if (s.dragging) {
				s.dragDX += p.x - s.lastX, s.dragDY += p.y - s.lastY;
			}
			s.dirty = true;
		}
		s.lastX = p.x, s.lastY = p.y;
		break;
	case MFD_POINTER_DOWN:
		s.inside = true;
		s.hover = s.HitTest (p.x, p.y);
		if (p.button != 0) return s.hover != 0;
		s.active = s.hover;
		s.pressX = s.lastX = p.x, s.pressY = s.lastY = p.y;
		s.dragging = false;
		s.dragDX = s.dragDY = 0.0f;
		s.dirty = true;
		if (s.active) {
			const Impl::Item *it = s.Find (s.active);
			if (it && it->kind == K_BLOCK) { // outside an open popup: close it
				s.CloseEdit (false);
				s.active = 0;
				return true;
			}
		}
		return s.active != 0;
	case MFD_POINTER_UP: {
		if (p.button != 0) return s.hover != 0;
		bool used = s.active != 0;
		if (s.active && !s.dragging && s.HitTest (p.x, p.y) == s.active) {
			s.clicked = s.active;
			s.clickX = p.x, s.clickY = p.y;
		}
		s.active = 0;
		s.dragging = false;
		s.dirty = true;
		return used;
	}
	case MFD_POINTER_WHEEL:
		s.inside = true;
		s.hover = s.HitTest (p.x, p.y);
		if (!s.hover) return false;
		{
			const Impl::Item *it = s.Find (s.hover);
			if (!it || it->kind == K_BUTTON || it->kind == K_BLOCK) return it && it->kind == K_BLOCK;
		}
		if (s.wheelId != s.hover) s.wheel = 0.0f;
		s.wheelId = s.hover;
		s.wheel += p.wheel;
		s.dirty = true;
		return true;
	case MFD_POINTER_LEAVE:
		s.inside = false;
		s.hover = 0;
		break;
	}
	if (s.hover != prevHover) s.dirty = true;
	return s.active != 0 || s.hover != 0;
}

bool Context::Key (DWORD key)
{
	Impl &s = *d;
	if (s.popup == Impl::POP_KEYPAD) {
		const char *k = nullptr;
		static const char *digit[10] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
		switch (key) {
		case OAPI_KEY_0: case OAPI_KEY_NUMPAD0: k = digit[0]; break;
		case OAPI_KEY_1: case OAPI_KEY_NUMPAD1: k = digit[1]; break;
		case OAPI_KEY_2: case OAPI_KEY_NUMPAD2: k = digit[2]; break;
		case OAPI_KEY_3: case OAPI_KEY_NUMPAD3: k = digit[3]; break;
		case OAPI_KEY_4: case OAPI_KEY_NUMPAD4: k = digit[4]; break;
		case OAPI_KEY_5: case OAPI_KEY_NUMPAD5: k = digit[5]; break;
		case OAPI_KEY_6: case OAPI_KEY_NUMPAD6: k = digit[6]; break;
		case OAPI_KEY_7: case OAPI_KEY_NUMPAD7: k = digit[7]; break;
		case OAPI_KEY_8: case OAPI_KEY_NUMPAD8: k = digit[8]; break;
		case OAPI_KEY_9: case OAPI_KEY_NUMPAD9: k = digit[9]; break;
		case OAPI_KEY_PERIOD: case OAPI_KEY_DECIMAL: case OAPI_KEY_COMMA: k = "."; break;
		case OAPI_KEY_MINUS: case OAPI_KEY_SUBTRACT: k = "-"; break;
		case OAPI_KEY_E: k = "E"; break;
		case OAPI_KEY_BACK: k = "<"; break;
		case OAPI_KEY_RETURN: case OAPI_KEY_NUMPADENTER: k = "OK"; break;
		case OAPI_KEY_ESCAPE: k = "ESC"; break;
		default: return true; // other keys are swallowed while editing
		}
		s.KeypadInput (k);
		s.dirty = true;
		return true;
	}
	if (s.popup == Impl::POP_CHOICE) {
		int n = (int)s.choiceItems.size();
		switch (key) {
		case OAPI_KEY_UP: s.choiceSel = std::max (0, s.choiceSel - 1); break;
		case OAPI_KEY_DOWN: s.choiceSel = std::min (n - 1, s.choiceSel + 1); break;
		case OAPI_KEY_RETURN: case OAPI_KEY_NUMPADENTER:
			s.resultOwner = s.popOwner, s.resultIndex = s.choiceSel;
			s.popup = Impl::POP_NONE;
			break;
		case OAPI_KEY_ESCAPE: s.popup = Impl::POP_NONE; break;
		default: return true;
		}
		s.dirty = true;
		return true;
	}
	return false;
}

bool Context::Editing () const { return d->popup != Impl::POP_NONE; }
bool Context::Dirty () const { return d->dirty || d->clicked || d->wheelId || d->resultOwner; }
bool Context::Dragging () const { return d->active && d->dragging; }

// =======================================================================
// Drawing

void Context::Line (float x0, float y0, float x1, float y1, DWORD col, float width)
{
	FVECTOR2 p[2] = { { x0, y0 }, { x1, y1 } };
	d->PolyLine (p, 2, col, width, false);
}

void Context::Polyline (const FVECTOR2 *pt, int n, DWORD col, float width, bool closed)
{
	d->PolyLine (pt, n, col, width, closed);
}

void Context::FillPolygon (const FVECTOR2 *pt, int n, DWORD col) { d->FillPoly (pt, n, col); }
void Context::FillRect (const Rect &r, DWORD col) { d->Fill (r, col); }
void Context::FrameRect (const Rect &r, DWORD col, float width) { d->Frame (r, col, width); }

void Context::Circle (float cx, float cy, float rad, DWORD col, float width, bool fill)
{
	int n = std::clamp ((int)(rad * 1.5f), 12, 96);
	std::vector<FVECTOR2> p (n);
	for (int i = 0; i < n; i++) {
		float a = 6.2831853f * i / n;
		p[i] = { cx + rad * std::cos (a), cy + rad * std::sin (a) };
	}
	if (fill) d->FillPoly (p.data(), n, col);
	else d->PolyLine (p.data(), n, col, width, true);
}

float Context::Text (float x, float y, const char *str, DWORD col, int align, int font)
{
	return d->Txt (x, y, str, col, align, font);
}

void Context::TextIn (const Rect &r, const char *str, DWORD col, int align, int font)
{
	float pad = d->Unit () * 0.15f;
	if (font == FONT_NORMAL && d->TextW (str, font) > r.W() - 2.0f * pad) font = FONT_SMALL; // (smaller before cut)
	std::string t = d->Fit (str, r.W() - 2.0f * pad, font);
	float x = (align & ALIGN_CENTRE) ? r.CX() : (align & ALIGN_RIGHT) ? r.x1 - pad : r.x0 + pad;
	float y = (align & ALIGN_MIDDLE) ? r.CY() : (align & ALIGN_BOTTOM) ? r.y1 : r.y0;
	d->Txt (x, y, t.c_str(), col, align, font);
}

float Context::TextWidth (const char *str, int font) { return d->TextW (str, font); }
float Context::TextHeight (int font) const { return d->cellH[std::clamp (font, 0, NFONTS - 1)]; }

void Context::Image (const DWORD *pix, int w, int h, const Rect &r, bool smooth)
{
	if (!d->skp || !pix || w < 1 || h < 1) return;
	if (d->v4) {
		d->skp->Image (pix, w, h, r.x0, r.y0, r.x1, r.y1, smooth);
		return;
	}
	// without image support: a rectangle per pixel
	float cw = r.W() / w, ch = r.H() / h;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			DWORD c = pix[j*w + i];
			if (!(c >> 24)) continue;
			d->Fill (Rect (r.x0 + i*cw, r.y0 + j*ch, r.x0 + (i+1)*cw, r.y0 + (j+1)*ch), c | 0xFF000000);
		}
}

void Context::Clip (const Rect *r) { d->ClipTo (r); }

// =======================================================================
// Widgets

bool Context::Button (const char *id, const Rect &r, const char *label, bool selected, bool enabled)
{
	Impl &s = *d;
	uint32_t h = Hash (id);
	if (enabled) s.Add (h, r, K_BUTTON, id);
	else s.last = h;
	bool hov = enabled && s.hover == h;
	bool down = hov && s.active == h;
	DWORD bg = down ? s.col[ACTIVE] : selected ? Scale (s.col[ACTIVE], 0.8f) : hov ? Scale (s.col[FILL], 1.8f) : s.col[FILL];
	s.Fill (r, bg);
	s.Frame (r, selected ? s.col[ACCENT] : hov ? s.col[TEXT] : enabled ? s.col[FRAME] : Scale (s.col[FRAME], 0.5f), 1.0f);
	TextIn (r, label, !enabled ? s.col[DIM] : selected ? s.col[ACCENT] : s.col[TEXT], ALIGN_CENTRE|ALIGN_MIDDLE);
	return enabled && s.TakeClick (h);
}

bool Context::Toggle (const char *id, const Rect &r, const char *label, bool *v)
{
	Impl &s = *d;
	uint32_t h = Hash (id);
	s.Add (h, r, K_BUTTON, id);
	bool changed = false;
	if (s.TakeClick (h)) *v = !*v, changed = true;
	bool hov = s.hover == h;
	float b = std::min (r.H() * 0.62f, s.Unit () * 0.8f);
	Rect box (r.x0 + b * 0.25f, r.CY() - b * 0.5f, r.x0 + b * 1.25f, r.CY() + b * 0.5f);
	s.Fill (box, hov ? Scale (s.col[FILL], 1.8f) : s.col[FILL]);
	s.Frame (box, hov ? s.col[TEXT] : s.col[FRAME], 1.0f);
	if (*v) s.Fill (box.Inset (b * 0.22f), s.col[ACCENT]);
	TextIn (r.CutLeft (b * 1.4f), label, *v ? s.col[TEXT] : s.col[DIM], ALIGN_LEFT|ALIGN_MIDDLE);
	return changed;
}

bool Context::Tabs (const char *id, const Rect &r, const char *const *labels, int n, int *sel)
{
	Impl &s = *d;
	if (n < 1) return false;
	uint32_t h0 = Hash (id);
	bool changed = false;
	float gap = std::max (1.0f, s.Unit () * 0.12f);
	// widths by the labels (normal font, or the small one if they don't fit)
	float pad = s.Unit () * 0.35f, avail = r.W() - gap * (n - 1);
	int f = FONT_NORMAL;
	std::vector<float> tw (n);
	float sum = 0.0f;
	for (int k = 0; k < 2; k++, f = FONT_SMALL) {
		sum = 0.0f;
		for (int i = 0; i < n; i++) sum += (tw[i] = s.TextW (labels[i], f) + pad);
		if (sum <= avail) break;
	}
	float x = r.x0;
	for (int i = 0; i < n; i++) {
		float w = tw[i] * avail / sum;
		Rect c (x, r.y0, x + w, r.y1);
		x += w + gap;
		uint32_t h = Hash2 (h0, (uint32_t)i + 1);
		std::string nm = std::string (id) + "." + std::to_string (i);
		s.Add (h, c, K_BUTTON, nm.c_str());
		if (s.TakeClick (h) && *sel != i) *sel = i, changed = true;
		bool on = (*sel == i), hov = s.hover == h;
		s.Fill (c, on ? s.col[ACTIVE] : hov ? Scale (s.col[FILL], 1.8f) : s.col[FILL]);
		if (on) s.Fill (Rect (c.x0, c.y1 - std::max (2.0f, gap * 1.5f), c.x1, c.y1), s.col[ACCENT]);
		else s.Frame (c, hov ? s.col[TEXT] : Scale (s.col[FRAME], 0.7f), 1.0f);
		std::string t = s.Fit (labels[i], c.W() - 2.0f, f);
		s.Txt (c.CX(), c.CY(), t.c_str(), on ? s.col[ACCENT] : s.col[TEXT], ALIGN_CENTRE|ALIGN_MIDDLE, f);
	}
	s.last = h0;
	return changed;
}

bool Context::Choice (const char *id, const Rect &r, const char *const *items, int n, int *sel)
{
	Impl &s = *d;
	uint32_t h = Hash (id);
	s.Add (h, r, K_BUTTON, id);
	bool changed = false;
	if (s.resultOwner == h && s.popup == Impl::POP_NONE) {
		if (s.resultIndex >= 0 && s.resultIndex < n && s.resultIndex != *sel) *sel = s.resultIndex, changed = true;
		s.resultOwner = 0;
	}
	if (s.TakeClick (h)) {
		s.popup = Impl::POP_CHOICE;
		s.popOwner = h;
		s.popAnchor = r;
		s.choiceSel = *sel;
		s.dirty = true;
	}
	if (s.popup == Impl::POP_CHOICE && s.popOwner == h) {
		s.choiceItems.assign (items, items + n);
		s.popAnchor = r;
	}
	bool hov = s.hover == h;
	s.Fill (r, hov ? Scale (s.col[FILL], 1.8f) : s.col[FILL]);
	s.Frame (r, hov ? s.col[TEXT] : s.col[FRAME], 1.0f);
	float u = s.Unit ();
	float aw = u * 0.7f;
	TextIn (r.CutRight (aw), (*sel >= 0 && *sel < n) ? items[*sel] : "-", s.col[ACCENT], ALIGN_LEFT|ALIGN_MIDDLE);
	float ax = r.x1 - aw * 0.6f, ay = r.CY();
	FVECTOR2 tri[3] = { { ax - aw * 0.25f, ay - aw * 0.12f }, { ax + aw * 0.25f, ay - aw * 0.12f }, { ax, ay + aw * 0.2f } };
	s.FillPoly (tri, 3, s.col[TEXT]);
	s.last = h;
	return changed;
}

bool Context::Number (const char *id, const Rect &r, double *v, const NumberSpec &spec)
{
	Impl &s = *d;
	uint32_t h = Hash (id);
	s.Add (h, r, K_DRAG, id);
	Impl::State &st = s.St (h);
	double old = *v;
	auto Clamp = [&](double x) { return (spec.hi > spec.lo) ? std::clamp (x, spec.lo, spec.hi) : x; };
	auto StepBy = [&](double x, double steps) {
		return spec.log ? x * std::pow (1.0 + spec.step, steps) : x + spec.step * steps;
	};

	if (s.resultOwner == h && s.popup == Impl::POP_NONE) {
		*v = Clamp (s.resultValue / (spec.scale ? spec.scale : 1.0));
		s.resultOwner = 0;
	}
	float wh = s.TakeWheel (h);
	if (wh != 0.0f) *v = Clamp (StepBy (*v, wh));
	float dx, dy;
	if (s.TakeDrag (h, dx, dy)) {
		float px = s.Unit () * 0.5f; // pixels per step
		st.dragRem += dx / px;
		double steps = std::trunc (st.dragRem);
		st.dragRem -= (float)steps;
		if (steps != 0.0) *v = Clamp (StepBy (*v, steps));
	}
	char buf[64];
	snprintf (buf, sizeof (buf), spec.fmt ? spec.fmt : "%g", *v * (spec.scale ? spec.scale : 1.0));
	if (s.TakeClick (h)) {
		s.popup = Impl::POP_KEYPAD;
		s.popOwner = h;
		s.popAnchor = r;
		s.editBuf = buf;
		s.editUnit = spec.unit ? spec.unit : "";
		s.editTitle = id;
		size_t hh = s.editTitle.find ("##");
		if (hh != std::string::npos) s.editTitle.resize (hh);
		s.dirty = true;
	}

	bool hov = s.hover == h, act = s.active == h;
	bool editing = s.popup == Impl::POP_KEYPAD && s.popOwner == h;
	s.Fill (r, act || editing ? s.col[ACTIVE] : hov ? Scale (s.col[FILL], 1.8f) : s.col[FILL]);
	s.Frame (r, editing ? s.col[ACCENT] : hov ? s.col[TEXT] : s.col[FRAME], 1.0f);
	float u = s.Unit ();
	float uw = spec.unit ? s.TextW (spec.unit, FONT_SMALL) + u * 0.35f : 0.0f;
	if (spec.unit) s.Txt (r.x1 - u * 0.2f, r.CY(), spec.unit, s.col[DIM], ALIGN_RIGHT|ALIGN_MIDDLE, FONT_SMALL);
	// the value in the normal font, or the small one if it doesn't fit
	float avail = r.W() - uw - u * 0.5f;
	int vf = (s.TextW (buf, FONT_NORMAL) <= avail) ? FONT_NORMAL : FONT_SMALL;
	TextIn (r.CutRight (uw), buf, s.col[ACCENT], ALIGN_RIGHT|ALIGN_MIDDLE, vf);
	if (hov && !editing) {
		// step arrows hint
		float ax = r.x0 + u * 0.35f, ay = r.CY(), a = u * 0.18f;
		FVECTOR2 l[3] = { { ax + a, ay - a }, { ax, ay }, { ax + a, ay + a } };
		s.PolyLine (l, 3, s.col[DIM], 1.0f, false);
	}
	s.last = h;
	return *v != old;
}

bool Context::List (const char *id, const Rect &r, const char *const *items, int n, int *sel)
{
	Impl &s = *d;
	uint32_t h = Hash (id);
	s.Add (h, r, K_LIST, id);
	Impl::State &st = s.St (h);
	float rowH = s.Unit () * 1.1f;
	float total = rowH * n;
	float maxScroll = std::max (0.0f, total - r.H());
	bool changed = false;

	float wh = s.TakeWheel (h);
	if (wh != 0.0f) st.scroll -= wh * rowH * 2.0f;
	float dx, dy;
	if (s.TakeDrag (h, dx, dy)) st.scroll -= dy;
	float cx, cy;
	if (s.TakeClick (h, &cx, &cy)) {
		int i = (int)std::floor ((cy - r.y0 + st.scroll) / rowH);
		if (i >= 0 && i < n && i != *sel) *sel = i, changed = true;
	}
	// keep the selection in view when it changed from outside
	st.scroll = std::clamp (st.scroll, 0.0f, maxScroll);

	s.Fill (r, s.col[BACK]);
	s.Frame (r, s.col[FRAME], 1.0f);
	s.ClipTo (&r);
	int i0 = std::max (0, (int)std::floor (st.scroll / rowH));
	for (int i = i0; i < n; i++) {
		float y0 = r.y0 + i * rowH - st.scroll;
		if (y0 > r.y1) break;
		Rect row (r.x0 + 1.0f, y0, r.x1 - 1.0f, y0 + rowH);
		bool hovRow = s.hover == h && s.inside && row.Contains (s.px, s.py);
		if (i == *sel) s.Fill (row, s.col[ACTIVE]);
		else if (hovRow) s.Fill (row, Scale (s.col[FILL], 1.6f));
		TextIn (row, items[i], i == *sel ? s.col[ACCENT] : s.col[TEXT], ALIGN_LEFT|ALIGN_MIDDLE);
	}
	s.ClipTo (nullptr);
	if (maxScroll > 0.0f) { // scroll bar
		float bh = r.H() * r.H() / total;
		float by = r.y0 + (r.H() - bh) * st.scroll / maxScroll;
		s.Fill (Rect (r.x1 - 3.0f, by, r.x1 - 1.0f, by + bh), s.col[DIM]);
	}
	s.last = h;
	return changed;
}

int Context::Area (const char *id, const Rect &r, float *dx, float *dy, float *wheel)
{
	Impl &s = *d;
	uint32_t h = Hash (id);
	s.Add (h, r, K_DRAG, id);
	int f = 0;
	if (s.hover == h) f |= AREA_HOVER;
	if (s.active == h) f |= AREA_PRESSED;
	float cx, cy;
	if (s.TakeClick (h, &cx, &cy)) f |= AREA_CLICKED, s.lastClickX = cx, s.lastClickY = cy;
	float ddx = 0.0f, ddy = 0.0f;
	if (s.TakeDrag (h, ddx, ddy)) f |= AREA_DRAG;
	if (dx) *dx = ddx;
	if (dy) *dy = ddy;
	float w = s.TakeWheel (h);
	if (w != 0.0f) f |= AREA_WHEEL;
	if (wheel) *wheel = w;
	return f;
}

bool Context::PointerPos (float *x, float *y) const
{
	if (!d->inside) return false;
	*x = d->px, *y = d->py;
	return true;
}

void Context::ClickPos (float *x, float *y) const
{
	*x = d->lastClickX, *y = d->lastClickY;
}

void Context::RecordIds (bool on) { d->recordIds = on; }
int Context::WidgetCount () const { return (int)d->prevNames.size(); }
const char *Context::WidgetId (int i) const { return (i >= 0 && i < (int)d->prevNames.size()) ? d->prevNames[i].first.c_str() : ""; }
Rect Context::WidgetRect (int i) const { return (i >= 0 && i < (int)d->prevNames.size()) ? d->prevNames[i].second : Rect (); }

bool Context::Hovered () const { return d->last && d->hover == d->last; }

void Context::Tooltip (const char *text)
{
	if (Hovered () && text && !d->active) d->tooltip = text;
}

void Context::ShowTooltip (const char *text)
{
	if (text) d->tooltip = text;
}

// =======================================================================
// Popups

void Context::Impl::CloseEdit (bool accept)
{
	if (popup == POP_KEYPAD && accept) {
		char *end = nullptr;
		double x = strtod (editBuf.c_str(), &end);
		if (end != editBuf.c_str() && std::isfinite (x)) {
			resultOwner = popOwner;
			resultValue = x;
		}
	}
	popup = POP_NONE;
	dirty = true;
}

void Context::Impl::KeypadInput (const char *k)
{
	if (!strcmp (k, "OK")) { CloseEdit (true); return; }
	if (!strcmp (k, "ESC")) { CloseEdit (false); return; }
	if (!strcmp (k, "<")) { if (!editBuf.empty()) editBuf.pop_back (); return; }
	if (!strcmp (k, "CLR")) { editBuf.clear (); return; }
	if (!strcmp (k, "-")) { // sign of the mantissa or the exponent
		size_t e = editBuf.find_first_of ("eE");
		size_t at = (e == std::string::npos) ? 0 : e + 1;
		if (at < editBuf.size() && editBuf[at] == '-') editBuf.erase (at, 1);
		else editBuf.insert (at, "-");
		return;
	}
	if (!strcmp (k, ".") && editBuf.find ('.') != std::string::npos) return;
	if (!strcmp (k, "E") && editBuf.find_first_of ("eE") != std::string::npos) return;
	if (editBuf.size() < 24) editBuf += k;
}

void Context::Impl::DrawPopups ()
{
	if (popup == POP_KEYPAD) DrawKeypad ();
	else if (popup == POP_CHOICE) DrawChoice ();
}

void Context::Impl::DrawKeypad ()
{
	float u = Unit ();
	float kw = std::min (w * 0.9f, u * 11.0f), kh = std::min (h * 0.9f, u * 9.6f);
	float x0 = std::clamp (popAnchor.CX() - kw * 0.5f, 0.0f, std::max (0.0f, w - kw));
	float y0 = popAnchor.y1 + u * 0.2f;
	if (y0 + kh > h) y0 = popAnchor.y0 - kh - u * 0.2f;
	if (y0 < 0.0f) y0 = std::max (0.0f, (h - kh) * 0.5f);
	Rect pad (x0, y0, x0 + kw, y0 + kh);
	uint32_t blk = Hash2 (popOwner, 0xB10C);
	Add (blk, Rect (0, 0, w, h), K_BLOCK);
	Fill (Rect (0, 0, w, h), WithAlpha (0, 0.45f));
	Fill (pad, 0x0A0A0A);
	Frame (pad, col[ACCENT], 1.0f);

	Rect in = pad.Inset (u * 0.3f);
	Rect title = in.Top (u);
	Txt (title.x0, title.CY(), editTitle.c_str(), col[DIM], ALIGN_LEFT|ALIGN_MIDDLE, FONT_SMALL);
	Rect field = in.CutTop (u * 1.1f).Top (u * 1.3f);
	Fill (field, col[BACK]);
	Frame (field, col[ACCENT], 1.0f);
	std::string shown = editBuf + "_";
	float uw = editUnit.empty() ? 0.0f : TextW (editUnit.c_str(), FONT_SMALL) + u * 0.4f;
	if (!editUnit.empty()) Txt (field.x1 - u * 0.2f, field.CY(), editUnit.c_str(), col[DIM], ALIGN_RIGHT|ALIGN_MIDDLE, FONT_SMALL);
	Txt (field.x1 - u * 0.25f - uw, field.CY(), shown.c_str(), col[BRIGHT], ALIGN_RIGHT|ALIGN_MIDDLE, FONT_MONO);

	static const char *keys[5][4] = {
		{ "7", "8", "9", "<" },
		{ "4", "5", "6", "CLR" },
		{ "1", "2", "3", "E" },
		{ "-", "0", ".", "ESC" },
		{ "", "", "", "OK" } };
	Rect grid = in.CutTop (u * 2.6f);
	float gap = u * 0.15f;
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			const char *k = keys[j][i];
			Rect c = grid.Row (j, 4, gap).Col (i, 4, gap);
			if (j == 3 && i == 3) { // OK and Cancel share the last column
				Rect ok = c, esc = c;
				esc = c.Col (0, 2, gap), ok = c.Col (1, 2, gap);
				for (int m = 0; m < 2; m++) {
					const char *kk = m ? "OK" : "ESC";
					Rect cc = m ? ok : esc;
					uint32_t hk = Hash2 (popOwner, Hash (kk));
					Add (hk, cc, K_BUTTON, m ? "keypad.OK" : "keypad.ESC");
					bool hv = hover == hk;
					Fill (cc, m ? (hv ? 0x006000 : 0x004000) : (hv ? 0x202060 : 0x101040));
					Frame (cc, hv ? col[TEXT] : col[FRAME], 1.0f);
					Txt (cc.CX(), cc.CY(), m ? "OK" : "X", col[BRIGHT], ALIGN_CENTRE|ALIGN_MIDDLE, FONT_SMALL);
					if (TakeClick (hk)) KeypadInput (kk);
				}
				continue;
			}
			uint32_t hk = Hash2 (popOwner, Hash (k));
			std::string kn = std::string ("keypad.") + k;
			Add (hk, c, K_BUTTON, kn.c_str());
			bool hv = hover == hk;
			Fill (c, hv ? Scale (col[FILL], 2.2f) : col[FILL]);
			Frame (c, hv ? col[TEXT] : col[FRAME], 1.0f);
			Txt (c.CX(), c.CY(), k, col[TEXT], ALIGN_CENTRE|ALIGN_MIDDLE, strlen (k) > 1 ? FONT_SMALL : FONT_NORMAL);
			if (TakeClick (hk)) KeypadInput (k);
		}
}

void Context::Impl::DrawChoice ()
{
	float u = Unit ();
	int n = (int)choiceItems.size();
	float rowH = u * 1.1f;
	float lw = popAnchor.W();
	for (const std::string &it : choiceItems) lw = std::max (lw, TextW (it.c_str(), FONT_NORMAL) + u);
	lw = std::min (lw, w);
	float lh = std::min (rowH * n, h * 0.8f);
	float x0 = std::clamp (popAnchor.x0, 0.0f, std::max (0.0f, w - lw));
	float y0 = popAnchor.y1;
	if (y0 + lh > h) y0 = std::max (0.0f, popAnchor.y0 - lh);
	Rect box (x0, y0, x0 + lw, y0 + lh);
	uint32_t blk = Hash2 (popOwner, 0xB10C);
	Add (blk, Rect (0, 0, w, h), K_BLOCK);
	uint32_t hl = Hash2 (popOwner, 0x1157);
	Add (hl, box, K_LIST, "choice");
	State &st = St (hl);
	float maxScroll = std::max (0.0f, rowH * n - lh);
	float wh = TakeWheel (hl);
	if (wh != 0.0f) st.scroll -= wh * rowH * 2.0f;
	float dx, dy;
	if (TakeDrag (hl, dx, dy)) st.scroll -= dy;
	st.scroll = std::clamp (st.scroll, 0.0f, maxScroll);
	float cx, cy;
	if (TakeClick (hl, &cx, &cy)) {
		int i = (int)std::floor ((cy - box.y0 + st.scroll) / rowH);
		if (i >= 0 && i < n) {
			resultOwner = popOwner, resultIndex = i;
			popup = POP_NONE;
			dirty = true;
		}
	}
	Fill (box, 0x080808);
	Frame (box, col[ACCENT], 1.0f);
	ClipTo (&box);
	for (int i = 0; i < n; i++) {
		float ry = box.y0 + i * rowH - st.scroll;
		if (ry > box.y1) break;
		if (ry + rowH < box.y0) continue;
		Rect row (box.x0 + 1.0f, ry, box.x1 - 1.0f, ry + rowH);
		bool hv = hover == hl && inside && row.Contains (px, py);
		if (i == choiceSel) Fill (row, col[ACTIVE]);
		else if (hv) Fill (row, Scale (col[FILL], 1.8f));
		std::string t = Fit (choiceItems[i].c_str(), row.W() - u * 0.5f, FONT_NORMAL);
		Txt (row.x0 + u * 0.25f, row.CY(), t.c_str(), i == choiceSel ? col[ACCENT] : col[TEXT], ALIGN_LEFT|ALIGN_MIDDLE, FONT_NORMAL);
	}
	ClipTo (nullptr);
}

// =======================================================================
// Plots

void Context::FormatValue (char *buf, int len, double v, int fmt, double span)
{
	if (fmt == AXIS_DATE) {
		struct tm t = *mjddate (v);
		int mon = std::clamp (t.tm_mon, 1, 12);
		if (span > 1500.0)     snprintf (buf, len, "%d", t.tm_year + 1900);
		else if (span > 120.0) snprintf (buf, len, "%s %02d", MONTH[mon], (t.tm_year + 1900) % 100);
		else if (span > 0.0)   snprintf (buf, len, "%d %s", t.tm_mday, MONTH[mon]);
		else                   snprintf (buf, len, "%04d-%02d-%02d", t.tm_year + 1900, mon, t.tm_mday);
		return;
	}
	double a = std::fabs (span > 0.0 ? span : std::fabs (v));
	if (fmt == AXIS_DAYS) {
		if (a >= 20.0 || span <= 0.0) snprintf (buf, len, "%.0f d", v);
		else snprintf (buf, len, "%.1f d", v);
		return;
	}
	if (span > 0.0 && std::fabs (v) < span * 1e-9) v = 0.0; // (no "-0")
	double av = std::fabs (v);
	if (av != 0.0 && (av >= 1e6 || av < 1e-3)) { snprintf (buf, len, "%.3g", v); return; }
	int dec = (a > 0.0) ? std::clamp ((int)std::ceil (-std::log10 (a / 5.0)), 0, 6) : 2;
	snprintf (buf, len, "%.*f", dec, v);
	if (buf[0] == '-' && strspn (buf + 1, "0.") == strlen (buf + 1)) memmove (buf, buf + 1, strlen (buf)); // ("-0.0")
}

DWORD Context::MapColour (float t, int cmap)
{
	t = std::clamp (t, 0.0f, 1.0f);
	float r, g, b;
	if (cmap == CMAP_GREY) {
		r = g = b = 0.15f + 0.85f * t;
	} else {
		static const float stop[5][3] = {
			{ 0.10f, 0.25f, 0.85f }, { 0.00f, 0.70f, 0.90f }, { 0.15f, 0.80f, 0.25f },
			{ 0.95f, 0.85f, 0.10f }, { 0.90f, 0.15f, 0.10f } };
		float f = t * 4.0f;
		int i = std::min (3, (int)f);
		f -= i;
		r = stop[i][0] + (stop[i+1][0] - stop[i][0]) * f;
		g = stop[i][1] + (stop[i+1][1] - stop[i][1]) * f;
		b = stop[i][2] + (stop[i+1][2] - stop[i][2]) * f;
	}
	auto B = [](float c) { return (DWORD)std::clamp ((int)(c * 255.0f + 0.5f), 0, 255); };
	return 0xFF000000 | (B (b) << 16) | (B (g) << 8) | B (r);
}

namespace {
	// ticks of an axis: positions in data units
	void Ticks (double v0, double v1, int fmt, int nmax, std::vector<double> &t)
	{
		t.clear ();
		double span = v1 - v0;
		if (!(span > 0.0)) return;
		if (fmt == AXIS_DATE && span / nmax >= 20.0) {
			// calendar months
			static const int MSTEP[] = { 1, 2, 3, 6, 12, 24, 60, 120 };
			int ms = 120;
			for (int m : MSTEP) if (m * 30.44 >= span / nmax) { ms = m; break; }
			struct tm d0 = *mjddate (v0);
			int y = d0.tm_year + 1900, m = d0.tm_mon;
			int idx = (y * 12 + (m - 1));
			idx = (idx / ms) * ms;
			for (int k = 0; k < 400; k++, idx += ms) {
				double mjd = MonthStart (idx / 12, idx % 12 + 1);
				if (mjd > v1) break;
				if (mjd >= v0) t.push_back (mjd);
			}
			return;
		}
		double step = NiceStep (span, nmax);
		if (fmt == AXIS_DATE || fmt == AXIS_DAYS) {
			static const double DSTEP[] = { 0.25, 0.5, 1, 2, 5, 7, 10, 14, 20, 30, 50, 100, 200, 500, 1000 };
			for (double s : DSTEP) if (s >= span / nmax) { step = s; break; }
		}
		double first = std::ceil (v0 / step) * step;
		for (double v = first; v <= v1 + step * 1e-6 && t.size() < 64; v += step) t.push_back (v);
	}
}

bool Context::BeginPlot (const char *id, const Rect &r, const PlotSpec &spec)
{
	Impl &s = *d;
	if (s.plot.open) EndPlot ();
	uint32_t h = Hash (id);
	Impl::State &st = s.St (h);
	if (!st.init || st.hx0 != spec.x0 || st.hx1 != spec.x1 || st.hy0 != spec.y0 || st.hy1 != spec.y1) {
		st.vx0 = st.hx0 = spec.x0, st.vx1 = st.hx1 = spec.x1;
		st.vy0 = st.hy0 = spec.y0, st.vy1 = st.hy1 = spec.y1;
		st.init = true;
	}

	float u = s.Unit ();
	float sh = s.cellH[FONT_SMALL];
	// margins for the tick labels and axis titles
	char buf[64];
	FormatValue (buf, sizeof (buf), st.vy1, spec.yfmt, st.vy1 - st.vy0);
	float lw = std::max (s.TextW (buf, FONT_SMALL), s.TextW ("-0000", FONT_SMALL));
	FormatValue (buf, sizeof (buf), st.vy0, spec.yfmt, st.vy1 - st.vy0);
	lw = std::max (lw, s.TextW (buf, FONT_SMALL));
	float left = lw + u * 0.4f + (spec.ylabel ? sh * 1.1f : 0.0f);
	float bottom = sh * 1.2f + (spec.xlabel ? sh * 1.1f : 0.0f);
	Rect pa (r.x0 + left, r.y0 + sh * 0.5f, r.x1 - u * 0.3f, r.y1 - bottom);
	if (pa.W() < 10.0f || pa.H() < 10.0f) return false;

	// interaction since the last frame
	s.Add (h, r, K_PLOT, id);
	double sx = (st.vx1 - st.vx0) / pa.W(), sy = (st.vy1 - st.vy0) / pa.H();
	float dx, dy;
	if (spec.pan && s.TakeDrag (h, dx, dy)) {
		st.vx0 -= dx * sx, st.vx1 -= dx * sx;
		st.vy0 += dy * sy, st.vy1 += dy * sy;
	}
	float wh = s.TakeWheel (h);
	if (spec.zoom && wh != 0.0f) {
		double f = std::pow (0.8, (double)wh);
		bool zx = s.py <= pa.y1, zy = s.px >= pa.x0; // over an axis: only that one
		double cx = st.vx0 + (std::clamp (s.px, pa.x0, pa.x1) - pa.x0) * sx;
		double cy = st.vy0 + (pa.y1 - std::clamp (s.py, pa.y0, pa.y1)) * sy;
		if (zx) st.vx0 = cx + (st.vx0 - cx) * f, st.vx1 = cx + (st.vx1 - cx) * f;
		if (zy) st.vy0 = cy + (st.vy0 - cy) * f, st.vy1 = cy + (st.vy1 - cy) * f;
	}

	s.plot.id = h;
	s.plot.area = pa;
	s.plot.x0 = st.vx0, s.plot.x1 = st.vx1, s.plot.y0 = st.vy0, s.plot.y1 = st.vy1;
	s.plot.xfmt = spec.xfmt, s.plot.yfmt = spec.yfmt;
	s.plot.open = true;

	// frame, grid and tick labels
	s.Fill (pa, s.col[BACK]);
	std::vector<double> tx, ty;
	Ticks (st.vx0, st.vx1, spec.xfmt, std::max (2, (int)(pa.W() / (u * (spec.xfmt == AXIS_DATE ? 3.0f : 4.5f)))), tx);
	Ticks (st.vy0, st.vy1, spec.yfmt, std::max (2, (int)(pa.H() / (u * 2.2f))), ty);
	DWORD grid = WithAlpha (s.col[DIM], 0.45f);
	for (double v : tx) {
		float x, y;
		PlotToScreen (v, st.vy0, &x, &y);
		s.PolyLine (std::vector<FVECTOR2>{ { x, pa.y0 }, { x, pa.y1 } }.data(), 2, grid, 1.0f, false);
		FormatValue (buf, sizeof (buf), v, spec.xfmt, st.vx1 - st.vx0);
		s.Txt (x, pa.y1 + sh * 0.1f, buf, s.col[DIM], ALIGN_CENTRE|ALIGN_TOP, FONT_SMALL);
	}
	for (double v : ty) {
		float x, y;
		PlotToScreen (st.vx0, v, &x, &y);
		s.PolyLine (std::vector<FVECTOR2>{ { pa.x0, y }, { pa.x1, y } }.data(), 2, grid, 1.0f, false);
		FormatValue (buf, sizeof (buf), v, spec.yfmt, st.vy1 - st.vy0);
		s.Txt (pa.x0 - u * 0.2f, y, buf, s.col[DIM], ALIGN_RIGHT|ALIGN_MIDDLE, FONT_SMALL);
	}
	if (spec.xlabel) s.Txt (pa.CX(), r.y1, spec.xlabel, s.col[TEXT], ALIGN_CENTRE|ALIGN_BOTTOM, FONT_SMALL);
	if (spec.ylabel) { // (no rotated text everywhere: written above the axis)
		s.Txt (r.x0, r.y0, spec.ylabel, s.col[TEXT], ALIGN_LEFT|ALIGN_TOP, FONT_SMALL);
	}
	s.Frame (pa, s.col[FRAME], 1.0f);
	s.ClipTo (&pa);
	return true;
}

void Context::PlotToScreen (double x, double y, float *sx, float *sy) const
{
	const Impl::Plot &p = d->plot;
	*sx = (float)(p.area.x0 + (x - p.x0) / (p.x1 - p.x0) * p.area.W());
	*sy = (float)(p.area.y1 - (y - p.y0) / (p.y1 - p.y0) * p.area.H());
}

void Context::PlotView (double *x0, double *x1, double *y0, double *y1) const
{
	*x0 = d->plot.x0, *x1 = d->plot.x1, *y0 = d->plot.y0, *y1 = d->plot.y1;
}

void Context::PlotLine (const double *x, const double *y, int n, DWORD col, float width)
{
	if (!d->plot.open || n < 2) return;
	std::vector<FVECTOR2> p;
	p.reserve (n);
	auto Flush = [&]() { if (p.size() > 1) d->PolyLine (p.data(), (int)p.size(), col, width, false); p.clear (); };
	for (int i = 0; i < n; i++) {
		if (!std::isfinite (x[i]) || !std::isfinite (y[i])) { Flush (); continue; }
		float sx, sy;
		PlotToScreen (x[i], y[i], &sx, &sy);
		sx = std::clamp (sx, -1e5f, 1e5f), sy = std::clamp (sy, -1e5f, 1e5f);
		p.push_back ({ sx, sy });
	}
	Flush ();
}

void Context::PlotMarker (double x, double y, DWORD col, int shape, const char *label)
{
	if (!d->plot.open) return;
	float sx, sy;
	PlotToScreen (x, y, &sx, &sy);
	float a = d->Unit () * 0.32f;
	switch (shape) {
	case MARK_CROSS:
		Line (sx - a, sy - a, sx + a, sy + a, col, 1.5f);
		Line (sx - a, sy + a, sx + a, sy - a, col, 1.5f);
		break;
	case MARK_DIAMOND: {
		FVECTOR2 p[4] = { { sx, sy - a }, { sx + a, sy }, { sx, sy + a }, { sx - a, sy } };
		d->PolyLine (p, 4, col, 1.5f, true);
		} break;
	case MARK_SQUARE: {
		FVECTOR2 p[4] = { { sx - a, sy - a }, { sx + a, sy - a }, { sx + a, sy + a }, { sx - a, sy + a } };
		d->PolyLine (p, 4, col, 1.5f, true);
		} break;
	case MARK_TRIANGLE: {
		FVECTOR2 p[3] = { { sx, sy - a }, { sx + a, sy + a * 0.8f }, { sx - a, sy + a * 0.8f } };
		d->PolyLine (p, 3, col, 1.5f, true);
		} break;
	default:
		Circle (sx, sy, a, col, 1.5f);
		break;
	}
	if (label) {
		const Rect &pa = d->plot.area;
		int align = (sx > pa.CX()) ? ALIGN_RIGHT : ALIGN_LEFT;
		float lx = (align == ALIGN_RIGHT) ? sx - a * 1.4f : sx + a * 1.4f;
		d->Txt (lx, sy - a * 1.2f, label, col, align | ALIGN_BOTTOM, FONT_SMALL);
	}
}

void Context::PlotRect (double x0, double y0, double x1, double y1, DWORD col)
{
	if (!d->plot.open) return;
	float ax, ay, bx, by;
	PlotToScreen (x0, y0, &ax, &ay);
	PlotToScreen (x1, y1, &bx, &by);
	d->Fill (Rect (std::min (ax, bx), std::min (ay, by), std::max (ax, bx), std::max (ay, by)), col);
}

void Context::PlotHeatmap (const float *z, int nx, int ny, double x0, double x1, double y0, double y1, float zlo, float zhi, int cmap)
{
	if (!d->plot.open || nx < 2 || ny < 2 || !(zhi > zlo)) return;
	std::vector<DWORD> pix ((size_t)nx * ny);
	for (int j = 0; j < ny; j++)
		for (int i = 0; i < nx; i++) {
			float v = z[(size_t)j * nx + i];
			DWORD c = 0; // transparent
			if (std::isfinite (v) && v <= zhi) c = MapColour ((v - zlo) / (zhi - zlo), cmap);
			pix[(size_t)(ny - 1 - j) * nx + i] = c; // rows from the top
		}
	// the grid values are at the cell centres of the image
	double hx = 0.5 * (x1 - x0) / (nx - 1), hy = 0.5 * (y1 - y0) / (ny - 1);
	float ax, ay, bx, by;
	PlotToScreen (x0 - hx, y1 + hy, &ax, &ay);
	PlotToScreen (x1 + hx, y0 - hy, &bx, &by);
	Image (pix.data(), nx, ny, Rect (ax, ay, bx, by), true);
}

void Context::PlotContours (const float *z, int nx, int ny, double x0, double x1, double y0, double y1, const float *levels, int nlev, DWORD col, float width)
{
	if (!d->plot.open || nx < 2 || ny < 2) return;
	double dx = (x1 - x0) / (nx - 1), dy = (y1 - y0) / (ny - 1);
	std::vector<FVECTOR2> seg;
	auto P = [&](double gx, double gy) {
		float sx, sy;
		PlotToScreen (x0 + gx * dx, y0 + gy * dy, &sx, &sy);
		return FVECTOR2 (sx, sy);
	};
	for (int l = 0; l < nlev; l++) {
		float lv = levels[l];
		for (int j = 0; j + 1 < ny; j++)
			for (int i = 0; i + 1 < nx; i++) {
				float c[4] = { z[(size_t)j*nx + i], z[(size_t)j*nx + i + 1], z[(size_t)(j+1)*nx + i + 1], z[(size_t)(j+1)*nx + i] };
				if (!std::isfinite (c[0]) || !std::isfinite (c[1]) || !std::isfinite (c[2]) || !std::isfinite (c[3])) continue;
				int code = (c[0] > lv) | ((c[1] > lv) << 1) | ((c[2] > lv) << 2) | ((c[3] > lv) << 3);
				if (code == 0 || code == 15) continue;
				// crossing points on the cell's edges: 0 bottom, 1 right, 2 top, 3 left
				static const int CORNER[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
				FVECTOR2 e[4];
				bool has[4] = {};
				for (int k = 0; k < 4; k++) {
					int a = k, b = (k + 1) % 4;
					if ((c[a] > lv) == (c[b] > lv)) continue;
					float t = (lv - c[a]) / (c[b] - c[a]);
					double gx = i + CORNER[a][0] + (CORNER[b][0] - CORNER[a][0]) * t;
					double gy = j + CORNER[a][1] + (CORNER[b][1] - CORNER[a][1]) * t;
					e[k] = P (gx, gy), has[k] = true;
				}
				int idx[4], m = 0;
				for (int k = 0; k < 4; k++) if (has[k]) idx[m++] = k;
				if (m == 2) {
					seg.push_back (e[idx[0]]), seg.push_back (e[idx[1]]);
				} else if (m == 4) { // saddle: pair by the centre value
					float cc = 0.25f * (c[0] + c[1] + c[2] + c[3]);
					bool cAbove = cc > lv, c0Above = c[0] > lv;
					if (cAbove == c0Above) {
						seg.push_back (e[0]), seg.push_back (e[1]);
						seg.push_back (e[2]), seg.push_back (e[3]);
					} else {
						seg.push_back (e[3]), seg.push_back (e[0]);
						seg.push_back (e[1]), seg.push_back (e[2]);
					}
				}
			}
	}
	for (size_t k = 0; k + 1 < seg.size(); k += 2)
		d->PolyLine (&seg[k], 2, col, width, false);
}

bool Context::PlotClicked (double *x, double *y)
{
	Impl &s = *d;
	if (!s.plot.open) return false;
	if (s.clicked != s.plot.id) return false;
	if (!s.plot.area.Contains (s.clickX, s.clickY)) { s.clicked = 0; return false; }
	float cx, cy;
	s.TakeClick (s.plot.id, &cx, &cy);
	const Impl::Plot &p = s.plot;
	*x = p.x0 + (cx - p.area.x0) / p.area.W() * (p.x1 - p.x0);
	*y = p.y0 + (p.area.y1 - cy) / p.area.H() * (p.y1 - p.y0);
	return true;
}

bool Context::PlotHover (double *x, double *y)
{
	Impl &s = *d;
	if (!s.plot.open || s.hover != s.plot.id || !s.inside || !s.plot.area.Contains (s.px, s.py)) return false;
	const Impl::Plot &p = s.plot;
	*x = p.x0 + (s.px - p.area.x0) / p.area.W() * (p.x1 - p.x0);
	*y = p.y0 + (p.area.y1 - s.py) / p.area.H() * (p.y1 - p.y0);
	return true;
}

void Context::EndPlot ()
{
	Impl &s = *d;
	if (!s.plot.open) return;
	s.ClipTo (nullptr);
	s.plot.open = false;
	// reset button when the view was changed
	Impl::State &st = s.St (s.plot.id);
	if (st.vx0 != st.hx0 || st.vx1 != st.hx1 || st.vy0 != st.hy0 || st.vy1 != st.hy1) {
		float u = s.Unit ();
		Rect b (s.plot.area.x1 - u * 2.2f, s.plot.area.y0 + 1.0f, s.plot.area.x1 - 1.0f, s.plot.area.y0 + u);
		char bid[32];
		snprintf (bid, sizeof (bid), "##reset%08x", s.plot.id);
		if (Button (bid, b, "Reset")) {
			st.vx0 = st.hx0, st.vx1 = st.hx1, st.vy0 = st.hy0, st.vy1 = st.hy1;
		}
	}
	s.last = s.plot.id;
}

void Context::ResetPlot (const char *id)
{
	auto it = d->state.find (Hash (id));
	if (it != d->state.end()) it->second.init = false;
}

} // namespace mfdui
