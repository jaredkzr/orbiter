// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Keyboard shortcuts (Alt+F1): a sheet over the view listing the keys of
// the current key map, in groups, with a search field.
// =======================================================================

#ifndef __DLGKEYHELP_H
#define __DLGKEYHELP_H

#include "OrbiterAPI.h"

class DlgKeyHelp : public ImGuiDialog {
public:
	DlgKeyHelp ();
	/// Open the sheet, or close it
	void Toggle ();
	/// Close the sheet (if open)
	void Close ();
	void Display () override;
	void OnDraw () override {}

private:
	int openFrame = 0;
	char filter[64] = "";
};

#endif // !__DLGKEYHELP_H
