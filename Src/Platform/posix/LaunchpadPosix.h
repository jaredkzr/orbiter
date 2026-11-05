// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Launchpad for non-Windows platforms: an ImGui user interface drawn in
// the graphics client's window, over a live scene (a backdrop session)
// or between simulation sessions: scenario browser and preview,
// settings, add-on modules.
// =======================================================================

#ifndef __LAUNCHPADPOSIX_H
#define __LAUNCHPADPOSIX_H

#include "OrbiterAPI.h"
#include "UiTheme.h"
#include <memory>
#include <string>
#include <vector>

class Orbiter;
class SettingsUI;

class PosixLaunchpad {
public:
	enum Action { NONE, LAUNCH, EXIT };

	explicit PosixLaunchpad (Orbiter *orbiter);
	~PosixLaunchpad ();

	/// Draw the user interface for one frame (between ImGui::NewFrame and
	/// rendering). Returns the action selected by the user. backdrop: a
	/// scene is rendered behind it.
	Action Draw (bool backdrop = false);

	/// The selected scenario (path relative to the scenario directory, without extension)
	const std::string &Scenario () const { return selScenario; }

	/// Release graphics resources (before a session starts)
	void ReleaseResources ();

	/// Rescan the scenario directory (after a session, which may have saved scenarios)
	void Refresh ();

	/// Draw the loading screen of the selected scenario instead
	void SetLoading (bool on) { loading = on; }

	/// Description of a scenario file or directory as text blocks (headings,
	/// paragraphs, list items), from its DESC, HYPERDESC or URLDESC section
	/// or a directory's Description.txt
	struct TextBlock {
		enum Type { HEADING, PARAGRAPH, ITEM } type;
		std::string text;
	};
	static std::vector<TextBlock> ReadDescription (const std::string &path, bool directory);
	/// Text blocks of an HTML fragment (tags reduced to headings, paragraphs and list items)
	static std::vector<TextBlock> ParseHtml (const std::string &html);

	/// What a scenario file says about its start: the focus vessel, where
	/// it is, and the date
	struct ScenarioInfo {
		std::string vessel;    // focus vessel's name
		std::string cls;       // its class
		std::string body;      // the body it orbits or is landed on
		std::string situation; // "In orbit", "Landed at Brighton Beach", ...
		std::string date;      // "14 Mar 2001, 12:42 UTC"
		std::string system;    // planetary system
	};
	static ScenarioInfo ReadInfo (const std::string &path);

private:
	enum Page { SCENARIOS, SETTINGS, ADDONS, ABOUT, NPAGE };

	struct Node {
		std::string name;  // display name
		std::string path;  // relative to the scenario directory, without extension
		bool directory = false;
		int count = 0;     // scenarios in a directory and below
		std::vector<Node> children;
	};
	void ScanDir (Node &node, const std::string &dir, const std::string &rel);
	bool DrawTree (const Node &node, const std::string &filter);
	bool Matches (const Node &node, const std::string &filter) const;
	void Select (const Node &node);
	void SelectPath (const std::string &path);

	void DrawHeader (float x, float y, float w);
	void DrawScenarios (Action &action, float x, float y, float w, float h);
	void DrawBrowser (Action &action);
	void DrawPreview (Action &action);
	void DrawImage (float w);
	void DrawDescription ();
	void DrawSettings (float x, float y, float w, float h);
	void DrawAddons (float x, float y, float w, float h);
	void DrawAbout (float x, float y, float w, float h);
	void DrawLoading ();
	void DrawBackground (bool backdrop);


	void LoadImage ();
	void ScanModules ();
	void LoadState ();
	void SaveState () const;
	void AddRecent (const std::string &path);

	Orbiter *orbiter;
	Node root;
	bool scanned = false;
	Page page = SCENARIOS;
	std::string selScenario;   // selected scenario
	std::string selPath;       // selected tree entry (scenario or directory)
	bool selDirectory = false;
	int selCount = 0;          // scenarios in the selected directory
	bool scrollToSel = true;
	std::vector<TextBlock> desc;
	ScenarioInfo info;
	char filter[128] = "";
	ui::Fill previewFill; // (the preview above its actions)
	int settingsPage = 0; // (kept between runs)
	std::unique_ptr<class SettingsUI> settings; // (the settings screen, shared with the flight's Settings dialog)
	std::vector<std::string> recent; // recently launched scenarios, the latest first
	bool loading = false;
	bool exitRequest = false; // (the power button or Ctrl+Q)

	// the selected scenario's image (Images/Scenarios/<path>.jpg|png; the
	// current state's Images/CurrentState.jpg)
	SURFHANDLE image = NULL;
	std::string imagePath;     // loaded image ("": none)
	std::string imageFor;      // the entry it was looked up for
	DWORD imageW = 0, imageH = 0;

	// test automation (environment): ORBITER_LAUNCHPAD_TAB selects the
	// initial page (0: scenarios, 1: settings, 2: add-ons, 3: about),
	// ORBITER_LAUNCHPAD_SETTINGS the settings page (SettingsUI::Page);
	// ORBITER_LAUNCHPAD_AUTO=<scenario> launches it after a few frames,
	// ORBITER_LAUNCHPAD_RUNS times (default 1), then exits
	std::string autoScenario;
	int autoRuns = 0;
	int frame = 0; // frames since the launchpad was shown

	struct ModuleRec {
		std::string name;
		bool active = false;
		bool locked = false; // requested on the command line
	};
	std::vector<ModuleRec> modules;
	bool modulesScanned = false;
};

#endif // !__LAUNCHPADPOSIX_H
