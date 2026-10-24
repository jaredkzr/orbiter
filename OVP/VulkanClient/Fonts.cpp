// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Fonts.h"
#include "VkTexture.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

struct FontFile {
	std::vector<unsigned char> data;
	stbtt_fontinfo info;
	int unitsAscent = 0, unitsDescent = 0; ///< Windows cell metrics (font units)
	int unitsAvgWidth = 0;
};

namespace {

	// Code points of the Windows-1252 characters 0x80-0x9F (0: undefined)
	const uint16_t cp1252[32] = {
		0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
		0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
	};

	int CodePoint (int c)
	{
		if (c >= 0x80 && c < 0xA0) return cp1252[c - 0x80];
		return c;
	}

	bool FileExists (const std::string &path)
	{
		FILE *f = fopen (path.c_str(), "rb");
		if (!f) return false;
		fclose (f);
		return true;
	}

	std::string Lower (const char *s)
	{
		std::string r (s ? s : "");
		for (auto &c : r) c = (char)tolower ((unsigned char)c);
		return r;
	}

	// ------------------------------------------------------------------
	// fontconfig, loaded at runtime so that it isn't a build dependency

	struct FontConfig {
		typedef void FcPattern;
		typedef unsigned char FcChar8;
		typedef int FcBool;
		typedef int FcResult; // FcResultMatch = 0

		FcBool (*Init)() = nullptr;
		FcPattern *(*PatternCreate)() = nullptr;
		FcBool (*PatternAddString)(FcPattern*, const char*, const FcChar8*) = nullptr;
		FcBool (*PatternAddInteger)(FcPattern*, const char*, int) = nullptr;
		FcBool (*ConfigSubstitute)(void*, FcPattern*, int) = nullptr;
		void (*DefaultSubstitute)(FcPattern*) = nullptr;
		FcPattern *(*FontMatch)(void*, FcPattern*, FcResult*) = nullptr;
		FcResult (*PatternGetString)(const FcPattern*, const char*, int, FcChar8**) = nullptr;
		FcResult (*PatternGetInteger)(const FcPattern*, const char*, int, int*) = nullptr;
		void (*PatternDestroy)(FcPattern*) = nullptr;
		bool ok = false;

		FontConfig () {
#ifndef _WIN32
			void *lib = dlopen ("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
			if (!lib) return;
			auto sym = [lib](const char *name) { return dlsym (lib, name); };
			Init = (FcBool(*)())sym ("FcInit");
			PatternCreate = (FcPattern*(*)())sym ("FcPatternCreate");
			PatternAddString = (FcBool(*)(FcPattern*, const char*, const FcChar8*))sym ("FcPatternAddString");
			PatternAddInteger = (FcBool(*)(FcPattern*, const char*, int))sym ("FcPatternAddInteger");
			ConfigSubstitute = (FcBool(*)(void*, FcPattern*, int))sym ("FcConfigSubstitute");
			DefaultSubstitute = (void(*)(FcPattern*))sym ("FcDefaultSubstitute");
			FontMatch = (FcPattern*(*)(void*, FcPattern*, FcResult*))sym ("FcFontMatch");
			PatternGetString = (FcResult(*)(const FcPattern*, const char*, int, FcChar8**))sym ("FcPatternGetString");
			PatternGetInteger = (FcResult(*)(const FcPattern*, const char*, int, int*))sym ("FcPatternGetInteger");
			PatternDestroy = (void(*)(FcPattern*))sym ("FcPatternDestroy");
			ok = Init && PatternCreate && PatternAddString && PatternAddInteger && ConfigSubstitute &&
				DefaultSubstitute && FontMatch && PatternGetString && PatternGetInteger && PatternDestroy && Init();
#endif
		}
	};

	FontConfig &Fc ()
	{
		static FontConfig fc;
		return fc;
	}

	constexpr int FC_WEIGHT_REGULAR = 80, FC_WEIGHT_BOLD = 200;
	constexpr int FC_SLANT_ROMAN = 0, FC_SLANT_ITALIC = 100;

