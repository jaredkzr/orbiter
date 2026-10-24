// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Font rasterisation into glyph atlas textures (stb_truetype), and font
// file lookup (fontconfig if available, bundled fonts otherwise)
// =======================================================================

#ifndef __FONTS_H
#define __FONTS_H

#include "OrbiterAPI.h"
#include "DrawAPI.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

class VkTexture;
class TextureManager;
struct FontFile;

/// Rasterised glyphs of one typeface at one size
class GlyphAtlas {
public:
	struct Glyph {
		int ax = 0, ay = 0, w = 0, h = 0; ///< position and size in the atlas [texels]
		int xoff = 0, yoff = 0;           ///< bitmap offset from the pen position on the baseline
		int advance = 0;                  ///< pen advance [pixels]
	};
	~GlyphAtlas ();

	VkTexture *tex = nullptr;
	Glyph glyph[256];
	int ascent = 0, descent = 0;  ///< [pixels], both positive
	int avgWidth = 0;             ///< average character width [pixels]
	int CellHeight () const { return ascent + descent; }
	int TextWidth (const char *str, int len, float spacing = 0.0f) const;

	/// Glyph index of the next character of a string, advancing p. Strings are
	/// decoded as UTF-8 where valid, and as Windows-1252 bytes otherwise.
	static int NextChar (const char *&p, const char *end);

	TextureManager *texmgr = nullptr;
};

class VkFont : public oapi::Font {
public:
	VkFont (std::shared_ptr<GlyphAtlas> atlas, int height, bool prop, const char *face, FontStyle style, int orientation, float spacing);
	std::shared_ptr<GlyphAtlas> atlas;
	FontStyle style;
	float angle;    ///< text orientation [rad], counter-clockwise
	float spacing;  ///< extra character spacing [pixels]
	bool synthItalic;
};

class FontManager {
public:
	explicit FontManager (TextureManager *tm);
	~FontManager ();

	/// Create a font with GDI-style parameters: height < 0 is the character
	/// (em) height, height > 0 the cell height. width: average character width
	/// (0: default). weight: 100-900 (400 normal, 700 bold).
	VkFont *Create (int height, bool prop, const char *face, int width, int weight,
		FontStyle style, int orientation, float spacing);

private:
	struct Located { std::string path; int index = 0; bool bold = false; bool italic = false; };
	Located Locate (const char *face, bool prop, int weight, bool italic);
	FontFile *LoadFile (const std::string &path, int index);

	TextureManager *texmgr;
	std::map<std::string, std::unique_ptr<FontFile>> files;
	std::map<std::string, std::weak_ptr<GlyphAtlas>> atlases;
};

#endif // !__FONTS_H
