// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "LaunchpadPosix.h"
#include "Orbiter.h"
#include "Config.h"
#include "Input.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

	const char *STATE_FILE = "Launchpad.cfg"; // last selected scenario etc.

	// directory names from the configuration use Windows separators
	std::string NativePath (std::string p)
	{
		std::replace (p.begin(), p.end(), '\\', '/');
		return p;
	}

	std::string Lower (std::string s)
	{
		for (auto &c : s) c = (char)tolower ((unsigned char)c);
		return s;
	}

	/// The lines between BEGIN_<tag> and END_<tag> (empty if not found)
	bool ReadSection (std::istream &is, const char *tag, std::string &out)
	{
		std::string begin = std::string("BEGIN_") + tag, end = std::string("END_") + tag, line;
		bool in = false, found = false;
		while (std::getline (is, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!in) {
				if (line.compare (0, begin.size(), begin) == 0 &&
					(line.size() == begin.size() || isspace ((unsigned char)line[begin.size()]))) {
					in = found = true;
					std::string rest = line.substr (begin.size());
					if (rest.find_first_not_of (" \t") != std::string::npos) out += rest + '\n';
				}
			} else if (line.compare (0, end.size(), end) == 0) {
				break;
			} else if (line.compare (0, 17, "BEGIN_ENVIRONMENT") == 0) {
				break; // (unterminated description)
			} else {
				out += line + '\n';
			}
		}
		return found;
	}

	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n");
		if (a == std::string::npos) return "";
		size_t b = s.find_last_not_of (" \t\r\n");
		return s.substr (a, b - a + 1);
	}

	/// Collapse white space (HTML text)
	std::string Collapse (const std::string &s)
	{
		std::string r;
		bool space = false;
		for (char c : s) {
			if (isspace ((unsigned char)c)) space = true;
			else {
				if (space && !r.empty()) r += ' ';
				space = false;
				r += c;
			}
		}
		return r;
	}

	std::string DecodeEntities (const std::string &s)
	{
		static const struct { const char *name; const char *text; } ent[] = {
			{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
			{"nbsp", " "}, {"deg", "\xC2\xB0"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
			{"times", "\xC3\x97"}, {"middot", "\xC2\xB7"}, {"copy", "\xC2\xA9"}, {"micro", "\xC2\xB5"},
		};
		std::string r;
		for (size_t i = 0; i < s.size(); i++) {
			if (s[i] == '&') {
				size_t e = s.find (';', i);
				if (e != std::string::npos && e - i < 10) {
					std::string name = s.substr (i + 1, e - i - 1);
					std::string text;
					if (!name.empty() && name[0] == '#') {
						unsigned long cp = (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) ?
							strtoul (name.c_str() + 2, nullptr, 16) : strtoul (name.c_str() + 1, nullptr, 10);
						// UTF-8
						if (cp < 0x80) text = (char)cp;
						else if (cp < 0x800) { text += (char)(0xC0 | (cp >> 6)); text += (char)(0x80 | (cp & 0x3F)); }
						else { text += (char)(0xE0 | (cp >> 12)); text += (char)(0x80 | ((cp >> 6) & 0x3F)); text += (char)(0x80 | (cp & 0x3F)); }
					} else {
						for (auto &x : ent) if (name == x.name) text = x.text;
					}
					if (!text.empty()) {
						r += text;
						i = e;
						continue;
					}
				}
			}
			r += s[i];
		}
		return r;
	}

	/// Text blocks of plain text: paragraphs separated by blank lines
	std::vector<PosixLaunchpad::TextBlock> ParseText (const std::string &txt)
	{
		std::vector<PosixLaunchpad::TextBlock> blocks;
		std::istringstream is (txt);
		std::string line, para;
		auto flush = [&]() {
			std::string t = Trim (para);
			if (!t.empty()) blocks.push_back ({PosixLaunchpad::TextBlock::PARAGRAPH, t});
			para.clear();
		};
		while (std::getline (is, line)) {
			if (Trim (line).empty()) flush ();
			else {
				if (!para.empty()) para += ' ';
				para += Trim (line);
			}
		}
		flush ();
		return blocks;
	}

} // namespace

// =======================================================================

PosixLaunchpad::PosixLaunchpad (Orbiter *orbiter): orbiter(orbiter)
{
	LoadState ();
	if (const char *t = getenv ("ORBITER_LAUNCHPAD_TAB")) initTab = atoi (t);
	if (const char *a = getenv ("ORBITER_LAUNCHPAD_AUTO")) {
		autoScenario = a;
		const char *r = getenv ("ORBITER_LAUNCHPAD_RUNS");
		autoRuns = r ? atoi (r) : 1;
		Node n;
		n.path = autoScenario;
		Select (n);
		selScenario = autoScenario;
		scrollToSel = true;
	}
}

PosixLaunchpad::~PosixLaunchpad ()
{
}

void PosixLaunchpad::ReleaseResources ()
{
	if (image) {
		if (auto *gc = orbiter->GetGraphicsClient()) gc->clbkReleaseSurface (image);
		image = NULL;
	}
	imagePath.clear();
}

void PosixLaunchpad::Refresh ()
{
	frame = 0;
	scanned = false;
	modulesScanned = false;
	if (!selPath.empty()) {
		// the description may have changed (e.g. the current state was saved)
		std::string base = NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir) + selPath;
		desc = ReadDescription (selDirectory ? base : base + ".scn", selDirectory);
	}
	ReleaseResources ();
}

