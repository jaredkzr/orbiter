// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Named screen positions of interactive elements (map view handles,
// replay controls, ...), published while they are drawn so that input
// tests can aim at them (posix: oapiInputUiAnchor). Nothing is stored
// until the first lookup.
// =======================================================================

#ifndef __UIANCHOR_H
#define __UIANCHOR_H

#include <string>

namespace uianchor {

	/// Publish a position [pixels] for this frame
	void Set (const char *name, float x, float y);
	/// Publish the centre of the last ImGui item
	void SetItem (const char *name);
	/// Start of an ImGui frame: positions not published again are dropped
	void NewFrame ();
	/// Position published in this or the previous frame
	bool Get (const char *name, float &x, float &y);
	/// Are positions stored (has anyone asked for them)?
	bool Enabled ();
	/// The positions of this and the last frame, "name x y" per line, sorted
	/// (names starting with prefix); enables storing them
	std::string List (const std::string &prefix);

	/// The ImGui widgets drawn in the last frame and visible (through Dear
	/// ImGui's test engine hooks, ImGuiItemHooks.h): one per line,
	/// "x y w h<TAB>window<TAB>label<TAB>state" (centre and size [pixels];
	/// state: "checked", "unchecked", "input", "open", "closed" or ""),
	/// those whose label or window contains filter (case-insensitive).
	/// Labels lose ImGui's "##id" and icon glyphs. Get resolves "ui:<label>",
	/// "ui:<window>/<label>" and "ui:<label>[n]" (the n-th, from the top) to
	/// a widget's centre.
	std::string Widgets (const std::string &filter);

}

#endif // !__UIANCHOR_H
