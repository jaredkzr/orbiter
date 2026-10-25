// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Launchpad for non-Windows platforms: an ImGui user interface drawn in
// the graphics client's window between simulation sessions (scenario
// selection, options, video and module settings).
// =======================================================================

#ifndef __LAUNCHPADPOSIX_H
#define __LAUNCHPADPOSIX_H

#include "OrbiterAPI.h"
#include <memory>
#include <string>
#include <vector>

class Orbiter;

class PosixLaunchpad {
public:
	enum Action { NONE, LAUNCH, EXIT };

	explicit PosixLaunchpad (Orbiter *orbiter);
	~PosixLaunchpad ();

	/// Draw the user interface for one frame (between ImGui::NewFrame and
	/// rendering). Returns the action selected by the user.
	Action Draw ();

	/// The selected scenario (path relative to the scenario directory, without extension)
	const std::string &Scenario () const { return selScenario; }

	/// Release graphics resources (before a session starts)
	void ReleaseResources ();

	/// Rescan the scenario directory (after a session, which may have saved scenarios)
	void Refresh ();

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

private:
	struct Node {
		std::string name;  // display name
		std::string path;  // relative to the scenario directory, without extension
		bool directory = false;
		std::vector<Node> children;
	};
	void ScanDir (Node &node, const std::string &dir, const std::string &rel);
	bool DrawTree (const Node &node, const std::string &filter);
	bool Matches (const Node &node, const std::string &filter) const;
	void Select (const Node &node);

	void DrawScenarioTab (Action &action);
	void DrawOptionsTab ();
	void DrawVideoTab ();
	void DrawModulesTab ();
	void DrawAboutTab ();
	void DrawDescription ();

	void ScanModules ();
	void LoadState ();
	void SaveState () const;

	Orbiter *orbiter;
	Node root;
	bool scanned = false;
	std::string selScenario;   // selected scenario
	std::string selPath;       // selected tree entry (scenario or directory)
	bool selDirectory = false;
	bool scrollToSel = true;
	std::vector<TextBlock> desc;
	char filter[128] = "";
	int optionsPage = 0;

	// scenario image (only the "current state" scenario has one)
	SURFHANDLE image = NULL;
	std::string imagePath;
	DWORD imageW = 0, imageH = 0;

	// test automation (environment): ORBITER_LAUNCHPAD_TAB selects the
	// initial tab (0-4); ORBITER_LAUNCHPAD_AUTO=<scenario> launches it after
	// a few frames, ORBITER_LAUNCHPAD_RUNS times (default 1), then exits
	int initTab = -1;
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