void PosixLaunchpad::LoadState ()
{
	std::ifstream ifs (NativePath (orbiter->Cfg()->CfgDirPrm.ConfigDir) + STATE_FILE);
	std::string line;
	while (std::getline (ifs, line)) {
		if (line.compare (0, 9, "Scenario=") == 0) selScenario = selPath = Trim (line.substr (9));
		else if (line.compare (0, 8, "Options=") == 0) optionsPage = atoi (line.c_str() + 8);
	}
	if (selScenario.empty()) selScenario = selPath = "(Current state)";
	std::string base = NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir) + selPath;
	desc = ReadDescription (base + ".scn", false);
}

void PosixLaunchpad::SaveState () const
{
	std::ofstream ofs (NativePath (orbiter->Cfg()->CfgDirPrm.ConfigDir) + STATE_FILE);
	ofs << "Scenario=" << selScenario << '\n';
	ofs << "Options=" << optionsPage << '\n';
}

// =======================================================================
// Scenario descriptions

std::vector<PosixLaunchpad::TextBlock> PosixLaunchpad::ParseHtml (const std::string &html)
{
	std::vector<TextBlock> blocks;
	std::string text;
	TextBlock::Type type = TextBlock::PARAGRAPH;
	auto flush = [&]() {
		std::string t = Trim (Collapse (DecodeEntities (text)));
		if (!t.empty()) blocks.push_back ({type, t});
		text.clear();
		type = TextBlock::PARAGRAPH;
	};
	for (size_t i = 0; i < html.size(); i++) {
		if (html[i] != '<') {
			text += html[i];
			continue;
		}
		size_t e = html.find ('>', i);
		if (e == std::string::npos) break;
		std::string tag = Lower (html.substr (i + 1, e - i - 1));
		bool close = !tag.empty() && tag[0] == '/';
		if (close) tag.erase (0, 1);
		std::string name = tag.substr (0, tag.find_first_of (" \t\r\n/"));
		i = e;
		if (name == "style" || name == "script" || name == "head" || name == "title") {
			if (!close) { // skip the element's content
				size_t c = Lower (html).find ("</" + name, e);
				if (c == std::string::npos) break;
				i = html.find ('>', c);
				if (i == std::string::npos) break;
			}
		} else if (name == "h1" || name == "h2" || name == "h3" || name == "h4") {
			flush ();
			if (!close) type = TextBlock::HEADING;
		} else if (name == "li") {
			flush ();
			if (!close) type = TextBlock::ITEM;
		} else if (name == "p" || name == "div" || name == "ul" || name == "ol" || name == "table" ||
			name == "tr" || name == "dt" || name == "dd" || name == "blockquote") {
			flush ();
		} else if (name == "br") {
			if (type == TextBlock::PARAGRAPH) flush ();
			else text += ' ';
		} else if (name == "td" || name == "th") {
			text += ' ';
		}
	}
	flush ();
	return blocks;
}

