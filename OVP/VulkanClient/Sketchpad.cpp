// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Sketchpad.h"
#include "Fonts.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

	// Points are moved slightly off the pixel centres, so that lines along
	// rows or columns don't depend on the rasteriser's tie-breaking rule.
	constexpr float CENTRE = 0.5f - 1.0f / 64.0f;
	constexpr float DASH_ON = 18.0f, DASH_OFF = 6.0f; // dash pattern [pixels per unit pen width]

	float Cross (float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

} // namespace

VkPen::VkPen (int s, int w, DWORD c) : oapi::Pen (s, w, c), style(s), width((float)std::max (1, w))
{
	ColorRefToLinear (c, 1.0f, col);
}

VkBrush::VkBrush (DWORD c) : oapi::Brush (c)
{
	ColorRefToLinear (c, 1.0f, col);
}

// ======================================================================

VkSketchpad::VkSketchpad (Renderer2D *r, FontManager *f, VkTexture *t)
	: oapi::Sketchpad ((SURFHANDLE)t), r2d(r), fonts(f), tgt(t)
{
	valid = r2d->Begin (tgt);
	textColRef = 0x000000;
	bkColRef = 0xFFFFFF;
	ColorRefToLinear (textColRef, 1.0f, textCol);
	ColorRefToLinear (bkColRef, 1.0f, bkCol);
}

VkSketchpad::~VkSketchpad ()
{
	r2d->Flush ();
}

DrawState2D VkSketchpad::SolidState ()
{
	// other sketchpads may have drawn to a different target in the meantime
	r2d->Begin (tgt);
	DrawState2D st;
	st.clip = clip;
	return st;
}

VkFont *VkSketchpad::CurrentFont ()
{
	if (font) return font;
	if (!defFont) defFont.reset (fonts->Create (-14, true, "Sans", 0, 400, FONT_NORMAL, 0, 0.0f));
	return defFont.get();
}

// ======================================================================
// State

oapi::Font *VkSketchpad::SetFont (oapi::Font *f)
{
	oapi::Font *prev = font;
	font = (VkFont*)f;
	return prev;
}

oapi::Pen *VkSketchpad::SetPen (oapi::Pen *p)
{
	oapi::Pen *prev = pen;
	pen = (VkPen*)p;
	return prev;
}

oapi::Brush *VkSketchpad::SetBrush (oapi::Brush *b)
{
	oapi::Brush *prev = brush;
	brush = (VkBrush*)b;
	return prev;
}

void VkSketchpad::SetTextAlign (TAlign_horizontal h, TAlign_vertical v)
{
	tah = h, tav = v;
}

DWORD VkSketchpad::SetTextColor (DWORD col)
{
	DWORD prev = textColRef;
	textColRef = col;
	ColorRefToLinear (col, 1.0f, textCol);
	return prev;
}

DWORD VkSketchpad::SetBackgroundColor (DWORD col)
{
	DWORD prev = bkColRef;
	bkColRef = col;
	ColorRefToLinear (col, 1.0f, bkCol);
	return prev;
}

void VkSketchpad::SetBackgroundMode (BkgMode mode)
{
	bkOpaque = (mode == BK_OPAQUE);
}

DWORD VkSketchpad::GetCharSize ()
{
	VkFont *f = CurrentFont ();
	if (!f) return 0;
	return ((DWORD)f->atlas->avgWidth << 16) | (DWORD)(f->atlas->CellHeight() & 0xFFFF);
}

DWORD VkSketchpad::GetTextWidth (const char *str, int len)
{
	VkFont *f = CurrentFont ();
	if (!f || !str) return 0;
	if (!len) len = (int)strlen (str);
	return (DWORD)f->atlas->TextWidth (str, len, f->spacing);
}

void VkSketchpad::SetOrigin (int x, int y)
{
	ox = x, oy = y;
}

void VkSketchpad::GetOrigin (int *x, int *y) const
{
	*x = ox, *y = oy;
}

void VkSketchpad::GetRenderSurfaceSize (LPSIZE size)
{
	VkExtent2D ext = r2d->TargetSize ();
	size->cx = ext.width, size->cy = ext.height;
}

