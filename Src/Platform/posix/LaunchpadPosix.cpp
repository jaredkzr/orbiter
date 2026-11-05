// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "LaunchpadPosix.h"
#include "SettingsUI.h"
#include "Orbiter.h"
#include "Config.h"
#include "Input.h"
#include "imgui.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "UiAnchor.h"
#include <algorithm>
#include <cmath>
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

	// ---- scenario information ------------------------------------------

	/// "14 Mar 2001, 12:42 UTC" from a modified Julian date
	std::string DateString (double mjd)
	{
		// (days since 1970-01-01 to the civil date: H. Hinnant's algorithm)
		double days = std::floor (mjd);
		long z = (long)days - 40587 + 719468;
		long era = (z >= 0 ? z : z - 146096) / 146097;
		long doe = z - era * 146097;
		long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		long y = yoe + era * 400;
		long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		long mp = (5 * doy + 2) / 153;
		long d = doy - (153 * mp + 2) / 5 + 1;
		long m = mp < 10 ? mp + 3 : mp - 9;
		if (m <= 2) y++;
		int minutes = (int)std::floor ((mjd - days) * 1440.0 + 0.5);
		if (minutes >= 1440) minutes = 1439;
		static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
		char buf[64];
		snprintf (buf, sizeof(buf), "%ld %s %ld, %02d:%02d UTC", d, mon[m - 1], y, minutes / 60, minutes % 60);
		return buf;
	}

	/// Draw text with extra space between the letters (a wordmark)
	float SpacedText (ImDrawList *dl, ImVec2 p, const char *text, float spacing, ImU32 col, bool measure = false)
	{
		float x = p.x;
		for (const char *c = text; *c; c++) {
			char ch[2] = { *c, 0 };
			if (!measure) dl->AddText (ImVec2(x, p.y), col, ch);
			x += ImGui::CalcTextSize (ch).x + spacing;
		}
		return x - spacing - p.x;
	}

	/// The image file of a scenario ("" if none): <scenario>.jpg|png beside
	/// it (Utils/ScenarioShots renders them), the current state's saved image
	std::string ImageFile (const std::string &scnDir, const std::string &path)
	{
		std::vector<std::string> candidates;
		if (path == "(Current state)") {
			candidates = { "Images/CurrentState.jpg", "Images/CurrentState.png" };
		} else {
			candidates = { scnDir + path + ".jpg", scnDir + path + ".png" };
		}
		std::error_code ec;
		for (auto &c : candidates) if (fs::exists (c, ec)) return c;
		return "";
	}

	const char *RECENT_KEY = "Recent=";
	constexpr size_t RECENT_MAX = 5;

} // namespace

// =======================================================================