std::vector<PosixLaunchpad::TextBlock> PosixLaunchpad::ReadDescription (const std::string &path, bool directory)
{
	if (directory) {
		std::ifstream ifs (path + "/Description.txt");
		if (!ifs) return {};
		std::stringstream ss;
		ss << ifs.rdbuf();
		std::string txt = ss.str();
		// (some descriptions are HTML)
		return txt.find ('<') != std::string::npos ? ParseHtml (txt) : ParseText (txt);
	}

	std::string txt;
	{
		std::ifstream ifs (path);
		if (ifs && ReadSection (ifs, "HYPERDESC", txt)) return ParseHtml (txt);
	}
	{
		std::ifstream ifs (path);
		txt.clear();
		if (ifs && ReadSection (ifs, "DESC", txt)) return ParseText (txt);
	}
	{
		std::ifstream ifs (path);
		txt.clear();
		if (ifs && ReadSection (ifs, "URLDESC", txt)) {
			// Html/Scenarios/<page>.htm (compiled help topics are not supported)
			std::string ref = Trim (txt);
			std::string page = ref.substr (0, ref.find (','));
			std::ifstream hs (NativePath ("Html/Scenarios/" + Trim (page) + ".htm"));
			if (hs) {
				std::stringstream ss;
				ss << hs.rdbuf();
				return ParseHtml (ss.str());
			}
			if (page.compare (0, 12, "CurrentState") == 0) // (saved on exit; the page may not be installed)
				return { {TextBlock::HEADING, "Current state"},
				         {TextBlock::PARAGRAPH, "Launch this scenario to return to your last simulation state."} };
		}
	}
	return {};
}

// =======================================================================
// Scenario tree

void PosixLaunchpad::ScanDir (Node &node, const std::string &dir, const std::string &rel)
{
	std::error_code ec;
	std::vector<Node> dirs, files;
	for (const auto &e : fs::directory_iterator (dir, ec)) {
		std::string name = e.path().filename().string();
		if (e.is_directory (ec)) {
			Node n;
			n.name = name;
			n.path = rel + name;
			n.directory = true;
			ScanDir (n, e.path().string(), rel + name + "/");
			dirs.push_back (std::move (n));
		} else if (e.path().extension() == ".scn") {
			Node n;
			n.name = e.path().stem().string();
			n.path = rel + n.name;
			files.push_back (std::move (n));
		}
	}
	auto byName = [](const Node &a, const Node &b) { return Lower (a.name) < Lower (b.name); };
	std::sort (dirs.begin(), dirs.end(), byName);
	std::sort (files.begin(), files.end(), byName);
	node.children = std::move (dirs);
	for (auto &f : files) node.children.push_back (std::move (f));
}

bool PosixLaunchpad::Matches (const Node &node, const std::string &f) const
{
	if (f.empty() || Lower (node.name).find (f) != std::string::npos) return true;
	for (const auto &c : node.children)
		if (Matches (c, f)) return true;
	return false;
}

void PosixLaunchpad::Select (const Node &node)
{
	if (selPath == node.path) return;
	selPath = node.path;
	selDirectory = node.directory;
	if (!node.directory) selScenario = node.path;
	std::string base = NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir) + node.path;
	desc = ReadDescription (node.directory ? base : base + ".scn", node.directory);
}

// Returns true if a scenario was double-clicked
bool PosixLaunchpad::DrawTree (const Node &node, const std::string &f)
{
	bool launch = false;
	for (const auto &c : node.children) {
		if (!Matches (c, f)) continue;
		bool sel = (c.path == selPath);
		if (c.directory) {
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
				ImGuiTreeNodeFlags_SpanAvailWidth | (sel ? ImGuiTreeNodeFlags_Selected : 0);
			if (!f.empty()) ImGui::SetNextItemOpen (true);
			else if (scrollToSel && selPath.compare (0, c.path.size() + 1, c.path + "/") == 0)
				ImGui::SetNextItemOpen (true); // contains the selection
			bool open = ImGui::TreeNodeEx (c.path.c_str(), flags, "%s", c.name.c_str());
			if (ImGui::IsItemClicked () && !ImGui::IsItemToggledOpen ()) Select (c);
			if (open) {
				launch |= DrawTree (c, f);
				ImGui::TreePop ();
			}
		} else {
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
				ImGuiTreeNodeFlags_SpanAvailWidth | (sel ? ImGuiTreeNodeFlags_Selected : 0);
			ImGui::TreeNodeEx (c.path.c_str(), flags, "%s", c.name.c_str());
			if (ImGui::IsItemClicked ()) Select (c);
			if (ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left)) launch = true;
			if (sel && scrollToSel) {
				ImGui::SetScrollHereY (0.3f);
				scrollToSel = false;
			}
		}
	}
	return launch;
}