void VkSketchpad::QuickPen (DWORD color, float width, DWORD style)
{
	quickPen = std::make_unique<VkPen> ((int)style, (int)std::lround (width), color);
	quickPen->width = std::max (1.0f, width);
	pen = quickPen.get();
}

void VkSketchpad::QuickBrush (DWORD color)
{
	quickBrush = std::make_unique<VkBrush> (color);
	brush = quickBrush.get();
}

void VkSketchpad::ClipRect (const LPRECT r)
{
	if (r && r->right > r->left && r->bottom > r->top)
		clip = { { r->left + ox, r->top + oy }, { (uint32_t)(r->right - r->left), (uint32_t)(r->bottom - r->top) } };
	else
		clip = { { 0, 0 }, { 0, 0 } };
}

// ======================================================================
// Primitives

void VkSketchpad::Tri (const DrawState2D &st, P a, P b, P c, const float col[4])
{
	Vtx2D *v = r2d->AddTris (st, 3);
	if (!v) return;
	v[0] = { a.x, a.y, 0, 0, col[0], col[1], col[2], col[3] };
	v[1] = { b.x, b.y, 0, 0, col[0], col[1], col[2], col[3] };
	v[2] = { c.x, c.y, 0, 0, col[0], col[1], col[2], col[3] };
}

void VkSketchpad::Segment (P a, P b, float w, const float col[4])
{
	float dx = b.x - a.x, dy = b.y - a.y;
	float len = std::sqrt (dx*dx + dy*dy);
	if (len < 1e-4f) return;
	float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
	DrawState2D st = SolidState ();
	P p0{ a.x + nx, a.y + ny }, p1{ b.x + nx, b.y + ny }, p2{ b.x - nx, b.y - ny }, p3{ a.x - nx, a.y - ny };
	Tri (st, p0, p1, p2, col);
	Tri (st, p0, p2, p3, col);
}

void VkSketchpad::Stroke (const P *pts, int n, bool closed)
{
	if (!pen || pen->style == 0 || n < 2) return;
	float w = pen->width;
	int nseg = closed ? n : n - 1;
	for (int i = 0; i < nseg; i++) {
		P a = pts[i], b = pts[(i + 1) % n];
		if (pen->style == 2) {
			// dashed: split the segment according to the running dash pattern
			float on = DASH_ON * w, period = (DASH_ON + DASH_OFF) * w;
			float dx = b.x - a.x, dy = b.y - a.y;
			float len = std::sqrt (dx*dx + dy*dy);
			float s = 0.0f;
			while (s < len) {
				float ph = std::fmod (dashPos, period);
				float step = (ph < on) ? std::min (on - ph, len - s) : std::min (period - ph, len - s);
				if (ph < on)
					Segment ({ a.x + dx * s / len, a.y + dy * s / len }, { a.x + dx * (s + step) / len, a.y + dy * (s + step) / len }, w, pen->col);
				s += step;
				dashPos += step;
				if (step <= 0.0f) break;
			}
		} else {
			Segment (a, b, w, pen->col);
		}
		// fill the joints of wide lines
		if (w > 1.5f && (closed || i < nseg - 1)) {
			float h = w * 0.5f;
			DrawState2D st = SolidState ();
			Tri (st, { b.x - h, b.y - h }, { b.x + h, b.y - h }, { b.x + h, b.y + h }, pen->col);
			Tri (st, { b.x - h, b.y - h }, { b.x + h, b.y + h }, { b.x - h, b.y + h }, pen->col);
		}
	}
}