PosixLaunchpad::PosixLaunchpad (Orbiter *orbiter): orbiter(orbiter)
{
	LoadState ();
	if (const char *t = getenv ("ORBITER_LAUNCHPAD_TAB")) {
		int p = atoi (t);
		if (p >= 0 && p < NPAGE) page = (Page)p;
	}
	if (const char *t = getenv ("ORBITER_LAUNCHPAD_SETTINGS")) settingsPage = atoi (t);
	if (const char *a = getenv ("ORBITER_LAUNCHPAD_AUTO")) {
		autoScenario = a;
		const char *r = getenv ("ORBITER_LAUNCHPAD_RUNS");
		autoRuns = r ? atoi (r) : 1;
		SelectPath (autoScenario);
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
	imageFor.clear();
}

void PosixLaunchpad::Refresh ()
{
	frame = 0;
	scanned = false;
	modulesScanned = false;
	loading = false;
	if (!selPath.empty()) {
		// the description may have changed (e.g. the current state was saved)
		std::string p = selPath;
		selPath.clear();
		SelectPath (p);
	}
	ReleaseResources ();
}

void PosixLaunchpad::LoadState ()
{
	std::ifstream ifs (NativePath (orbiter->Cfg()->CfgDirPrm.ConfigDir) + STATE_FILE);
	std::string line, sel;
	while (std::getline (ifs, line)) {
		if (line.compare (0, 9, "Scenario=") == 0) sel = Trim (line.substr (9));
		else if (line.compare (0, 9, "Settings=") == 0) settingsPage = atoi (line.c_str() + 9);
		else if (line.compare (0, strlen (RECENT_KEY), RECENT_KEY) == 0 && recent.size() < RECENT_MAX) {
			std::string r = Trim (line.substr (strlen (RECENT_KEY)));
			if (!r.empty()) recent.push_back (r);
		}
	}
	if (sel.empty()) sel = "(Current state)";
	SelectPath (sel);
}

void PosixLaunchpad::SaveState () const
{
	std::ofstream ofs (NativePath (orbiter->Cfg()->CfgDirPrm.ConfigDir) + STATE_FILE);
	ofs << "Scenario=" << selScenario << '\n';
	ofs << "Settings=" << (settings ? (int)settings->CurrentPage () : settingsPage) << '\n';
	for (auto &r : recent) ofs << RECENT_KEY << r << '\n';
}

void PosixLaunchpad::AddRecent (const std::string &path)
{
	recent.erase (std::remove (recent.begin(), recent.end(), path), recent.end());
	recent.insert (recent.begin(), path);
	if (recent.size() > RECENT_MAX) recent.resize (RECENT_MAX);
}

// =======================================================================
// Scenario descriptions

PosixLaunchpad::ScenarioInfo PosixLaunchpad::ReadInfo (const std::string &path)
{
	ScenarioInfo inf;
	std::ifstream ifs (path);
	if (!ifs) return inf;
	enum { NONE, ENV, FOCUS, SHIPS } sec = NONE;
	std::string line, focus, vessel, cls, status, base;
	struct Ship { std::string name, cls, status, base; };
	std::vector<Ship> ships;
	auto flush = [&]() {
		if (!vessel.empty()) ships.push_back ({vessel, cls, status, base});
		vessel.clear(); cls.clear(); status.clear(); base.clear();
	};
	while (std::getline (ifs, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::string t = Trim (line);
		if (t.compare (0, 17, "BEGIN_ENVIRONMENT") == 0) { sec = ENV; continue; }
		if (t.compare (0, 11, "BEGIN_FOCUS") == 0) { sec = FOCUS; continue; }
		if (t.compare (0, 11, "BEGIN_SHIPS") == 0) { sec = SHIPS; continue; }
		if (t.compare (0, 4, "END_") == 0) { if (sec == SHIPS) flush (); sec = NONE; continue; }
		std::istringstream ls (t);
		std::string key;
		ls >> key;
		switch (sec) {
		case ENV:
			if (key == "System") ls >> inf.system;
			else if (key == "Date") {
				std::string kind;
				double v;
				if (ls >> kind >> v) {
					if (kind == "MJD") inf.date = DateString (v);
					else if (kind == "JD") inf.date = DateString (v - 2400000.5);
				}
			}
			break;
		case FOCUS:
			if (key == "Ship") ls >> focus;
			break;
		case SHIPS:
			if (t == "END") flush ();
			else if (!line.empty() && !isspace ((unsigned char)line[0]) && vessel.empty()) {
				size_t c = t.find (':');
				vessel = Trim (t.substr (0, c));
				cls = c == std::string::npos ? vessel : Trim (t.substr (c + 1));
			} else if (key == "STATUS") {
				std::string rest;
				std::getline (ls, rest);
				status = Trim (rest);
			} else if (key == "BASE") {
				std::string rest;
				std::getline (ls, rest);
				base = Trim (rest);
				base = base.substr (0, base.find (':'));
			}
			break;
		default:
			break;
		}
	}
	flush ();
	for (auto &s : ships) {
		if (!focus.empty() && s.name != focus) continue;
		inf.vessel = s.name;
		inf.cls = s.cls;
		std::istringstream ss (s.status);
		std::string state, body;
		ss >> state >> body;
		inf.body = body;
		if (state == "Landed") inf.situation = s.base.empty() ? "Landed" : "Landed at " + s.base;
		else if (state == "Orbiting") inf.situation = "In flight";
		break;
	}
	return inf;
}

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
	node.count = (int)files.size();
	for (auto &d : dirs) node.count += d.count;
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
	selCount = node.count;
	if (!node.directory) selScenario = node.path;
	std::string base = NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir) + node.path;
	desc = ReadDescription (node.directory ? base : base + ".scn", node.directory);
	info = node.directory ? ScenarioInfo() : ReadInfo (base + ".scn");
}

void PosixLaunchpad::SelectPath (const std::string &path)
{
	Node n;
	n.path = path;
	n.name = path.substr (path.find_last_of ('/') + 1);
	std::error_code ec;
	n.directory = fs::is_directory (NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir) + path, ec);
	Select (n);
}

