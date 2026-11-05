// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// In-session settings dialog
// ======================================================================

#include "DlgOptions.h"
#include "Orbiter.h"
#include "IconsLucide.h"
#include "imgui.h"
#include <cstring>

extern Orbiter* g_pOrbiter;

DlgOptions::DlgOptions(): ImGuiDialog(ICON_LC_SETTINGS "  Settings", {900,620})
{
	m_Settings = std::make_unique<SettingsUI>(g_pOrbiter, SettingsUI::FLIGHT);
}

void DlgOptions::SwitchPage(const char *page)
{
	for (int p = 0; p < SettingsUI::NPAGE; p++)
		if (!strcmp(page, SettingsUI::PageName((SettingsUI::Page)p))) m_Settings->SelectPage((SettingsUI::Page)p);
}

void DlgOptions::OnDraw()
{
	SettingsUI::Page p = m_Settings->CurrentPage();
	if (p != m_HelpPage) { // (the help button's topic)
		static const char *topics[] = { "/tab_param.htm", "/tab_param.htm", "/tab_param.htm", "/tab_param.htm",
			"/tab_param.htm", "/tab_joystick.htm", "/vishelper.htm" };
		SetHelp("html/orbiter.chm", topics[p]);
		m_HelpPage = p;
	}
	ImVec2 avail = ImGui::GetContentRegionAvail();
	m_Settings->Draw(avail.x, avail.y, false);
}