	int FcWeight (int gdiWeight)
	{
		if (gdiWeight <= 150) return 0;    // thin
		if (gdiWeight <= 250) return 40;   // extralight
		if (gdiWeight <= 350) return 50;   // light
		if (gdiWeight <= 450) return 80;   // regular
		if (gdiWeight <= 550) return 100;  // medium
		if (gdiWeight <= 650) return 180;  // demibold
		if (gdiWeight <= 750) return 200;  // bold
		if (gdiWeight <= 850) return 205;  // extrabold
		return 210;                        // black
	}

} // namespace

// ======================================================================

GlyphAtlas::~GlyphAtlas ()
{
	if (tex && texmgr) texmgr->Release (tex);
}

int GlyphAtlas::NextChar (const char *&p, const char *end)
{
	unsigned char c = (unsigned char)*p++;
	if (c < 0x80) return c;
	int n = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 0;
	if (!n || end - p < n) return c;
	int cp = c & (0x3F >> n);
	for (int i = 0; i < n; i++) {
		unsigned char cc = (unsigned char)p[i];
		if ((cc & 0xC0) != 0x80) return c; // not UTF-8: a single-byte character
		cp = (cp << 6) | (cc & 0x3F);
	}
	p += n;
	if (cp >= 0xA0 && cp < 0x100) return cp;
	for (int i = 0; i < 32; i++)
		if (cp1252[i] == cp) return 0x80 + i;
	return '?';
}

int GlyphAtlas::TextWidth (const char *str, int len, float spacing) const
{
	float w = 0.0f;
	for (const char *p = str, *end = str + len; p < end; )
		w += glyph[NextChar (p, end)].advance + spacing;
	return (int)std::lround (w);
}

VkFont::VkFont (std::shared_ptr<GlyphAtlas> a, int height, bool prop, const char *face, FontStyle st, int orientation, float sp)
	: oapi::Font (height, prop, face, st, orientation), atlas(std::move (a)), style(st), spacing(sp), synthItalic(false)
{
	angle = (float)(orientation * 0.1 * 3.14159265358979323846 / 180.0);
}

FontManager::FontManager (TextureManager *tm) : texmgr(tm)
{
}

FontManager::~FontManager ()
{
}

FontManager::Located FontManager::Locate (const char *face, bool prop, int weight, bool italic)
{
	std::string f = Lower (face);
	bool mono = !prop || f == "fixed" || f.find ("courier") != std::string::npos || f.find ("mono") != std::string::npos;
	const char *generic = (f == "serif" || f.find ("times") != std::string::npos) ? "serif" : (mono ? "monospace" : "sans-serif");

	Located loc;
	FontConfig &fc = Fc();
	if (fc.ok) {
		void *pat = fc.PatternCreate ();
		if (f != "fixed" && f != "sans" && f != "serif" && !f.empty())
			fc.PatternAddString (pat, "family", (const unsigned char*)face);
		fc.PatternAddString (pat, "family", (const unsigned char*)generic);
		fc.PatternAddInteger (pat, "weight", FcWeight (weight));
		fc.PatternAddInteger (pat, "slant", italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
		fc.ConfigSubstitute (nullptr, pat, 0 /*FcMatchPattern*/);
		fc.DefaultSubstitute (pat);
		int res = 0;
		void *match = fc.FontMatch (nullptr, pat, &res);
		if (match) {
			unsigned char *file = nullptr;
			int idx = 0, w = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
			if (fc.PatternGetString (match, "file", 0, &file) == 0 && file) {
				loc.path = (const char*)file;
				if (fc.PatternGetInteger (match, "index", 0, &idx) == 0) loc.index = idx;
				fc.PatternGetInteger (match, "weight", 0, &w);
				fc.PatternGetInteger (match, "slant", 0, &slant);
				loc.bold = (w >= 180);
				loc.italic = (slant != FC_SLANT_ROMAN);
			}
			fc.PatternDestroy (match);
		}
		fc.PatternDestroy (pat);
		if (!loc.path.empty()) return loc;
	}

	// fall back to the fonts installed with Orbiter, or common system fonts
	static const char *monoFiles[] = {
		"Fonts/Cousine-Regular.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"C:/Windows/Fonts/cour.ttf",
	};
	static const char *sansFiles[] = {
		"Fonts/Roboto-Medium.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"C:/Windows/Fonts/arial.ttf",
	};
	const char **list = mono ? monoFiles : sansFiles;
	for (int i = 0; i < 3; i++)
		if (FileExists (list[i])) { loc.path = list[i]; break; }
	return loc;
}

FontFile *FontManager::LoadFile (const std::string &path, int index)
{
	std::string key = path + "#" + std::to_string (index);
	auto it = files.find (key);
	if (it != files.end()) return it->second.get();

	FILE *f = fopen (path.c_str(), "rb");
	if (!f) return nullptr;
	auto ff = std::make_unique<FontFile>();
	fseek (f, 0, SEEK_END);
	long size = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (size > 0) {
		ff->data.resize (size);
		if (fread (ff->data.data(), 1, size, f) != (size_t)size) ff->data.clear();
	}
	fclose (f);
	int ofs = ff->data.empty() ? -1 : stbtt_GetFontOffsetForIndex (ff->data.data(), index);
	if (ofs < 0 || !stbtt_InitFont (&ff->info, ff->data.data(), ofs)) {
		oapiWriteLogV ("VulkanClient: could not load font %s", path.c_str());
		files[key] = nullptr;
		return nullptr;
	}

	// GDI cell metrics come from the Windows ascent/descent of the OS/2 table
	int asc, desc, gap;
	stbtt_GetFontVMetrics (&ff->info, &asc, &desc, &gap);
	ff->unitsAscent = asc, ff->unitsDescent = -desc;
	stbtt_uint32 os2 = stbtt__find_table (ff->info.data, ff->info.fontstart, "OS/2");
	if (os2) {
		stbtt_uint8 *p = ff->info.data + os2;
		ff->unitsAvgWidth = ttSHORT (p + 2);
		int winAsc = ttUSHORT (p + 74), winDesc = ttUSHORT (p + 76);
		if (winAsc + winDesc > 0) ff->unitsAscent = winAsc, ff->unitsDescent = winDesc;
	}
	if (ff->unitsAvgWidth <= 0) {
		int adv, lsb;
		stbtt_GetCodepointHMetrics (&ff->info, 'x', &adv, &lsb);
		ff->unitsAvgWidth = adv;
	}
	FontFile *res = ff.get();
	files[key] = std::move (ff);
	return res;
}

VkFont *FontManager::Create (int height, bool prop, const char *face, int width, int weight,
	FontStyle style, int orientation, float spacing)
{
	if (style & FONT_BOLD) weight = std::max (weight, 700);
	bool italic = (style & FONT_ITALIC) != 0;
	Located loc = Locate (face, prop, weight, italic);
	FontFile *ff = loc.path.empty() ? nullptr : LoadFile (loc.path, loc.index);
	if (!ff) {
		oapiWriteLogV ("VulkanClient: no font file found for \"%s\"", face ? face : "");
		return nullptr;
	}
	bool synthBold = (weight >= 600 && !loc.bold);
	bool crisp = (style & FONT_CRISP) != 0;
	if (!height) height = -16;

	std::string key = loc.path + "#" + std::to_string (loc.index) + "/" + std::to_string (height) + "/" +
		std::to_string (width) + (synthBold ? "b" : "") + (crisp ? "c" : "");
	std::shared_ptr<GlyphAtlas> atlas = atlases[key].lock();

	if (!atlas) {
		const stbtt_fontinfo *info = &ff->info;
		float scale = (height < 0) ? stbtt_ScaleForMappingEmToPixels (info, (float)-height)
		                           : (float)height / (ff->unitsAscent + ff->unitsDescent);
		float xscale = scale;
		if (width > 0 && ff->unitsAvgWidth > 0) xscale = (float)width / ff->unitsAvgWidth;

		atlas = std::make_shared<GlyphAtlas>();
		atlas->texmgr = texmgr;
		atlas->ascent = (int)std::lround (ff->unitsAscent * scale);
		atlas->descent = (int)std::lround (ff->unitsDescent * scale);
		atlas->avgWidth = std::max (1, (int)std::lround (ff->unitsAvgWidth * xscale));

		// rasterise characters 0x20-0xFF onto shelves of a 512 texel wide atlas
		const int W = 512, PAD = 1;
		struct Bitmap { int w, h; std::vector<uint8_t> px; };
		std::vector<Bitmap> bmp (256);
		int x = PAD, y = PAD, shelf = 0;
		for (int c = 32; c < 256; c++) {
			int cp = CodePoint (c);
			int g = cp ? stbtt_FindGlyphIndex (info, cp) : 0;
			if (!g && c != 32) g = stbtt_FindGlyphIndex (info, '?');
			GlyphAtlas::Glyph &gl = atlas->glyph[c];
			int adv, lsb;
			stbtt_GetGlyphHMetrics (info, g, &adv, &lsb);
			gl.advance = (int)std::lround (adv * xscale) + (synthBold ? 1 : 0);
			int x0, y0, x1, y1;
			stbtt_GetGlyphBitmapBox (info, g, xscale, scale, &x0, &y0, &x1, &y1);
			int w = x1 - x0, h = y1 - y0;
			if (w <= 0 || h <= 0) continue;
			Bitmap &b = bmp[c];
			b.w = w + (synthBold ? 1 : 0), b.h = h;
			b.px.assign ((size_t)b.w * b.h, 0);
			stbtt_MakeGlyphBitmap (info, b.px.data(), w, h, b.w, xscale, scale, g);
			if (synthBold) // smear one pixel to the right
				for (int r = 0; r < h; r++)
					for (int i = b.w - 1; i > 0; i--)
						b.px[r*b.w + i] = std::max (b.px[r*b.w + i], b.px[r*b.w + i - 1]);
			if (crisp)
				for (auto &p : b.px) p = (p >= 128 ? 255 : 0);
			if (x + b.w + PAD > W) { x = PAD; y += shelf + PAD; shelf = 0; }
			gl.ax = x, gl.ay = y, gl.w = b.w, gl.h = b.h;
			gl.xoff = x0, gl.yoff = y0;
			x += b.w + PAD;
			shelf = std::max (shelf, b.h);
		}
		// glyphs missing in the font (control characters) have no width
		for (int c = 0; c < 32; c++) atlas->glyph[c] = GlyphAtlas::Glyph();

		int H = y + shelf + PAD;
		std::vector<uint8_t> px ((size_t)W * H, 0);
		for (int c = 32; c < 256; c++) {
			const Bitmap &b = bmp[c];
			const GlyphAtlas::Glyph &gl = atlas->glyph[c];
			for (int r = 0; r < b.h; r++)
				memcpy (&px[(size_t)(gl.ay + r) * W + gl.ax], &b.px[(size_t)r * b.w], b.w);
		}
		atlas->tex = texmgr->CreateCoverage (W, H, px.data());
		atlases[key] = atlas;
	}

	VkFont *font = new VkFont (atlas, height, prop, face, style, orientation, spacing);
	font->synthItalic = italic && !loc.italic;
	return font;
}