void VkSketchpad::Fill (const P *pts, int n)
{
	if (!brush || n < 3) return;
	DrawState2D st = SolidState ();

	// ear clipping triangulation of a simple polygon
	std::vector<int> idx;
	idx.reserve (n);
	for (int i = 0; i < n; i++)
		if (idx.empty() || pts[i].x != pts[idx.back()].x || pts[i].y != pts[idx.back()].y) idx.push_back (i);
	if (idx.size() > 1 && pts[idx.front()].x == pts[idx.back()].x && pts[idx.front()].y == pts[idx.back()].y) idx.pop_back();
	if (idx.size() < 3) return;
	float area = 0.0f;
	for (size_t i = 0; i < idx.size(); i++) {
		const P &a = pts[idx[i]], &b = pts[idx[(i + 1) % idx.size()]];
		area += Cross (a.x, a.y, b.x, b.y);
	}
	float sgn = (area >= 0.0f) ? 1.0f : -1.0f;

	while (idx.size() > 3) {
		size_t m = idx.size();
		bool found = false;
		for (size_t i = 0; i < m; i++) {
			const P &a = pts[idx[(i + m - 1) % m]], &b = pts[idx[i]], &c = pts[idx[(i + 1) % m]];
			float cr = Cross (b.x - a.x, b.y - a.y, c.x - b.x, c.y - b.y) * sgn;
			if (cr <= 0.0f) continue; // reflex or degenerate
			bool inside = false;
			for (size_t j = 0; j < m && !inside; j++) {
				if (j == i || j == (i + 1) % m || j == (i + m - 1) % m) continue;
				const P &p = pts[idx[j]];
				float d0 = Cross (b.x - a.x, b.y - a.y, p.x - a.x, p.y - a.y) * sgn;
				float d1 = Cross (c.x - b.x, c.y - b.y, p.x - b.x, p.y - b.y) * sgn;
				float d2 = Cross (a.x - c.x, a.y - c.y, p.x - c.x, p.y - c.y) * sgn;
				inside = (d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f);
			}
			if (inside) continue;
			Tri (st, a, b, c, brush->col);
			idx.erase (idx.begin() + i);
			found = true;
			break;
		}
		if (!found) break; // not a simple polygon: fan the remainder
	}
	for (size_t i = 1; i + 1 < idx.size(); i++)
		Tri (st, pts[idx[0]], pts[idx[i]], pts[idx[i + 1]], brush->col);
}

void VkSketchpad::Pixel (int x, int y, DWORD col)
{
	float c[4];
	ColorRefToLinear (col, 1.0f, c);
	x += ox, y += oy;
	r2d->Quad (SolidState (), (float)x, (float)y, (float)(x + 1), (float)(y + 1), 0, 0, 0, 0, c);
}

void VkSketchpad::MoveTo (int x, int y)
{
	curx = x, cury = y;
	dashPos = 0.0f;
}

void VkSketchpad::LineTo (int x, int y)
{
	P p[2] = { { curx + ox + CENTRE, cury + oy + CENTRE }, { x + ox + CENTRE, y + oy + CENTRE } };
	Stroke (p, 2, false);
	curx = x, cury = y;
}

void VkSketchpad::Line (int x0, int y0, int x1, int y1)
{
	MoveTo (x0, y0);
	LineTo (x1, y1);
}

void VkSketchpad::Rectangle (int x0, int y0, int x1, int y1)
{
	if (x0 > x1) std::swap (x0, x1);
	if (y0 > y1) std::swap (y0, y1);
	x0 += ox, x1 += ox, y0 += oy, y1 += oy;
	DrawState2D st = SolidState ();
	if (brush)
		r2d->Quad (st, (float)x0, (float)y0, (float)x1, (float)y1, 0, 0, 0, 0, brush->col);
	if (!pen || pen->style == 0) return;
	if (pen->style == 2) {
		P p[4] = { { x0 + CENTRE, y0 + CENTRE }, { x1 - 1 + CENTRE, y0 + CENTRE },
		           { x1 - 1 + CENTRE, y1 - 1 + CENTRE }, { x0 + CENTRE, y1 - 1 + CENTRE } };
		dashPos = 0.0f;
		Stroke (p, 4, true);
		return;
	}
	float w = std::max (1.0f, std::round (pen->width));
	float fx0 = (float)x0, fx1 = (float)x1, fy0 = (float)y0, fy1 = (float)y1;
	if (fx1 - fx0 <= 2*w || fy1 - fy0 <= 2*w) {
		r2d->Quad (st, fx0, fy0, fx1, fy1, 0, 0, 0, 0, pen->col);
		return;
	}
	r2d->Quad (st, fx0, fy0, fx1, fy0 + w, 0, 0, 0, 0, pen->col);
	r2d->Quad (st, fx0, fy1 - w, fx1, fy1, 0, 0, 0, 0, pen->col);
	r2d->Quad (st, fx0, fy0 + w, fx0 + w, fy1 - w, 0, 0, 0, 0, pen->col);
	r2d->Quad (st, fx1 - w, fy0 + w, fx1, fy1 - w, 0, 0, 0, 0, pen->col);
}