// =======================================================================
// User interface

PosixLaunchpad::Action PosixLaunchpad::Draw ()
{
	Action action = NONE;
	const ImGuiViewport *vp = ImGui::GetMainViewport();

	// background: a dark gradient (colours are linear: the render target is sRGB)
	ImDrawList *bg = ImGui::GetBackgroundDrawList();
	bg->AddRectFilledMultiColor (vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
		IM_COL32(0, 0, 2, 255), IM_COL32(0, 0, 2, 255), IM_COL32(1, 3, 8, 255), IM_COL32(1, 3, 8, 255));

	float margin = std::max (16.0f, vp->Size.x * 0.04f);
	ImGui::SetNextWindowPos (ImVec2(vp->Pos.x + margin, vp->Pos.y + margin));
	ImGui::SetNextWindowSize (ImVec2(vp->Size.x - 2 * margin, vp->Size.y - 2 * margin));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
	ImGui::Begin ("##launchpad", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar ();

	// title
	ImGui::PushFont (nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
	ImGui::TextUnformatted ("ORBITER");
	ImGui::PopFont ();
	ImGui::SameLine ();
	ImGui::SetCursorPosY (ImGui::GetCursorPosY() + ImGui::GetStyle().FontSizeBase * 0.7f);
	ImGui::TextDisabled ("Space Flight Simulator  \xC2\xB7  build %06d", orbiter->GetVersion());
	ImGui::Spacing ();

	float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
	if (ImGui::BeginTabBar ("##tabs")) {
		auto flag = [&](int tab) { return (frame == 0 && initTab == tab) ? ImGuiTabItemFlags_SetSelected : 0; };
		if (ImGui::BeginTabItem ("Scenarios", nullptr, flag (0))) { DrawScenarioTab (action); ImGui::EndTabItem (); }
		if (ImGui::BeginTabItem ("Options", nullptr, flag (1)))   { DrawOptionsTab ();        ImGui::EndTabItem (); }
		if (ImGui::BeginTabItem ("Video", nullptr, flag (2)))     { DrawVideoTab ();          ImGui::EndTabItem (); }
		if (ImGui::BeginTabItem ("Modules", nullptr, flag (3)))   { DrawModulesTab ();        ImGui::EndTabItem (); }
		if (ImGui::BeginTabItem ("About", nullptr, flag (4)))     { DrawAboutTab ();          ImGui::EndTabItem (); }
		ImGui::EndTabBar ();
	}

	// footer: launch and exit buttons
	ImGui::SetCursorPosY (ImGui::GetWindowHeight() - footer);
	ImGui::Separator ();
	ImGui::Checkbox ("Start paused", &orbiter->Cfg()->CfgLogicPrm.bStartPaused);
	ImGui::SameLine ();
	ImGui::TextDisabled ("  %s", selScenario.c_str());
	float bw = ImGui::GetStyle().FontSizeBase * 7.0f;
	ImGui::SameLine (ImGui::GetWindowWidth() - 2 * bw - 40);
	if (ImGui::Button ("Exit", ImVec2(bw, 0))) action = EXIT;
	ImGui::SameLine ();
	ImGui::PushStyleColor (ImGuiCol_Button, ImVec4(0.10f, 0.45f, 0.20f, 1.0f));
	ImGui::PushStyleColor (ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.60f, 0.28f, 1.0f));
	ImGui::BeginDisabled (selScenario.empty());
	if (ImGui::Button ("Launch", ImVec2(bw, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter, false)) action = LAUNCH;
	ImGui::EndDisabled ();
	ImGui::PopStyleColor (2);

	ImGui::End ();

	frame++;
	if (!autoScenario.empty() && frame == 10) {
		if (autoRuns > 0) {
			autoRuns--;
			selScenario = autoScenario;
			action = LAUNCH;
		}
	} else if (!autoScenario.empty() && frame == 40)
		action = EXIT;

	if (action != NONE) SaveState ();
	if (action == LAUNCH && selScenario.empty()) action = NONE;
	return action;
}

void PosixLaunchpad::DrawScenarioTab (Action &action)
{
	if (!scanned) {
		root = Node();
		std::string dir = NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir);
		if (!dir.empty() && dir.back() == '/') dir.pop_back();
		ScanDir (root, dir, "");
		// the current state first
		auto it = std::find_if (root.children.begin(), root.children.end(),
			[](const Node &n) { return !n.directory && n.name == "(Current state)"; });
		if (it != root.children.end()) std::rotate (root.children.begin(), it, it + 1);
		scanned = true;
		scrollToSel = true;
	}

	float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f + ImGui::GetStyle().ItemSpacing.y;
	float h = ImGui::GetContentRegionAvail().y - footer;
	if (ImGui::BeginTable ("##scn", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoHostExtendY, ImVec2(0, h))) {
		ImGui::TableSetupColumn ("list", ImGuiTableColumnFlags_WidthStretch, 0.4f);
		ImGui::TableSetupColumn ("desc", ImGuiTableColumnFlags_WidthStretch, 0.6f);
		ImGui::TableNextRow ();

		ImGui::TableNextColumn ();
		ImGui::SetNextItemWidth (-FLT_MIN);
		ImGui::InputTextWithHint ("##filter", "Search scenarios", filter, sizeof(filter));
		ImGui::BeginChild ("##tree", ImVec2(0, 0), ImGuiChildFlags_None);
		if (DrawTree (root, Lower (Trim (filter))) && !selDirectory) action = LAUNCH;
		ImGui::EndChild ();

		ImGui::TableNextColumn ();
		ImGui::BeginChild ("##desc", ImVec2(0, 0), ImGuiChildFlags_None);
		DrawDescription ();
		ImGui::EndChild ();
		ImGui::EndTable ();
	}
}

void PosixLaunchpad::DrawDescription ()
{
	ImGui::Indent (8.0f);
	// (heading from the entry's name, unless the description starts with one)
	if (desc.empty() || desc[0].type != TextBlock::HEADING) {
		std::string name = selPath.substr (selPath.find_last_of ('/') + 1);
		ImGui::PushFont (nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
		ImGui::TextWrapped ("%s", name.c_str());
		ImGui::PopFont ();
		ImGui::Spacing ();
	}
	for (const auto &b : desc) {
		switch (b.type) {
		case TextBlock::HEADING:
			ImGui::Spacing ();
			ImGui::PushFont (nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
			ImGui::PushStyleColor (ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.0f, 1.0f));
			ImGui::TextWrapped ("%s", b.text.c_str());
			ImGui::PopStyleColor ();
			ImGui::PopFont ();
			ImGui::Spacing ();
			break;
		case TextBlock::ITEM:
			ImGui::Bullet ();
			ImGui::TextWrapped ("%s", b.text.c_str());
			break;
		default:
			ImGui::TextWrapped ("%s", b.text.c_str());
			ImGui::Spacing ();
			break;
		}
	}

	// the image saved with the current state
	if (!selDirectory && selPath == "(Current state)") {
		const char *candidates[] = { "Images/CurrentState.png", "Images/CurrentState.jpg" };
		std::string path;
		for (auto c : candidates) if (fs::exists (c)) { path = c; break; }
		auto *gc = orbiter->GetGraphicsClient();
		if (!path.empty() && gc && path != imagePath) {
			ReleaseResources ();
			image = gc->clbkLoadSurface (path.c_str(), OAPISURFACE_TEXTURE, true);
			imagePath = path;
			imageW = imageH = 0;
			if (image) gc->clbkGetSurfaceSize (image, &imageW, &imageH);
		}
		if (image && imageW && imageH) {
			ImGui::Spacing ();
			float w = ImGui::GetContentRegionAvail().x - 8.0f;
			ImTextureID id = (ImTextureID)gc->clbkImGuiSurfaceTexture (image);
			if (id) ImGui::Image (ImTextureRef(id), ImVec2(w, w * imageH / imageW));
		}
	}
	ImGui::Unindent (8.0f);
}

void PosixLaunchpad::DrawOptionsTab ()
{
	static const char *pages[] = { "Visual", "Physics", "Controllers" };
	Config *cfg = orbiter->Cfg();
	float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f + ImGui::GetStyle().ItemSpacing.y;
	float h = ImGui::GetContentRegionAvail().y - footer;

	ImGui::BeginChild ("##pages", ImVec2(ImGui::GetStyle().FontSizeBase * 10.0f, h), ImGuiChildFlags_Borders);
	for (int i = 0; i < IM_ARRAYSIZE(pages); i++)
		if (ImGui::Selectable (pages[i], optionsPage == i)) optionsPage = i;
	ImGui::EndChild ();
	ImGui::SameLine ();
	ImGui::BeginChild ("##page", ImVec2(0, h));
	ImGui::PushItemWidth (ImGui::GetStyle().FontSizeBase * 18.0f);

	if (optionsPage == 0) {
		CFG_VISUALPRM &v = cfg->CfgVisualPrm;
		ImGui::SeparatorText ("Planets");
		ImGui::Checkbox ("Cloud layers", &v.bClouds);
		ImGui::BeginDisabled (!v.bClouds);
		ImGui::Checkbox ("Cloud shadows", &v.bCloudShadows);
		ImGui::EndDisabled ();
		ImGui::Checkbox ("Horizon haze", &v.bHaze);
		ImGui::Checkbox ("Distance fog", &v.bFog);
		ImGui::Checkbox ("Specular water reflections", &v.bWaterreflect);
		ImGui::BeginDisabled (!v.bWaterreflect);
		ImGui::Checkbox ("Specular ripples", &v.bSpecularRipple);
		ImGui::EndDisabled ();
		ImGui::Checkbox ("Night lights", &v.bNightlights);
		ImGui::BeginDisabled (!v.bNightlights);
		float lb = (float)v.LightBrightness;
		if (ImGui::SliderFloat ("Night light level", &lb, 0.0f, 1.0f, "%.2f")) v.LightBrightness = lb;
		ImGui::EndDisabled ();
		bool elev = v.ElevMode != 0;
		if (ImGui::Checkbox ("Surface elevation", &elev)) v.ElevMode = elev ? 2 : 0;
		ImGui::BeginDisabled (!elev);
		int mode = std::max (0, v.ElevMode - 1);
		const char *modes[] = { "Linear interpolation", "Cubic interpolation" };
		ImGui::SetNextItemWidth (ImGui::GetStyle().FontSizeBase * 14.0f);
		if (ImGui::Combo ("##elevmode", &mode, modes, 2) && elev) v.ElevMode = mode + 1;
		ImGui::EndDisabled ();
		int lvl = (int)v.PlanetMaxLevel;
		if (ImGui::SliderInt ("Max. resolution level", &lvl, 1, SURF_MAX_PATCHLEVEL2)) v.PlanetMaxLevel = (DWORD)lvl;
		// level of detail: +1 halves the screen size of the surface tiles
		float bias = (float)orbiter->Cfg()->CfgPRenderPrm.ResolutionBias;
		if (ImGui::SliderFloat ("Resolution bias", &bias, -2.0f, 2.0f, "%+.1f"))
			orbiter->Cfg()->CfgPRenderPrm.ResolutionBias = bias;
		if (ImGui::IsItemHovered ()) ImGui::SetTooltip ("Higher: finer surface meshes farther from the camera (slower)");
		ImGui::BeginDisabled (!elev);
		ImGui::Checkbox ("Procedural terrain detail", &v.bTerrainDetail);
		if (ImGui::IsItemHovered ()) ImGui::SetTooltip ("Relief and craters finer than the elevation data (also for ground contact)");
		ImGui::EndDisabled ();

		ImGui::SeparatorText ("Vessels and objects");
		ImGui::Checkbox ("Vessel shadows", &v.bVesselShadows);
		ImGui::Checkbox ("Object shadows", &v.bShadows);
		ImGui::Checkbox ("Specular reflections from objects", &v.bSpecular);
		ImGui::Checkbox ("Re-entry flames", &v.bReentryFlames);
		ImGui::Checkbox ("Particle streams", &v.bParticleStreams);
		ImGui::Checkbox ("Local light sources", &v.bLocalLight);
		int amb = (int)v.AmbientLevel;
		if (ImGui::SliderInt ("Ambient level", &amb, 0, 255)) cfg->SetAmbientLevel ((DWORD)amb);
	}
	else if (optionsPage == 1) {
		CFG_PHYSICSPRM &p = cfg->CfgPhysicsPrm;
		ImGui::SeparatorText ("Physics engine");
		ImGui::Checkbox ("Nonspherical gravity sources", &p.bNonsphericalGrav);
		ImGui::Checkbox ("Gravity-gradient torque", &p.bDistributedMass);
		ImGui::Checkbox ("Radiation pressure", &p.bRadiationPressure);
		ImGui::Checkbox ("Atmospheric wind effects", &p.bAtmWind);
		ImGui::SeparatorText ("Docking");
		float v = (float)p.DockSpeedLimit;
		if (ImGui::SliderFloat ("Max. closing speed [m/s]", &v, 0.05f, 5.0f, "%.2f")) p.DockSpeedLimit = v;
		float a = (float)(p.DockAngleLimit * DEG);
		if (ImGui::SliderFloat ("Max. misalignment [\xC2\xB0]", &a, 1.0f, 45.0f, "%.1f")) p.DockAngleLimit = a * RAD;
	}
	else {
		CFG_JOYSTICKPRM &j = cfg->CfgJoystickPrm;
		DIDEVICEINSTANCE *list;
		DWORD ndev;
		orbiter->GetInput()->GetJoysticks (&list, &ndev);
		ImGui::SeparatorText ("Game controller");
		const char *preview = (j.Joy_idx > 0 && j.Joy_idx <= ndev) ? list[j.Joy_idx-1].tszProductName : "<Disabled>";
		ImGui::SetNextItemWidth (ImGui::GetStyle().FontSizeBase * 20.0f);
		if (ImGui::BeginCombo ("Device", preview)) {
			DWORD idx = j.Joy_idx;
			if (ImGui::Selectable ("<Disabled>", idx == 0)) idx = 0;
			for (DWORD i = 0; i < ndev; i++) {
				ImGui::PushID ((int)i);
				if (ImGui::Selectable (list[i].tszProductName, idx == i + 1)) idx = i + 1;
				ImGui::PopID ();
			}
			ImGui::EndCombo ();
			if (idx != j.Joy_idx) {
				j.Joy_idx = idx;
				orbiter->GetInput()->OptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_DEVICE);
			}
		}
		if (!ndev) ImGui::TextDisabled ("No controllers connected.");

		const OAPI_CONTROLLERSTATE *pad = orbiter->GetInput()->Gamepad ();
		if (pad) {
			ImGui::SeparatorText ("Gamepad controls");
			if (ImGui::BeginTable ("##pad", 2, ImGuiTableFlags_SizingFixedFit)) {
				static const char *rows[][2] = {
					{"Left stick", "Pitch and bank"}, {"Shoulder buttons", "Yaw"},
					{"Triggers", "Main engine (right: increase, left: decrease)"},
					{"Right stick", "Rotate camera"}, {"A", "Kill rotation"}, {"B", "RCS mode"},
					{"X", "HUD mode"}, {"Y", "Cockpit / external view"}, {"D-pad up/down", "Time acceleration"},
					{"D-pad left/right", "Prograde / retrograde"}, {"Left stick button", "Level with horizon"},
					{"Right stick button", "Reset cockpit camera"}, {"Start", "Pause"}, {"Back", "Main menu"},
				};
				for (auto &r : rows) {
					ImGui::TableNextRow ();
					ImGui::TableNextColumn (); ImGui::TextUnformatted (r[0]);
					ImGui::TableNextColumn (); ImGui::TextDisabled ("%s", r[1]);
				}
				ImGui::EndTable ();
			}
		} else {
			ImGui::BeginDisabled (j.Joy_idx == 0);
			ImGui::SeparatorText ("Main engine control");
			const char *axes[] = { "<Keyboard only>", "Z-axis", "Slider 0", "Slider 1" };
			int ax = (int)j.ThrottleAxis;
			ImGui::SetNextItemWidth (ImGui::GetStyle().FontSizeBase * 14.0f);
			if (ImGui::Combo ("Throttle axis", &ax, axes, 4)) {
				j.ThrottleAxis = (DWORD)ax;
				orbiter->GetInput()->OptionChanged (OPTCAT_JOYSTICK, OPTITEM_JOYSTICK_PARAM);
			}
			ImGui::Checkbox ("Ignore throttle setting on launch", &j.bThrottleIgnore);
			ImGui::SeparatorText ("Calibration");
			ImGui::SliderInt ("Saturation", &j.ThrottleSaturation, 0, 10000);
			ImGui::EndDisabled ();
		}
		ImGui::BeginDisabled (j.Joy_idx == 0);
		ImGui::SliderInt ("Dead zone", &j.Deadzone, 0, 10000);
		ImGui::EndDisabled ();
	}
	ImGui::PopItemWidth ();
	ImGui::EndChild ();
}

void PosixLaunchpad::DrawVideoTab ()
{
	CFG_DEVPRM &d = orbiter->Cfg()->CfgDevPrm;
	ImGui::SeparatorText ("Render window");
	ImGui::Checkbox ("Fullscreen (desktop resolution)", &d.bFullscreen);
	ImGui::BeginDisabled (d.bFullscreen);
	static const struct { int w, h; } sizes[] = {
		{1280, 720}, {1280, 800}, {1600, 900}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {3840, 2160}
	};
	char cur[32];
	snprintf (cur, sizeof(cur), "%u x %u", d.WinW, d.WinH);
	ImGui::SetNextItemWidth (ImGui::GetStyle().FontSizeBase * 10.0f);
	if (ImGui::BeginCombo ("Window size", cur)) {
		for (auto &s : sizes) {
			char lbl[32];
			snprintf (lbl, sizeof(lbl), "%d x %d", s.w, s.h);
			if (ImGui::Selectable (lbl, (int)d.WinW == s.w && (int)d.WinH == s.h)) d.WinW = s.w, d.WinH = s.h;
		}
		ImGui::EndCombo ();
	}
	int wh[2] = { (int)d.WinW, (int)d.WinH };
	ImGui::SetNextItemWidth (ImGui::GetStyle().FontSizeBase * 10.0f);
	if (ImGui::InputInt2 ("Custom size", wh)) {
		d.WinW = (DWORD)std::clamp (wh[0], 320, 16384);
		d.WinH = (DWORD)std::clamp (wh[1], 200, 16384);
	}
	ImGui::EndDisabled ();
	ImGui::Spacing ();
	ImGui::TextDisabled ("Graphics options of the Vulkan client are set with ORBITER_VK_* environment variables (see OVP/VulkanClient).");
}

void PosixLaunchpad::ScanModules ()
{
	modules.clear();
	std::error_code ec;
	const auto &cmd = orbiter->Cfg()->CfgCmdlinePrm.LoadPlugins;
	for (const auto &e : fs::directory_iterator ("Modules/Plugin", ec)) {
		if (e.path().extension() != ".so") continue;
		ModuleRec m;
		m.name = e.path().stem().string();
		if (m.name.compare (0, 3, "lib") == 0) m.name.erase (0, 3);
		if (m.name == "VulkanClient") continue; // (graphics clients are not plugins to switch here)
		m.active = orbiter->Cfg()->IsActiveModule (m.name);
		m.locked = std::find (cmd.begin(), cmd.end(), m.name) != cmd.end();
		if (m.locked) m.active = true;
		modules.push_back (m);
	}
	std::sort (modules.begin(), modules.end(), [](const ModuleRec &a, const ModuleRec &b) { return Lower (a.name) < Lower (b.name); });
	modulesScanned = true;
}

void PosixLaunchpad::DrawModulesTab ()
{
	if (!modulesScanned) ScanModules ();
	ImGui::TextDisabled ("Optional plugin modules (Modules/Plugin). Activated modules are loaded at once and stay active in later sessions.");
	ImGui::Spacing ();
	if (modules.empty()) ImGui::TextDisabled ("No plugin modules found.");
	for (auto &m : modules) {
		ImGui::BeginDisabled (m.locked);
		bool act = m.active;
		if (ImGui::Checkbox (m.name.c_str(), &act)) {
			m.active = act;
			if (act) {
				orbiter->Cfg()->AddActiveModule (m.name);
				orbiter->LoadModule ("Modules\\Plugin", m.name.c_str());
			} else {
				orbiter->Cfg()->DelActiveModule (m.name);
				orbiter->UnloadModule (m.name);
			}
		}
		ImGui::EndDisabled ();
		if (m.locked && ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip ("Requested on the command line");
	}
}

void PosixLaunchpad::DrawAboutTab ()
{
	ImGui::Spacing ();
	ImGui::TextUnformatted ("Orbiter Space Flight Simulator");
	ImGui::TextDisabled ("Build %06d (%s)", orbiter->GetVersion(), __DATE__);
	ImGui::Spacing ();
	ImGui::TextWrapped ("Orbiter was created by Martin Schweiger and is developed as open source "
		"software under the MIT license by the Orbiter community.");
	ImGui::Spacing ();
	ImGui::TextUnformatted ("https://github.com/orbitersim/orbiter");
	ImGui::TextUnformatted ("https://www.orbiter-forum.com");
}