// Returns true if a scenario was double-clicked
bool PosixLaunchpad::DrawTree (const Node &node, const std::string &f)
{
	bool launch = false;
	for (const auto &c : node.children) {
		if (!Matches (c, f)) continue;
		bool sel = (c.path == selPath);
		ImGui::PushID (c.path.c_str());
		if (c.directory) {
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
				ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding | (sel ? ImGuiTreeNodeFlags_Selected : 0);
			if (!f.empty()) ImGui::SetNextItemOpen (true);
			else if (scrollToSel && selPath.compare (0, c.path.size() + 1, c.path + "/") == 0)
				ImGui::SetNextItemOpen (true); // contains the selection
			bool open = ImGui::TreeNodeEx ("##dir", flags, "%s", c.name.c_str());
			if (ImGui::IsItemClicked () && !ImGui::IsItemToggledOpen ()) Select (c);
			// (the number of scenarios, right-aligned)
			char cnt[16];
			snprintf (cnt, sizeof(cnt), "%d", c.count);
			ImVec2 rmax = ImGui::GetItemRectMax (), rmin = ImGui::GetItemRectMin ();
			ImVec2 cs = ImGui::CalcTextSize (cnt);
			ImGui::GetWindowDrawList()->AddText (ImVec2(rmax.x - cs.x - ui::Px (8), (rmin.y + rmax.y - cs.y) * 0.5f),
				ui::Col32 (ui::TEXT_FAINT), cnt);
			if (open) {
				launch |= DrawTree (c, f);
				ImGui::TreePop ();
			}
		} else {
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
				ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding | (sel ? ImGuiTreeNodeFlags_Selected : 0);
			ImGui::PushStyleColor (ImGuiCol_Text, sel ? ui::Col (ui::TEXT) : ui::Col (ui::TEXT, 0.86f));
			ImGui::TreeNodeEx ("##scn", flags, "%s", c.name.c_str());
			ImGui::PopStyleColor ();
			if (sel) { // (an accent bar at the selected scenario)
				ImVec2 rmin = ImGui::GetItemRectMin (), rmax = ImGui::GetItemRectMax ();
				ImGui::GetWindowDrawList()->AddRectFilled (rmin, ImVec2(rmin.x + ui::Px (3), rmax.y), ui::Col32 (ui::ACCENT), ui::Px (2));
			}
			if (ImGui::IsItemClicked ()) Select (c);
			if (ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left)) launch = true;
			if (sel && scrollToSel) {
				ImGui::SetScrollHereY (0.3f);
				scrollToSel = false;
			}
		}
		ImGui::PopID ();
	}
	return launch;
}

// =======================================================================
// User interface

PosixLaunchpad::Action PosixLaunchpad::Draw (bool backdrop)
{
	Action action = NONE;
	const ImGuiViewport *vp = ImGui::GetMainViewport();

	if (loading) {
		DrawLoading ();
		return NONE;
	}
	DrawBackground (backdrop);

	ImGui::SetNextWindowPos (vp->Pos);
	ImGui::SetNextWindowSize (vp->Size);
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin ("##launchpad", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar (2);

	float margin = std::max (ui::Px (28), vp->Size.x * 0.022f);
	float headerH = ui::Px (52);
	DrawHeader (vp->Pos.x + margin, vp->Pos.y + margin * 0.8f, vp->Size.x - 2 * margin);
	float top = vp->Pos.y + margin * 0.8f + headerH + ui::Px (14);
	float h = vp->Pos.y + vp->Size.y - margin - top;
	float x = vp->Pos.x + margin, w = vp->Size.x - 2 * margin;

	switch (page) {
	case SCENARIOS: DrawScenarios (action, x, top, w, h); break;
	case SETTINGS:  DrawSettings (x, top, w, h); break;
	case ADDONS:    DrawAddons (x, top, w, h); break;
	case ABOUT:     DrawAbout (x, top, w, h); break;
	default: break;
	}
	if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed (ImGuiKey_Q, false)) exitRequest = true;
	if (exitRequest) {
		exitRequest = false;
		action = EXIT;
	}

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

	if (action == LAUNCH && selScenario.empty()) action = NONE;
	if (action == LAUNCH) AddRecent (selScenario);
	if (action != NONE) SaveState ();
	return action;
}

void PosixLaunchpad::DrawBackground (bool backdrop)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImDrawList *bg = ImGui::GetBackgroundDrawList();
	ImVec2 p0 = vp->Pos, p1 (vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	if (!backdrop) {
		// no scene: deep space blue, lighter towards the bottom
		ImU32 top = ui::Col32 (ui::BACKDROP), bot = ImGui::ColorConvertFloat4ToU32 (ui::Srgb (0x0B1630));
		bg->AddRectFilledMultiColor (p0, p1, top, top, bot, bot);
	}
	// shade under the header and along the bottom, so that the text reads
	// over a bright scene
	float hh = ui::Px (150);
	ImU32 dark = ui::Col32 (ui::BACKDROP, backdrop ? 0.7f : 0.0f), clear = ui::Col32 (ui::BACKDROP, 0.0f);
	if (backdrop) // (over the scene, under the windows)
		bg->AddRectFilledMultiColor (p0, ImVec2(p1.x, p0.y + hh), dark, dark, clear, clear);
}