void VkSketchpad::Ellipse (int x0, int y0, int x1, int y1)
{
	if (x0 > x1) std::swap (x0, x1);
	if (y0 > y1) std::swap (y0, y1);
	float cx = 0.5f * (x0 + x1) + ox, cy = 0.5f * (y0 + y1) + oy;
	float rx = 0.5f * (x1 - x0), ry = 0.5f * (y1 - y0);
	if (rx <= 0.0f || ry <= 0.0f) return;
	int n = std::clamp ((int)(2.0f * 3.14159265f * std::max (rx, ry) / 3.0f), 12, 180);
	std::vector<P> p (n);
	if (brush) {
		for (int i = 0; i < n; i++) {
			float a = 2.0f * 3.14159265f * i / n;
			p[i] = { cx + rx * std::cos (a), cy + ry * std::sin (a) };
		}
		Fill (p.data(), n);
	}
	if (pen && pen->style) {
		float h = 0.5f * pen->width;
		for (int i = 0; i < n; i++) {
			float a = 2.0f * 3.14159265f * i / n;
			p[i] = { cx + std::max (0.0f, rx - h) * std::cos (a), cy + std::max (0.0f, ry - h) * std::sin (a) };
		}
		dashPos = 0.0f;
		Stroke (p.data(), n, true);
	}
}

void VkSketchpad::Polygon (const oapi::IVECTOR2 *pt, int npt)
{
	if (npt < 2) return;
	std::vector<P> p (npt);
	for (int i = 0; i < npt; i++) p[i] = { pt[i].x + ox + CENTRE, pt[i].y + oy + CENTRE };
	Fill (p.data(), npt);
	dashPos = 0.0f;
	Stroke (p.data(), npt, true);
}

void VkSketchpad::Polyline (const oapi::IVECTOR2 *pt, int npt)
{
	if (npt < 2) return;
	std::vector<P> p (npt);
	for (int i = 0; i < npt; i++) p[i] = { pt[i].x + ox + CENTRE, pt[i].y + oy + CENTRE };
	dashPos = 0.0f;
	Stroke (p.data(), npt, false);
}

// ======================================================================
// Text

