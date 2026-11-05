// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// class MenuInfoBar
// Rendering and user interface for main menu and info bars

#ifndef __MENUINFOBAR_H
#define __MENUINFOBAR_H

#define STRICT 1

#include <memory>
#include "OrbiterAPI.h"

// =======================================================================
// class MenuInfoBar
class DynamicMenuBar;
class InfoFrameRate;
class InfoTime;
class InfoTarget;
class CameraModeLabel;
class EventBanners;
class AnnotationPanels;

// Flight events (damage, destruction, impacts, docking refused,
// relativistic flight): a banner under the menu bar, a few at a time,
// newest first; repeats are counted
namespace flightevent {
	enum Level { INFO, SUCCESS, WARNING, DANGER };
	/// key, hint: an optional key cap and what it does (e.g. "F3", "Switch
	/// to another vessel"); duration [s] (0: by the level)
	void Post (Level level, const char *title, const char *detail = nullptr,
		const char *key = nullptr, const char *hint = nullptr, double duration = 0.0);
	void Clear ();
}

class MenuInfoBar {
	std::unique_ptr<DynamicMenuBar> dynMenu;
	std::unique_ptr<InfoFrameRate>  infoFps;
	std::unique_ptr<InfoTime>       infoTime;
	std::unique_ptr<InfoTarget>     infoTgt;
	std::unique_ptr<CameraModeLabel> camLabel;
	std::unique_ptr<EventBanners>   banners;
	std::unique_ptr<AnnotationPanels> notes;
public:
	struct MenuPreference {
		std::string label;
		bool enabled;
	};

	MenuInfoBar ();
	~MenuInfoBar ();
	void RegisterMenuItem(const char *label, const char *imagepath, int id, CustomFunc func, void *context);
	void UnregisterMenuItem(int);
	std::vector<MenuPreference> &GetPreferences();
	void SyncPreferences();
};

#endif // !__MENUINFOBAR_H