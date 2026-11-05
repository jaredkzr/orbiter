// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// In-session settings dialog: the settings screen (SettingsUI), with the
// same pages as the launchpad's, and the visual helpers
// ======================================================================

#ifndef __DLGOPTIONS_H
#define __DLGOPTIONS_H

#include "OrbiterAPI.h"
#include "SettingsUI.h"
#include <memory>

class DlgOptions: public ImGuiDialog {
public:
	DlgOptions();
	void OnDraw();
	/// Show a page by its name (e.g. "Visual helpers")
	void SwitchPage(const char *page);

private:
	std::unique_ptr<SettingsUI> m_Settings;
	SettingsUI::Page m_HelpPage = SettingsUI::NPAGE; // (the page the help topic was set for)
};

#endif // !__DLGOPTIONS_H
