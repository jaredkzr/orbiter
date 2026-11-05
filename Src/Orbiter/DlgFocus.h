// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Focus vessel selection dialog
// ======================================================================

#ifndef __DLGFOCUS_H
#define __DLGFOCUS_H

#include "OrbiterAPI.h"
#include "UiTheme.h"
#include <string>

class Vessel;

class DlgFocus : public ImGuiDialog {
public:
	DlgFocus();
	void OnDraw() override;

	/// How the list is arranged
	enum Mode { ALL, NEARBY, BY_BODY, BY_CLASS };

private:
	void DrawList();
	void SetFocus(const std::string &name);

	std::string m_SelectedShip; // the vessel chosen in the list
	int m_Mode = ALL;
	float m_Range = 100.0f;     // NEARBY: the range [km] from the camera
	char m_Filter[64] = "";
	ui::Fill m_Fill;
};

#endif // !__DLGFOCUS_H