bool VkSketchpad::Text (int x, int y, const char *str, int len)
{
	VkFont *f = CurrentFont ();
	if (!f || !str) return false;
	if (len < 0) len = (int)strlen (str);
	const GlyphAtlas &at = *f->atlas;
	if (!at.tex) return false;
	x += ox, y += oy;

	int w = (int)GetTextWidth (str, len);
	float lx0 = (tah == CENTER) ? -(float)(w / 2) : (tah == RIGHT) ? -(float)w : 0.0f;
	float base = (tav == TOP) ? (float)at.ascent : (tav == BOTTOM) ? -(float)at.descent : 0.0f;
	float ca = std::cos (f->angle), sa = std::sin (f->angle);
	bool rotated = (f->angle != 0.0f) || f->synthItalic;
	float shear = f->synthItalic ? 0.2f : 0.0f;
	auto Xf = [&](float lx, float ly) -> P {
		lx += shear * (base - ly);
		return { x + lx * ca + ly * sa, y - lx * sa + ly * ca };
	};
	auto LocalQuad = [&](const DrawState2D &st, float lx0, float ly0, float lx1, float ly1,
		float u0, float v0, float u1, float v1, const float col[4]) {
		Vtx2D *v = r2d->AddTris (st, 6);
		if (!v) return;
		P a = Xf (lx0, ly0), b = Xf (lx1, ly0), c = Xf (lx1, ly1), d = Xf (lx0, ly1);
		Vtx2D va{ a.x, a.y, u0, v0, col[0], col[1], col[2], col[3] };
		Vtx2D vb{ b.x, b.y, u1, v0, col[0], col[1], col[2], col[3] };
		Vtx2D vc{ c.x, c.y, u1, v1, col[0], col[1], col[2], col[3] };
		Vtx2D vd{ d.x, d.y, u0, v1, col[0], col[1], col[2], col[3] };
		v[0] = va, v[1] = vb, v[2] = vc, v[3] = va, v[4] = vc, v[5] = vd;
	};

	if (bkOpaque)
		LocalQuad (SolidState (), lx0, base - at.ascent, lx0 + w, base + at.descent, 0, 0, 0, 0, bkCol);

	DrawState2D st = SolidState ();
	st.tex = at.tex;
	st.fetch = !rotated;
	float iw = 1.0f / at.tex->width, ih = 1.0f / at.tex->height;
	float pen_x = lx0;
	for (const char *p = str, *end = str + len; p < end; ) {
		const GlyphAtlas::Glyph &g = at.glyph[GlyphAtlas::NextChar (p, end)];
		if (g.w > 0 && g.h > 0) {
			float gx0 = pen_x + g.xoff, gy0 = base + g.yoff;
			if (st.fetch) gx0 = std::round (gx0);
			if (st.fetch)
				LocalQuad (st, gx0, gy0, gx0 + g.w, gy0 + g.h,
					(float)g.ax, (float)g.ay, (float)(g.ax + g.w), (float)(g.ay + g.h), textCol);
			else
				LocalQuad (st, gx0, gy0, gx0 + g.w, gy0 + g.h,
					g.ax * iw, g.ay * ih, (g.ax + g.w) * iw, (g.ay + g.h) * ih, textCol);
		}
		pen_x += g.advance + f->spacing;
	}

	float th = std::max (1.0f, std::round (at.CellHeight() / 16.0f));
	if (f->style & FONT_UNDERLINE) {
		float uy = base + std::max (1.0f, std::round (at.descent * 0.4f));
		LocalQuad (SolidState (), lx0, uy, lx0 + w, uy + th, 0, 0, 0, 0, textCol);
	}
	if (f->style & FONT_STRIKEOUT) {
		float sy = base - std::round (at.ascent * 0.3f);
		LocalQuad (SolidState (), lx0, sy, lx0 + w, sy + th, 0, 0, 0, 0, textCol);
	}
	return true;
}

// ======================================================================
// Surface copies

void VkSketchpad::CopyRect (const SURFHANDLE hSrc, const LPRECT src, int tx, int ty)
{
	VkTexture *s = (VkTexture*)hSrc;
	if (!s || !src) return;
	r2d->Blit (tgt, tx + ox, ty + oy, src->right - src->left, src->bottom - src->top,
		s, src->left, src->top, src->right - src->left, src->bottom - src->top, false);
}

void VkSketchpad::StretchRect (const SURFHANDLE hSrc, const LPRECT src, const LPRECT t)
{
	VkTexture *s = (VkTexture*)hSrc;
	if (!s) return;
	RECT sr = src ? *src : RECT{ 0, 0, (LONG)s->width, (LONG)s->height };
	VkExtent2D ext = r2d->TargetSize ();
	RECT tr = t ? *t : RECT{ 0, 0, (LONG)ext.width, (LONG)ext.height };
	r2d->Blit (tgt, tr.left + ox, tr.top + oy, tr.right - tr.left, tr.bottom - tr.top,
		s, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, false);
}

void VkSketchpad::ColorKey (const SURFHANDLE hSrc, const LPRECT src, int tx, int ty)
{
	VkTexture *s = (VkTexture*)hSrc;
	if (!s || !src) return;
	uint32_t prev = s->ckey;
	if (s->ckey == VkTexture::NO_CKEY) s->ckey = 0;
	r2d->Blit (tgt, tx + ox, ty + oy, src->right - src->left, src->bottom - src->top,
		s, src->left, src->top, src->right - src->left, src->bottom - src->top, true);
	r2d->Flush (); // the colour key is part of the batch state
	s->ckey = prev;
}
