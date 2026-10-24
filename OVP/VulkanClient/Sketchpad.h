// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sketchpad implementation: GDI-style drawing converted into triangle
// batches of the 2D renderer
// =======================================================================

#ifndef __SKETCHPAD_H
#define __SKETCHPAD_H

#include "DrawAPI.h"
#include "Draw2D.h"
#include <memory>

class VkFont;
class FontManager;

class VkPen : public oapi::Pen {
public:
	VkPen (int style, int width, DWORD col);
	int style;      ///< 0: invisible, 1: solid, 2: dashed
	float width;    ///< [pixels]
	float col[4];   ///< linear colour
};

class VkBrush : public oapi::Brush {
public:
	explicit VkBrush (DWORD col);
	float col[4];
};

class VkSketchpad : public oapi::Sketchpad {
public:
	/// tgt: target surface (nullptr: backbuffer)
	VkSketchpad (Renderer2D *r2d, FontManager *fonts, VkTexture *tgt);
	~VkSketchpad ();

	oapi::Font *SetFont (oapi::Font *font) override;
	oapi::Pen *SetPen (oapi::Pen *pen) override;
	oapi::Brush *SetBrush (oapi::Brush *brush) override;
	void SetTextAlign (TAlign_horizontal tah = LEFT, TAlign_vertical tav = TOP) override;
	DWORD SetTextColor (DWORD col) override;
	DWORD SetBackgroundColor (DWORD col) override;
	void SetBackgroundMode (BkgMode mode) override;
	DWORD GetCharSize () override;
	DWORD GetTextWidth (const char *str, int len = 0) override;
	void SetOrigin (int x, int y) override;
	void GetOrigin (int *x, int *y) const override;
	bool Text (int x, int y, const char *str, int len) override;
	void Pixel (int x, int y, DWORD col) override;
	void MoveTo (int x, int y) override;
	void LineTo (int x, int y) override;
	void Line (int x0, int y0, int x1, int y1) override;
	void Rectangle (int x0, int y0, int x1, int y1) override;
	void Ellipse (int x0, int y0, int x1, int y1) override;
	void Polygon (const oapi::IVECTOR2 *pt, int npt) override;
	void Polyline (const oapi::IVECTOR2 *pt, int npt) override;
	void GetRenderSurfaceSize (LPSIZE size) override;
	void QuickPen (DWORD color, float width = 1.0f, DWORD style = 1) override;
	void QuickBrush (DWORD color) override;
	void ClipRect (const LPRECT pClip = NULL) override;
	void CopyRect (const SURFHANDLE hSrc, const LPRECT src, int tx, int ty) override;
	void StretchRect (const SURFHANDLE hSrc, const LPRECT src = NULL, const LPRECT tgt = NULL) override;
	void ColorKey (const SURFHANDLE hSrc, const LPRECT src, int tx, int ty) override;

	bool Valid () const { return valid; }

private:
	struct P { float x, y; };
	DrawState2D SolidState (); ///< also selects the sketchpad's target in the renderer
	/// stroke a path of points in continuous pixel coordinates with the current pen
	void Stroke (const P *pts, int n, bool closed);
	/// fill a simple polygon with the current brush
	void Fill (const P *pts, int n);
	void Tri (const DrawState2D &st, P a, P b, P c, const float col[4]);
	void Segment (P a, P b, float w, const float col[4]);
	VkFont *CurrentFont ();

	Renderer2D *r2d;
	FontManager *fonts;
	VkTexture *tgt;
	bool valid;

	VkFont *font = nullptr;
	VkPen *pen = nullptr;
	VkBrush *brush = nullptr;
	std::unique_ptr<VkFont> defFont;
	std::unique_ptr<VkPen> quickPen;
	std::unique_ptr<VkBrush> quickBrush;
	TAlign_horizontal tah = LEFT;
	TAlign_vertical tav = TOP;
	DWORD textColRef = 0xFFFFFF, bkColRef = 0x000000;
	float textCol[4], bkCol[4];
	bool bkOpaque = false;
	int ox = 0, oy = 0;      // origin
	int curx = 0, cury = 0;  // pen position
	float dashPos = 0.0f;    // position in the dash pattern
	VkRect2D clip{ {0, 0}, {0, 0} };
};

#endif // !__SKETCHPAD_H
