// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// MFD windows: MFDs in ImGui windows inside the render window, any number
// of them, resizable (the MFD is recreated at the new size once the size
// settles), with their buttons and pointer input on the display. The open
// windows and their modes are kept between sessions (MFDWindows.cfg; the
// window positions are kept by ImGui).
// =======================================================================

#ifndef __MFDWINDOW_H
#define __MFDWINDOW_H

#include "OrbiterAPI.h"
#include <memory>
#include <vector>

class DialogManager;
class PopoutMFD;

class DlgMfdWindow: public ImGuiDialog {
	friend class PopoutMFD;
public:
	explicit DlgMfdWindow (int slot);
	~DlgMfdWindow ();
	void Display () override;
	void OnDraw () override;
	void OnClose () override;

	int Slot () const { return slot; }
	/// Open the MFD in a mode (the window becomes active)
	bool Open (int mode);
	/// Current mode (MFD_NONE if off)
	int Mode () const;
	ExternMFD *MFD () const;

private:
	void Resize (int px);
	void Pointer (int event, float x, float y, int button = 0, float wheel = 0.0f);
	void EditKeys ();

	int slot;
	PopoutMFD *ext = nullptr; // (owned by the pane's list of external MFDs)
	int px = 0;               // display size [pixels]
	int wantPx = 0;           // size the window asks for
	double wantSince = 0.0;   // (system time)
	int lastMode = MFD_ORBIT;
	bool ptrDown[2] = { false, false };
	bool hovered = false;
	float lastX = -1.0f, lastY = -1.0f;
	int heldButton = -1;
};

class MfdWindows {
public:
	explicit MfdWindows (DialogManager *dm);
	~MfdWindows ();

	/// Open an MFD window (a closed one again, or a new one)
	DlgMfdWindow *Open (int mode = MFD_ORBIT);
	DlgMfdWindow *Window (int slot) const;
	int Count () const { return (int)wins.size(); }

	/// Open windows and their modes, between sessions
	void Save (const char *path) const;
	void Restore (const char *path);
	/// Close all (before the pane goes)
	void CloseAll ();

private:
	DialogManager *dm;
	std::vector<std::unique_ptr<DlgMfdWindow>> wins;
};

#endif // !__MFDWINDOW_H