void PosixLaunchpad::DrawHeader (float x, float y, float w)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ui::Fonts &f = ui::GetFonts();

	// wordmark
	ui::PushFont (f.strong, 26.0f);
	float ww = SpacedText (dl, ImVec2(x, y), "ORBITER", ui::Px (5), ui::Col32 (ui::TEXT));
	float lh = ImGui::GetFontSize ();
	ui::PopFont ();
	ui::PushFont (f.medium, ui::SIZE_SMALL);
	float sh = ImGui::GetFontSize ();
	SpacedText (dl, ImVec2(x + ww + ui::Px (16), y + lh - sh - ui::Px (3)), "SPACE FLIGHT SIMULATOR", ui::Px (2.5f), ui::Col32 (ui::TEXT_DIM));
	ui::PopFont ();

	// pages, right-aligned, and the power button
	static const char *names[NPAGE] = { "Scenarios", "Settings", "Add-ons", "About" };
	float gap = ui::Px (30);
	ui::PushFont (f.medium, ui::SIZE_LARGE);
	float tw = 0;
	for (int i = 0; i < NPAGE; i++) tw += ImGui::CalcTextSize (names[i]).x + gap;
	ui::PopFont ();
	float bx = x + w - ImGui::GetFrameHeight () - ui::Px (4);
	float tx = bx - ui::Px (12) - tw;
	for (int i = 0; i < NPAGE; i++) {
		ImGui::SetCursorScreenPos (ImVec2(tx, y + ui::Px (2)));
		if (ui::NavTab (names[i], page == i)) page = (Page)i;
		tx = ImGui::GetItemRectMax().x + gap;
	}
	ImGui::SetCursorScreenPos (ImVec2(bx, y));
	if (ui::IconButton (ICON_LC_POWER, "Quit Orbiter (Ctrl+Q)")) exitRequest = true;
}

// ---- scenarios ------------------------------------------------------------

void PosixLaunchpad::DrawScenarios (Action &action, float x, float y, float w, float h)
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

	float gap = ui::Px (20);
	float lw = std::clamp (w * 0.27f, ui::Px (340), ui::Px (520));
	float rw = std::clamp (w * 0.36f, ui::Px (440), ui::Px (760));
	if (lw + rw + gap > w) rw = w - lw - gap;

	ImGui::SetCursorScreenPos (ImVec2(x, y));
	if (ui::BeginPanel ("##browser", ImVec2(lw, h))) DrawBrowser (action);
	ui::EndPanel ();

	ImGui::SetCursorScreenPos (ImVec2(x + w - rw, y));
	if (ui::BeginPanel ("##preview", ImVec2(rw, h))) DrawPreview (action);
	ui::EndPanel ();
}

void PosixLaunchpad::DrawBrowser (Action &action)
{
	const ui::Fonts &f = ui::GetFonts();
	// search: the icon drawn inside the field
	ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2(ui::Px (34), ui::Px (8)));
	ImGui::SetNextItemWidth (-FLT_MIN);
	ImGui::InputTextWithHint ("##filter", "Search scenarios", filter, sizeof(filter));
	uianchor::SetItem ("launchpad.search");
	ImGui::PopStyleVar ();
	{
		ImVec2 r0 = ImGui::GetItemRectMin (), r1 = ImGui::GetItemRectMax ();
		float ih = ImGui::GetFontSize ();
		ImGui::GetWindowDrawList()->AddText (ImVec2(r0.x + ui::Px (11), (r0.y + r1.y - ih) * 0.5f),
			ui::Col32 (ui::TEXT_DIM), ICON_LC_SEARCH);
	}
	std::string flt = Lower (Trim (filter));

	ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2(ui::Px (8), ui::Px (5)));
	ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2(ui::Px (8), ui::Px (1)));
	ImGui::PushStyleVar (ImGuiStyleVar_IndentSpacing, ui::Px (16));
	ImGui::BeginChild ("##tree", ImVec2(0, 0), ImGuiChildFlags_None);

	// recently launched
	if (flt.empty() && !recent.empty()) {
		ui::SectionHeader ("Recent");
		for (auto &r : recent) {
			std::string name = r.substr (r.find_last_of ('/') + 1);
			std::string dir = r.find ('/') == std::string::npos ? "" : r.substr (0, r.find_last_of ('/'));
			ImGui::PushID (r.c_str());
			bool sel = (r == selPath);
			std::string lbl = std::string (ICON_LC_HISTORY) + "  " + name;
			if (ImGui::Selectable (lbl.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, ImGui::GetFrameHeight ()))) {
				SelectPath (r);
				scrollToSel = true;
				if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left)) action = LAUNCH;
			}
			if (!dir.empty()) {
				ImVec2 rmax = ImGui::GetItemRectMax (), rmin = ImGui::GetItemRectMin ();
				ui::PushFont (f.body, ui::SIZE_SMALL);
				ImVec2 ds = ImGui::CalcTextSize (dir.c_str());
				ImGui::GetWindowDrawList()->AddText (ImVec2(rmax.x - ds.x - ui::Px (8), (rmin.y + rmax.y - ds.y) * 0.5f),
					ui::Col32 (ui::TEXT_FAINT), dir.c_str());
				ui::PopFont ();
			}
			ImGui::PopID ();
		}
		ImGui::Dummy (ImVec2(0, ui::Px (8)));
	}

	ui::SectionHeader (flt.empty() ? "All scenarios" : "Matching scenarios");
	if (DrawTree (root, flt) && !selDirectory) action = LAUNCH;
	ImGui::Dummy (ImVec2(0, ui::Px (6))); // (the last row clear of the fade)
	ui::ScrollFade ();
	ImGui::EndChild ();
	ImGui::PopStyleVar (3);
}

