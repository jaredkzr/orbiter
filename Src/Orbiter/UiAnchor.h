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

}

#endif // !__UIANCHOR_H