void PosixLaunchpad::DrawPreview (Action &action)
{
	const ui::Fonts &f = ui::GetFonts();
	float w = ImGui::GetContentRegionAvail().x;
	float footer = ImGui::GetFrameHeight () * 1.5f + ui::Px (20) + 3 * ImGui::GetStyle().ItemSpacing.y;

	ImGui::BeginChild ("##previewbody", ImVec2(0, previewFill.Height (footer)), ImGuiChildFlags_None);
	DrawImage (w);

	// title
	std::string title = selPath.substr (selPath.find_last_of ('/') + 1);
	if (!desc.empty() && desc[0].type == TextBlock::HEADING) title = desc[0].text;
	ImGui::Dummy (ImVec2(0, ui::Px (6)));
	ui::PushFont (f.strong, ui::SIZE_TITLE);
	ImGui::PushTextWrapPos (0.0f);
	ImGui::TextUnformatted (title.c_str());
	ImGui::PopTextWrapPos ();
	ui::PopFont ();

	// what and where
	auto meta = [&](const char *icon, const std::string &text) {
		if (text.empty()) return;
		std::string s = std::string (icon) + "  " + text;
		ImVec2 ts = ImGui::CalcTextSize (s.c_str());
		if (ImGui::GetCursorPosX () > ImGui::GetStyle().WindowPadding.x &&
			ImGui::GetCursorPosX () + ts.x > ImGui::GetContentRegionMax().x) ImGui::NewLine ();
		ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_DIM));
		ImGui::TextUnformatted (s.c_str());
		ImGui::PopStyleColor ();
		ImGui::SameLine (0, ui::Px (18));
	};
	ImGui::Dummy (ImVec2(0, ui::Px (2)));
	if (selDirectory) {
		char n[32];
		snprintf (n, sizeof(n), "%d scenario%s", selCount, selCount == 1 ? "" : "s");
		meta (ICON_LC_FOLDER, n);
	} else {
		std::string who = info.vessel.empty() ? "" : info.cls == info.vessel ? info.vessel : info.vessel + " (" + info.cls + ")";
		meta (ICON_LC_ROCKET, who);
		meta (ICON_LC_EARTH, info.body);
		meta (info.situation.compare (0, 6, "Landed") == 0 ? ICON_LC_MAP_PIN : ICON_LC_ORBIT, info.situation);
		meta (ICON_LC_CALENDAR, info.date);
	}
	ImGui::NewLine ();
	ImGui::Dummy (ImVec2(0, ui::Px (4)));
	ImVec2 p = ImGui::GetCursorScreenPos ();
	ImGui::GetWindowDrawList()->AddLine (p, ImVec2(p.x + w, p.y), ui::Col32 (ui::DIVIDER), std::max (1.0f, ui::Scale ()));
	ImGui::Dummy (ImVec2(0, ui::Px (10)));

	DrawDescription ();
	ui::ScrollFade ();
	ImGui::EndChild ();
	previewFill.PanelEnd ();

	// actions
	ImGui::Dummy (ImVec2(0, ui::Px (6)));
	p = ImGui::GetCursorScreenPos ();
	ImGui::GetWindowDrawList()->AddLine (p, ImVec2(p.x + w, p.y), ui::Col32 (ui::DIVIDER), std::max (1.0f, ui::Scale ()));
	ImGui::Dummy (ImVec2(0, ui::Px (14)));
	float bh = ImGui::GetFrameHeight () * 1.5f;
	float y0 = ImGui::GetCursorPosY ();
	ImGui::SetCursorPosY (y0 + (bh - ImGui::GetFrameHeight ()) * 0.5f);
	ui::Toggle ("Start paused", &orbiter->Cfg()->CfgLogicPrm.bStartPaused);
	float lw = ui::Px (170);
	ImGui::SameLine (ImGui::GetContentRegionMax().x - lw);
	ImGui::SetCursorPosY (y0);
	ImGui::BeginDisabled (selScenario.empty() || selDirectory);
	ImGui::PushStyleVar (ImGuiStyleVar_FrameRounding, ui::Px (8));
	std::string lbl = std::string (ICON_LC_PLAY) + "  Launch";
	if (ui::PrimaryButton (lbl.c_str(), ImVec2(lw, bh))) action = LAUNCH;
	uianchor::SetItem ("launchpad.launch");
	ImGui::PopStyleVar ();
	ImGui::EndDisabled ();
	previewFill.FooterEnd ();
	if (!selDirectory && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed (ImGuiKey_Enter, false)) action = LAUNCH;
	if (!selDirectory && ImGui::IsKeyPressed (ImGuiKey_Enter, false) && ImGui::IsItemDeactivated ()) action = LAUNCH;
}

void PosixLaunchpad::LoadImage ()
{
	std::string key = selDirectory ? "" : selPath;
	if (key == imageFor) return;
	ReleaseResources ();
	imageFor = key;
	if (key.empty()) return;
	std::string path = ImageFile (NativePath (orbiter->Cfg()->CfgDirPrm.ScnDir), key);
	auto *gc = orbiter->GetGraphicsClient();
	if (path.empty() || !gc) return;
	image = gc->clbkLoadSurface (path.c_str(), OAPISURFACE_TEXTURE, true);
	imagePath = path;
	imageW = imageH = 0;
	if (image) gc->clbkGetSurfaceSize (image, &imageW, &imageH);
}

void PosixLaunchpad::DrawImage (float w)
{
	LoadImage ();
	float h = w * 9.0f / 16.0f;
	ImVec2 p0 = ImGui::GetCursorScreenPos (), p1 (p0.x + w, p0.y + h);
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	float r = ui::Px (8);
	auto *gc = orbiter->GetGraphicsClient();
	ImTextureID id = (image && imageW && imageH && gc) ? (ImTextureID)gc->clbkImGuiSurfaceTexture (image) : ImTextureID();
	if (id) {
		// (cropped to fill the frame)
		float ia = (float)imageW / imageH, fa = w / h;
		ImVec2 uv0 (0, 0), uv1 (1, 1);
		if (ia > fa) { float c = (1.0f - fa / ia) * 0.5f; uv0.x = c, uv1.x = 1.0f - c; }
		else         { float c = (1.0f - ia / fa) * 0.5f; uv0.y = c, uv1.y = 1.0f - c; }
		dl->AddImageRounded (ImTextureRef(id), p0, p1, uv0, uv1, IM_COL32_WHITE, r);
	} else {
		// no image yet: a quiet placeholder
		ImU32 a = ImGui::ColorConvertFloat4ToU32 (ui::Srgb (0x14203A)), b = ImGui::ColorConvertFloat4ToU32 (ui::Srgb (0x0A1020));
		dl->AddRectFilled (p0, p1, b, r);
		dl->AddRectFilledMultiColor (ImVec2(p0.x + r, p0.y), ImVec2(p1.x - r, p1.y), a, a, b, b);
		const ui::Fonts &f = ui::GetFonts();
		ui::PushFont (f.body, 54.0f);
		const char *icon = selDirectory ? ICON_LC_FOLDER_OPEN : ICON_LC_ORBIT;
		ImVec2 is = ImGui::CalcTextSize (icon);
		dl->AddText (ImVec2((p0.x + p1.x - is.x) * 0.5f, (p0.y + p1.y - is.y) * 0.5f - ui::Px (8)), ui::Col32 (ui::TEXT_FAINT, 0.7f), icon);
		ui::PopFont ();
		ui::PushFont (f.medium, ui::SIZE_SMALL);
		const char *t = selDirectory ? "Scenario folder" : "No preview yet";
		ImVec2 ts = ImGui::CalcTextSize (t);
		dl->AddText (ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y + is.y) * 0.5f), ui::Col32 (ui::TEXT_FAINT), t);
		ui::PopFont ();
	}
	dl->AddRect (p0, p1, ui::Col32 (ui::LINE, 0.8f), r, 0, std::max (1.0f, ui::Scale ()));
	ImGui::Dummy (ImVec2(w, h));
}

void PosixLaunchpad::DrawDescription ()
{
	const ui::Fonts &f = ui::GetFonts();
	bool first = true;
	ImGui::PushTextWrapPos (0.0f);
	for (const auto &b : desc) {
		if (first && b.type == TextBlock::HEADING) { first = false; continue; } // (the title)
		first = false;
		switch (b.type) {
		case TextBlock::HEADING:
			ImGui::Dummy (ImVec2(0, ui::Px (6)));
			ui::PushFont (f.strong, ui::SIZE_LARGE);
			ImGui::TextUnformatted (b.text.c_str());
			ui::PopFont ();
			ImGui::Dummy (ImVec2(0, ui::Px (2)));
			break;
		case TextBlock::ITEM: {
			ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::ACCENT));
			ImGui::TextUnformatted ("\xE2\x80\xA2");
			ImGui::PopStyleColor ();
			ImGui::SameLine (0, ui::Px (8));
			ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT, 0.9f));
			ImGui::TextUnformatted (b.text.c_str());
			ImGui::PopStyleColor ();
			break;
		}
		default:
			ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT, 0.9f));
			ImGui::TextUnformatted (b.text.c_str());
			ImGui::PopStyleColor ();
			ImGui::Dummy (ImVec2(0, ui::Px (4)));
			break;
		}
	}
	if (desc.empty() || (desc.size() == 1 && desc[0].type == TextBlock::HEADING))
		ui::TextDim ("%s", selDirectory ? "Choose a scenario in this folder." : "This scenario has no description.");
	ImGui::PopTextWrapPos ();
}

void PosixLaunchpad::DrawLoading ()
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImDrawList *dl = ImGui::GetForegroundDrawList (const_cast<ImGuiViewport*>(vp));
	ImVec2 p0 = vp->Pos, p1 (vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	dl->AddRectFilled (p0, p1, ui::Col32 (ui::BACKDROP));
	LoadImage ();
	auto *gc = orbiter->GetGraphicsClient();
	ImTextureID id = (image && imageW && imageH && gc) ? (ImTextureID)gc->clbkImGuiSurfaceTexture (image) : ImTextureID();
	if (id) { // (the scenario's image, filling the screen, dimmed)
		float ia = (float)imageW / imageH, fa = vp->Size.x / vp->Size.y;
		ImVec2 uv0 (0, 0), uv1 (1, 1);
		if (ia > fa) { float c = (1.0f - fa / ia) * 0.5f; uv0.x = c, uv1.x = 1.0f - c; }
		else         { float c = (1.0f - ia / fa) * 0.5f; uv0.y = c, uv1.y = 1.0f - c; }
		dl->AddImage (ImTextureRef(id), p0, p1, uv0, uv1, ImGui::ColorConvertFloat4ToU32 (ImVec4(0.35f, 0.35f, 0.35f, 1)));
	}
	ImU32 clear = ui::Col32 (ui::BACKDROP, 0.0f), dark = ui::Col32 (ui::BACKDROP, 0.85f);
	dl->AddRectFilledMultiColor (ImVec2(p0.x, p1.y - ui::Px (320)), p1, clear, clear, dark, dark);

	const ui::Fonts &f = ui::GetFonts();
	float m = ui::Px (56);
	std::string title = selScenario.substr (selScenario.find_last_of ('/') + 1);
	if (!desc.empty() && desc[0].type == TextBlock::HEADING) title = desc[0].text;
	ui::PushFont (f.medium, ui::SIZE_SMALL);
	float sh = ImGui::GetFontSize ();
	ui::PopFont ();
	ui::PushFont (f.strong, ui::SIZE_DISPLAY);
	float th = ImGui::GetFontSize ();
	dl->AddText (ImVec2(p0.x + m, p1.y - m - th), ui::Col32 (ui::TEXT), title.c_str());
	ui::PopFont ();
	ui::PushFont (f.medium, ui::SIZE_SMALL);
	SpacedText (dl, ImVec2(p0.x + m, p1.y - m - th - sh - ui::Px (14)), "LOADING", ui::Px (3), ui::Col32 (ui::ACCENT));
	ui::PopFont ();
	// (a thin accent line under the title)
	dl->AddRectFilled (ImVec2(p0.x + m, p1.y - m + ui::Px (12)), ImVec2(p0.x + m + ui::Px (72), p1.y - m + ui::Px (15)), ui::Col32 (ui::ACCENT), ui::Px (2));
}

// ---- settings -------------------------------------------------------------

void PosixLaunchpad::DrawSettings (float x, float y, float w, float h)
{
	if (!settings) {
		settings = std::make_unique<SettingsUI> (orbiter, SettingsUI::LAUNCHPAD);
		settings->SelectPage ((SettingsUI::Page)std::clamp (settingsPage, 0, (int)SettingsUI::NPAGE - 1));
	}
	ImGui::SetCursorScreenPos (ImVec2(x, y));
	settings->Draw (w, h, true);
}

// ---- add-ons ---------------------------------------------------------------

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

void PosixLaunchpad::DrawAddons (float x, float y, float w, float h)
{
	if (!modulesScanned) ScanModules ();
	const ui::Fonts &f = ui::GetFonts();
	float cw = std::min (w, ui::Px (900));
	ImGui::SetCursorScreenPos (ImVec2(x, y));
	if (ui::BeginAutoPanel ("##addons", cw, h)) {
		ui::SectionHeader ("Add-on modules");
		ui::TextDim ("Optional plugin modules in Modules/Plugin. A module switched on is loaded at once and stays on in later sessions.");
		ImGui::Dummy (ImVec2(0, ui::Px (8)));
		if (modules.empty()) {
			ImGui::Dummy (ImVec2(0, ui::Px (40)));
			ui::PushFont (f.body, 48.0f);
			float iw = ImGui::CalcTextSize (ICON_LC_PUZZLE).x;
			ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail().x - iw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
			ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_FAINT));
			ImGui::TextUnformatted (ICON_LC_PUZZLE);
			ImGui::PopStyleColor ();
			ui::PopFont ();
			const char *t = "No add-on modules installed";
			ui::PushFont (f.strong, ui::SIZE_LARGE);
			ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize (t).x) * 0.5f + ImGui::GetStyle().WindowPadding.x);
			ImGui::TextUnformatted (t);
			ui::PopFont ();
			const char *s = "Add-ons for this build are compiled from source for Linux and placed in Modules/Plugin.";
			ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize (s).x) * 0.5f + ImGui::GetStyle().WindowPadding.x);
			ui::TextDim ("%s", s);
		}
		for (auto &m : modules) {
			ImGui::BeginDisabled (m.locked);
			bool act = m.active;
			if (ui::Toggle (m.name.c_str(), &act)) {
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
	ui::EndPanel ();
}

// ---- about -----------------------------------------------------------------

void PosixLaunchpad::DrawAbout (float x, float y, float w, float h)
{
	const ui::Fonts &f = ui::GetFonts();
	float cw = std::min (w, ui::Px (760));
	ImGui::SetCursorScreenPos (ImVec2(x, y));
	if (ui::BeginAutoPanel ("##about", cw, h)) {
		ImGui::Dummy (ImVec2(0, ui::Px (6)));
		ui::PushFont (f.strong, ui::SIZE_TITLE);
		ImGui::TextUnformatted ("Orbiter Space Flight Simulator");
		ui::PopFont ();
		ui::TextDim ("Build %06d  \xC2\xB7  %s", orbiter->GetVersion(), __DATE__);
		ImGui::Dummy (ImVec2(0, ui::Px (10)));
		ImGui::PushTextWrapPos (0.0f);
		ImGui::TextUnformatted ("Orbiter was created by Martin Schweiger and is developed as open source "
			"software under the MIT license by the Orbiter community.");
		ImGui::PopTextWrapPos ();
		ImGui::Dummy (ImVec2(0, ui::Px (6)));
		ImGui::TextLinkOpenURL ("github.com/orbitersim/orbiter", "https://github.com/orbitersim/orbiter");
		ImGui::TextLinkOpenURL ("orbiter-forum.com", "https://www.orbiter-forum.com");

		ImGui::Dummy (ImVec2(0, ui::Px (14)));
		ui::SectionHeader ("Third-party software and fonts");
		if (ImGui::BeginTable ("##credits", 2, ImGuiTableFlags_SizingStretchProp)) {
			static const char *rows[][2] = {
				{"Dear ImGui, ImPlot", "MIT"},
				{"SDL 3", "zlib"},
				{"Vulkan Memory Allocator", "MIT"},
				{"AMD FidelityFX SDK (FSR 3)", "MIT"},
				{"Lua", "MIT"},
				{"Inter (Rasmus Andersson)", "SIL Open Font License 1.1"},
				{"JetBrains Mono", "SIL Open Font License 1.1"},
				{"Lucide icons", "ISC"},
				{"Font Awesome Free (icons)", "SIL OFL 1.1 / CC BY 4.0"},
			};
			for (auto &r : rows) {
				ImGui::TableNextRow ();
				ImGui::TableNextColumn (); ImGui::TextUnformatted (r[0]);
				ImGui::TableNextColumn (); ui::TextDim ("%s", r[1]);
			}
			ImGui::EndTable ();
		}
	}
	ui::EndPanel ();
}
