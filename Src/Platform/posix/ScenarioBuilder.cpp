// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ScenarioBuilder.h"
#include "Orbiter.h"
#include "Config.h"
#include "UiTheme.h"
#include "UiAnchor.h"
#include "IconsLucide.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

	const double PI_ = 3.14159265358979323846, RAD_ = PI_ / 180.0, DEG_ = 180.0 / PI_;
	const double GGRAV_ = 6.67430e-11;
	const char *CUSTOM_DIR = "Custom";         // (under the scenario directory)
	const char *PREVIEW_NAME = "(Preview)";    // (the preview's file there)

	std::string Native (std::string p)
	{
		std::replace (p.begin(), p.end(), '\\', '/');
		if (!p.empty() && p.back() == '/') p.pop_back();
		return p;
	}

	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n"), b = s.find_last_not_of (" \t\r\n");
		return a == std::string::npos ? "" : s.substr (a, b - a + 1);
	}

	bool IEq (const std::string &a, const char *b)
	{
		size_t n = strlen (b);
		if (a.size() != n) return false;
		for (size_t i = 0; i < n; i++) if (tolower ((unsigned char)a[i]) != tolower ((unsigned char)b[i])) return false;
		return true;
	}

	// "Key = value" of a configuration line (comments, after ';', dropped)
	bool KeyValue (std::string line, std::string &key, std::string &val)
	{
		size_t c = line.find (';');
		if (c != std::string::npos) line.erase (c);
		size_t e = line.find ('=');
		if (e == std::string::npos) return false;
		key = Trim (line.substr (0, e)), val = Trim (line.substr (e + 1));
		return !key.empty();
	}

	// an item of a configuration file ("": none)
	std::string Item (const std::string &file, const char *item)
	{
		std::ifstream ifs (file);
		std::string line, k, v;
		while (std::getline (ifs, line))
			if (KeyValue (line, k, v) && IEq (k, item)) return v;
		return "";
	}

	double ItemReal (const std::string &file, const char *item, double def)
	{
		std::string v = Item (file, item);
		char *end = nullptr;
		double d = strtod (v.c_str(), &end);
		return v.empty() || end == v.c_str() ? def : d;
	}

	// a label to the left of the next control
	void Label (const char *text, float lw)
	{
		ImGui::AlignTextToFramePadding ();
		ui::TextDim ("%s", text);
		ImGui::SameLine (lw);
		ImGui::SetNextItemWidth (-FLT_MIN);
	}

	bool Number (const char *id, double *v, const char *fmt, double lo, double hi)
	{
		bool ch = ImGui::InputDouble (id, v, 0.0, 0.0, fmt);
		if (*v < lo) *v = lo;
		if (*v > hi) *v = hi;
		return ch;
	}

	// a combo box over a list of names; true when the choice changed
	bool Choice (const char *id, int *sel, const std::vector<std::string> &names, const char *none = "(none)")
	{
		bool ch = false;
		if (*sel >= (int)names.size()) *sel = std::max (0, (int)names.size() - 1);
		if (ui::BeginCombo (id, names.empty() ? none : names[*sel].c_str())) {
			for (int i = 0; i < (int)names.size(); i++) {
				ImGui::PushID (i);
				if (ui::ComboItem (names[i].c_str(), i == *sel)) *sel = i, ch = true;
				ImGui::PopID ();
			}
			ui::EndCombo ();
		}
		return ch;
	}

	// a runway's designator from its true heading [rad]
	std::string Designator (double hdg)
	{
		int n = (int)std::floor (std::fmod (hdg * DEG_ + 360.0, 360.0) / 10.0 + 0.5) % 36;
		char c[8];
		snprintf (c, sizeof(c), "%02d", n ? n : 36);
		return c;
	}
}

// =======================================================================

ScenarioBuilder::ScenarioBuilder (Orbiter *o) : orbiter (o)
{
	cfgDir = Native (o->Cfg()->CfgDirPrm.ConfigDir);
	scnDir = Native (o->Cfg()->CfgDirPrm.ScnDir);
	ReadSystem ();
	ReadClasses ();
	// a first ship: a delta-glider (if there is one) on the first base of Earth
	Ship s;
	snprintf (s.name, sizeof(s.name), "GL-01");
	for (int i = 0; i < (int)classes.size(); i++) if (IEq (classes[i], "DeltaGlider")) s.cls = i;
	for (int i = 0; i < (int)bodies.size(); i++) if (bodies[i].name == "Earth") s.body = i;
	if (s.body < (int)bodies.size()) {
		ReadBases (bodies[s.body]);
		auto &bs = bodies[s.body].bases;
		for (int i = 0; i < (int)bs.size(); i++) if (bs[i].name == "Cape Canaveral") s.base = i;
	}
	ships.push_back (s);
	// (today, at noon)
	time_t t = time (nullptr);
	if (struct tm *g = gmtime (&t)) date[0] = g->tm_year + 1900, date[1] = g->tm_mon + 1, date[2] = g->tm_mday;
}

void ScenarioBuilder::ReadSystem ()
{
	bodies.clear ();
	std::ifstream ifs (cfgDir + "/" + system + ".cfg");
	std::string line, k, v;
	while (std::getline (ifs, line)) {
		if (!KeyValue (line, k, v)) continue;
		Body b;
		size_t c = k.find (':');
		std::string kind = c == std::string::npos ? k : k.substr (c + 1);
		if (c != std::string::npos) b.parent = Trim (k.substr (0, c));
		if (kind.compare (0, 4, "Star") == 0) b.star = true;
		else if (kind.compare (0, 6, "Planet") != 0 && kind.compare (0, 4, "Moon") != 0) continue;
		b.name = v;
		std::string f = cfgDir + "/" + v + ".cfg";
		b.size = ItemReal (f, "Size", 0.0), b.mass = ItemReal (f, "Mass", 0.0);
		b.obliquity = ItemReal (f, "Obliquity", 0.0), b.lan = ItemReal (f, "LAN", 0.0);
		if (b.size > 0.0 && b.mass > 0.0) bodies.push_back (b);
	}
}

void ScenarioBuilder::ReadBases (Body &b)
{
	if (b.basesRead) return;
	b.basesRead = true;
	std::error_code ec;
	std::vector<fs::path> files;
	for (auto &e : fs::directory_iterator (cfgDir + "/" + b.name + "/Base", ec))
		if (e.path().extension() == ".cfg") files.push_back (e.path());
	std::sort (files.begin(), files.end());
	for (auto &f : files) {
		std::ifstream ifs (f);
		BaseRec r;
		std::string line, k, v, block;
		Runway rw{};
		Pad pad{};
		bool hasLoc = false, inList = false; // (inList: among the base's objects, each a block from its type to END)
		while (std::getline (ifs, line)) {
			size_t c = line.find (';');
			if (c != std::string::npos) line.erase (c);
			std::string t = Trim (line);
			if (t.empty()) continue;
			std::istringstream is (t);
			std::string w;
			is >> w;
			if (w == "BEGIN_OBJECTLIST") { inList = true; continue; }
			if (w == "END_OBJECTLIST") { inList = false; continue; }
			if (block.empty()) {
				if (!inList) {
					if (KeyValue (t, k, v)) {
						if (IEq (k, "Name")) r.name = v;
						else if (IEq (k, "Location")) hasLoc = sscanf (v.c_str(), "%lf%lf", &r.lng, &r.lat) == 2;
					}
				} else if (KeyValue (t, k, v)) {
					if (IEq (k, "Name")) r.name = v;
					else if (IEq (k, "Location")) hasLoc = sscanf (v.c_str(), "%lf%lf", &r.lng, &r.lat) == 2;
				} else if (w.compare (0, 4, "LPAD") == 0) block = "LPAD", pad = Pad{ 0, 0 };
				else if (w == "RUNWAY") block = "RUNWAY", rw = Runway{};
				else if (w.compare (0, 6, "BEGIN_") != 0 && w.compare (0, 4, "END_") != 0 && t.find (' ') == std::string::npos && t.find ('\t') == std::string::npos)
					block = w; // (another object's block: skipped to its END)
			} else if (w == "END") {
				if (block == "LPAD") r.pads.push_back (pad);
				else if (block == "RUNWAY" && std::hypot (rw.x2 - rw.x1, rw.z2 - rw.z1) > 100.0) r.runways.push_back (rw);
				block.clear ();
			} else if (block == "LPAD") {
				double y;
				if (w == "POS") is >> pad.x >> y >> pad.z;
			} else if (block == "RUNWAY") {
				double y;
				if (w == "END1") is >> rw.x1 >> y >> rw.z1;
				else if (w == "END2") is >> rw.x2 >> y >> rw.z2;
				else if (w == "OVERRUN") { is >> rw.overrun1; if (!(is >> rw.overrun2)) rw.overrun2 = rw.overrun1; }
				else if (w == "DESIG") is >> rw.desig1 >> rw.desig2;
			}
		}
		if (r.name.empty() || !hasLoc) continue;
		for (Runway &q : r.runways) {
			double hdg = std::atan2 (q.z2 - q.z1, -(q.x2 - q.x1));
			if (q.desig1.empty()) q.desig1 = Designator (hdg);
			if (q.desig2.empty()) q.desig2 = Designator (hdg + PI_);
		}
		b.bases.push_back (r);
	}
	std::sort (b.bases.begin(), b.bases.end(), [](const BaseRec &a, const BaseRec &c) { return a.name < c.name; });
}

void ScenarioBuilder::ReadClasses ()
{
	classes.clear ();
	std::error_code ec;
	std::string root = cfgDir + "/Vessels";
	for (auto it = fs::recursive_directory_iterator (root, ec); !ec && it != fs::recursive_directory_iterator (); it.increment (ec)) {
		if (!it->is_regular_file (ec) || it->path().extension() != ".cfg") continue;
		// (a class as scenarios name it: the path below Vessels, without the extension;
		// not those a scenario editor may not create: parts of other vessels)
		if (IEq (Item (it->path().string(), "EditorCreate"), "FALSE")) continue;
		std::string rel = fs::relative (it->path(), root, ec).replace_extension ("").generic_string ();
		if (!rel.empty()) classes.push_back (rel);
	}
	std::sort (classes.begin(), classes.end());
}

// =======================================================================
// The scenario

double ScenarioBuilder::Mjd () const
{
	// (Fliegel and Van Flandern: the Julian day number of the Gregorian date)
	long y = date[0], m = date[1], d = date[2];
	long a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
	long jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
	return (double)jdn - 2400001.0 + (date[3] + date[4] / 60.0) / 24.0;
}

std::vector<std::string> ScenarioBuilder::Sites (const BaseRec &b) const
{
	std::vector<std::string> s;
	char c[64];
	for (size_t i = 0; i < b.pads.size(); i++) { snprintf (c, sizeof(c), "Pad %d", (int)i + 1); s.push_back (c); }
	for (const Runway &r : b.runways) {
		s.push_back ("Runway " + r.desig1);
		s.push_back ("Runway " + r.desig2);
	}
	return s;
}

const ScenarioBuilder::Ship *ScenarioBuilder::Root (const Ship &s, double &dist) const
{
	const Ship *p = &s;
	dist = 0.0;
	for (int n = 0; p->where == BESIDE; n++) {
		if (n > (int)ships.size() || p->beside < 0 || p->beside >= (int)ships.size() || &ships[p->beside] == p) return nullptr;
		dist += p->dist;
		p = &ships[p->beside];
	}
	return p;
}

bool ScenarioBuilder::LandedAt (const Ship &s, double &lng, double &lat, double &heading) const
{
	if (s.body < 0 || s.body >= (int)bodies.size()) return false;
	const Body &b = bodies[s.body];
	lng = s.lng, lat = s.lat, heading = s.heading;
	if (s.where != AT_BASE) return true;
	if (s.base < 0 || s.base >= (int)b.bases.size()) return false;
	const BaseRec &r = b.bases[s.base];
	// (the base's frame: x south, z east [m])
	double x = 0, z = 0;
	int np = (int)r.pads.size(), k = s.site - np;
	if (s.site < np) x = r.pads[s.site].x, z = r.pads[s.site].z;
	else if (k >= 0 && k / 2 < (int)r.runways.size()) {
		const Runway &q = r.runways[k / 2];
		double dx = q.x2 - q.x1, dz = q.z2 - q.z1, len = std::hypot (dx, dz);
		dx /= len, dz /= len;
		// (on the centreline, 60 m past the threshold)
		if (k % 2 == 0) x = q.x1 + dx * (q.overrun1 + 60.0), z = q.z1 + dz * (q.overrun1 + 60.0), heading = std::atan2 (dz, -dx) * DEG_;
		else x = q.x2 - dx * (q.overrun2 + 60.0), z = q.z2 - dz * (q.overrun2 + 60.0), heading = std::atan2 (-dz, dx) * DEG_;
		heading = std::fmod (heading + 360.0, 360.0);
	}
	lat = r.lat - x / b.size * DEG_;
	lng = r.lng + z / (b.size * std::max (std::cos (lat * RAD_), 1e-3)) * DEG_;
	return true;
}

std::string ScenarioBuilder::ShipText (const Ship &s) const
{
	char c[512];
	std::string cls = s.cls >= 0 && s.cls < (int)classes.size() ? classes[s.cls] : "";
	std::string out = std::string (s.name) + ":" + cls + "\n";
	double along = 0.0;
	const Ship *root = Root (s, along);
	if (!root || root->body < 0 || root->body >= (int)bodies.size()) return "";
	const Body &b = bodies[root->body];
	if (root->where == IN_ORBIT) {
		// the state from the elements over the body's equator (the periapsis' altitude, the eccentricity;
		// a ship beside it: that far behind on the same orbit), turned to the ecliptic's frame
		double e = std::clamp (root->ecc, 0.0, 0.99), rp = b.size + root->alt * 1e3, a = rp / (1.0 - e);
		double mu = GGRAV_ * b.mass, p = a * (1.0 - e * e);
		double nu = root->anom * RAD_ - along / a, inc = root->inc * RAD_, O = root->lan * RAD_, w = root->argp * RAD_;
		double r = p / (1.0 + e * std::cos (nu));
		// (perifocal: P to the periapsis, Q a quarter ahead)
		double xp = r * std::cos (nu), yp = r * std::sin (nu);
		double k = std::sqrt (mu / p), vxp = -k * std::sin (nu), vyp = k * (e + std::cos (nu));
		double cO = std::cos (O), sO = std::sin (O), ci = std::cos (inc), si = std::sin (inc), cw = std::cos (w), sw = std::sin (w);
		double P[3] = { cO * cw - sO * sw * ci, sO * cw + cO * sw * ci, sw * si };
		double Q[3] = { -cO * sw - sO * cw * ci, -sO * sw + cO * cw * ci, cw * si };
		// (right-handed, Z the body's north: Orbiter's equatorial frame is x = X, y = Z, z = Y)
		double pe[3], ve[3];
		pe[0] = xp * P[0] + yp * Q[0], pe[2] = xp * P[1] + yp * Q[1], pe[1] = xp * P[2] + yp * Q[2];
		ve[0] = vxp * P[0] + vyp * Q[0], ve[2] = vxp * P[1] + vyp * Q[1], ve[1] = vxp * P[2] + vyp * Q[2];
		// (equator to ecliptic: the body's axis, CelestialBody::UpdatePrecession)
		double sl = std::sin (b.lan), cl = std::cos (b.lan), se = std::sin (b.obliquity), ce = std::cos (b.obliquity);
		double R[3][3] = { { cl, -sl * se, -sl * ce }, { 0, ce, -se }, { sl, cl * se, cl * ce } };
		double pg[3], vg[3];
		for (int i = 0; i < 3; i++) {
			pg[i] = R[i][0] * pe[0] + R[i][1] * pe[1] + R[i][2] * pe[2];
			vg[i] = R[i][0] * ve[0] + R[i][1] * ve[1] + R[i][2] * ve[2];
		}
		snprintf (c, sizeof(c), "  STATUS Orbiting %s\n  RPOS %.3f %.3f %.3f\n  RVEL %.5f %.5f %.5f\n", b.name.c_str(), pg[0], pg[1], pg[2], vg[0], vg[1], vg[2]);
		out += c;
	} else {
		double lng, lat, hdg;
		if (!LandedAt (*root, lng, lat, hdg)) return "";
		bool pad = root == &s && s.where == AT_BASE && s.site < (int)b.bases[s.base].pads.size();
		if (pad) { // (on a pad: the base's own place for it)
			snprintf (c, sizeof(c), "  STATUS Landed %s\n  BASE %s:%d\n  HEADING %.2f\n", b.name.c_str(), b.bases[s.base].name.c_str(), s.site + 1, s.heading);
		} else {
			// (a ship beside another: that far to its east)
			lng += along / (b.size * std::max (std::cos (lat * RAD_), 1e-3)) * DEG_;
			snprintf (c, sizeof(c), "  STATUS Landed %s\n  POS %.7f %.7f\n  HEADING %.2f\n", b.name.c_str(), lng, lat, hdg);
		}
		out += c;
	}
	return out + "END\n";
}

std::string ScenarioBuilder::Summary () const
{
	if (ships.empty() || focus >= (int)ships.size()) return "";
	const Ship &s = ships[focus];
	double d;
	const Ship *r = Root (s, d);
	std::string cls = s.cls < (int)classes.size() ? classes[s.cls] : "";
	std::string out = std::string (s.name) + " (" + cls + ")";
	if (r && r->body < (int)bodies.size()) {
		const Body &b = bodies[r->body];
		char c[160];
		if (r->where == IN_ORBIT) snprintf (c, sizeof(c), " in orbit about %s, %.0f km up.", b.name.c_str(), r->alt);
		else if (r->where == AT_BASE && r->base < (int)b.bases.size()) snprintf (c, sizeof(c), " at %s, %s.", b.bases[r->base].name.c_str(), b.name.c_str());
		else snprintf (c, sizeof(c), " on %s at %.3f, %.3f.", b.name.c_str(), r->lng, r->lat);
		out += c;
	}
	if (ships.size() > 1) out += " " + std::to_string (ships.size()) + " ships.";
	return out + " Made with the scenario builder.";
}

std::string ScenarioBuilder::Text () const
{
	char c[256];
	std::string out = "BEGIN_DESC\n" + (Trim (descr).empty() ? Summary () : Trim (descr)) + "\nEND_DESC\n\n";
	// (tests, ORBITER_BUILDER_CHECK: the scenario's ships checked by Script/Tests/BuilderTest.lua)
	const char *chk = getenv ("ORBITER_BUILDER_CHECK");
	snprintf (c, sizeof(c), "BEGIN_ENVIRONMENT\n  System %s\n  Date MJD %.6f\n%sEND_ENVIRONMENT\n\n", system, Mjd (),
		chk && *chk == '1' ? "  Script Tests/BuilderTest\n" : "");
	out += c;
	if (!ships.empty() && focus < (int)ships.size()) {
		const char *f = ships[focus].name;
		out += std::string ("BEGIN_FOCUS\n  Ship ") + f + "\nEND_FOCUS\n\n";
		out += std::string ("BEGIN_CAMERA\n  TARGET ") + f + "\n";
		out += camera == 0 ? "  MODE Cockpit\n  FOV 50.00\n" : "  MODE Extern\n  POS 60.00 20.00 15.00\n  TRACKMODE TargetRelative\n  FOV 50.00\n";
		out += "END_CAMERA\n\n";
	}
	out += "BEGIN_SHIPS\n";
	for (const Ship &s : ships) out += ShipText (s);
	return out + "END_SHIPS\n";
}

std::string ScenarioBuilder::FileName () const
{
	std::string n = Trim (name);
	for (char &ch : n) if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 32) ch = '_';
	return n;
}

bool ScenarioBuilder::Save (std::string &err)
{
	if (FileName ().empty()) { err = "The scenario needs a name."; return false; }
	if (ships.empty()) { err = "The scenario needs a ship."; return false; }
	for (size_t i = 0; i < ships.size(); i++) {
		std::string n = ships[i].name;
		if (Trim (n).empty() || n.find_first_of (" \t:") != std::string::npos) { err = "A ship's name may not be empty or hold spaces or colons."; return false; }
		for (size_t j = 0; j < i; j++) if (n == ships[j].name) { err = "Two ships are named " + n + "."; return false; }
		if (ShipText (ships[i]).empty()) { err = n + " has no place to start (the ship it is beside, its base)."; return false; }
		if (ships[i].cls >= (int)classes.size()) { err = "No vessel classes were found."; return false; }
	}
	std::error_code ec;
	fs::create_directories (scnDir + "/" + CUSTOM_DIR, ec);
	std::string rel = std::string (CUSTOM_DIR) + "/" + FileName ();
	std::ofstream ofs (scnDir + "/" + rel + ".scn");
	ofs << Text ();
	if (!ofs.good ()) { err = "Could not write " + scnDir + "/" + rel + ".scn."; return false; }
	saved = rel;
	return true;
}

void ScenarioBuilder::MakeTestScenario ()
{
	// a ship of each kind of start: on a pad, on a runway, on the surface, in orbit, beside the one in orbit
	Ship base = ships.empty() ? Ship () : ships[0];
	ships.clear ();
	auto add = [&](const char *n, int where) -> Ship& { Ship s = base; snprintf (s.name, sizeof(s.name), "%s", n); s.where = where; ships.push_back (s); return ships.back(); };
	add ("PAD", AT_BASE).site = 0;
	Ship &rw = add ("RWY", AT_BASE);
	if (rw.body < (int)bodies.size() && rw.base < (int)bodies[rw.body].bases.size())
		rw.site = (int)bodies[rw.body].bases[rw.base].pads.size(); // (its first runway's first end)
	Ship &sf = add ("SURF", ON_SURFACE);
	sf.lng = -117.0, sf.lat = 35.0, sf.heading = 90.0;
	Ship &ob = add ("ORB", IN_ORBIT);
	ob.alt = 300.0, ob.inc = 51.6, ob.lan = 40.0;
	Ship &bs = add ("NEAR", BESIDE);
	bs.beside = 3, bs.dist = 500.0;
	focus = 3, sel = 3;
}

// =======================================================================
// User interface

void ScenarioBuilder::DrawGeneral (float w)
{
	float lw = ui::Px (96);
	ui::SectionHeader ("Scenario");
	Label ("Name", lw);
	ImGui::InputText ("##name", name, sizeof(name));
	uianchor::SetItem ("builder.name");
	Label ("Description", lw);
	ImGui::InputTextWithHint ("##descr", "(written for you)", descr, sizeof(descr));
	Label ("Date (UTC)", lw);
	{
		float avail = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x;
		float fw = (avail - 4 * gap) / 6.0f;
		static const char *ids[5] = { "##y", "##mo", "##d", "##h", "##mi" };
		static const int lo[5] = { 1900, 1, 1, 0, 0 }, hi[5] = { 2200, 12, 31, 23, 59 };
		for (int i = 0; i < 5; i++) {
			if (i) ImGui::SameLine (0, gap);
			ImGui::SetNextItemWidth (i == 0 ? 2 * fw : fw);
			ImGui::InputInt (ids[i], &date[i], 0, 0);
			date[i] = std::clamp (date[i], lo[i], hi[i]);
		}
	}
	Label ("View", lw);
	static const char *cams[2] = { "Cockpit", "Outside" };
	ui::Segmented ("##camera", cams, 2, &camera);
}

void ScenarioBuilder::DrawShips (float w)
{
	ImGui::Dummy (ImVec2(0, ui::Px (8)));
	ui::SectionHeader ("Ships");
	int remove = -1;
	for (int i = 0; i < (int)ships.size(); i++) {
		ImGui::PushID (i);
		std::string cls = ships[i].cls < (int)classes.size() ? classes[ships[i].cls] : "";
		if (ui::ListRow ("##ship", i == focus ? ICON_LC_ROCKET : ICON_LC_CIRCLE, ships[i].name, cls.c_str(), i == sel, nullptr,
			i == focus ? ui::ACCENT : ui::TEXT_DIM)) sel = i;
		ImGui::PopID ();
	}
	ImGui::Dummy (ImVec2(0, ui::Px (6)));
	float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	if (ui::SecondaryButton (ICON_LC_PLUS "  Add a ship", ImVec2(bw, 0))) {
		Ship s = ships.empty() ? Ship () : ships[sel];
		for (int n = (int)ships.size() + 1; ; n++) {
			snprintf (s.name, sizeof(s.name), "SHIP-%02d", n);
			if (std::none_of (ships.begin(), ships.end(), [&](const Ship &o) { return !strcmp (o.name, s.name); })) break;
		}
		if (!ships.empty()) s.where = BESIDE, s.beside = sel;
		ships.push_back (s);
		sel = (int)ships.size() - 1;
	}
	uianchor::SetItem ("builder.add");
	ImGui::SameLine ();
	ImGui::BeginDisabled (ships.size() < 2);
	if (ui::SecondaryButton (ICON_LC_TRASH_2 "  Remove", ImVec2(bw, 0))) remove = sel;
	ImGui::EndDisabled ();
	if (remove >= 0) {
		ships.erase (ships.begin() + remove);
		for (Ship &s : ships) { // (those beside it: on their own, where it was; the indices after it shift)
			if (s.beside == remove) s.beside = 0;
			else if (s.beside > remove) s.beside--;
		}
		if (focus == remove) focus = 0; else if (focus > remove) focus--;
		sel = std::min (sel, (int)ships.size() - 1);
	}
}

void ScenarioBuilder::DrawShip (Ship &s, int idx, float w)
{
	float lw = ui::Px (120);
	ui::SectionHeader ("Ship");
	Label ("Name", lw);
	ImGui::InputText ("##sname", s.name, sizeof(s.name), ImGuiInputTextFlags_CharsNoBlank);
	Label ("Class", lw);
	Choice ("##class", &s.cls, classes, "(no vessel classes found)");
	Label ("In focus", lw);
	bool f = idx == focus;
	if (ui::Toggle ("##focus", &f) && f) focus = idx;

	ImGui::Dummy (ImVec2(0, ui::Px (8)));
	ui::SectionHeader ("Where it starts");
	static const char *wh[NWHERE] = { "At a base", "On the surface", "In orbit", "Beside a ship" };
	int nw = ships.size() > 1 ? NWHERE : NWHERE - 1;
	if (s.where >= nw) s.where = AT_BASE;
	ui::Segmented ("##where", wh, nw, &s.where);
	ImGui::Dummy (ImVec2(0, ui::Px (4)));

	std::vector<std::string> bn;
	for (const Body &b : bodies) bn.push_back (b.name);
	if (s.where != BESIDE) {
		Label ("Body", lw);
		if (Choice ("##body", &s.body, bn, "(no bodies found)")) s.base = s.site = 0;
	}
	if (s.body >= (int)bodies.size()) return;
	Body &b = bodies[s.body];

	if (s.where == AT_BASE) {
		ReadBases (b);
		std::vector<std::string> names;
		for (const BaseRec &r : b.bases) names.push_back (r.name);
		Label ("Base", lw);
		if (Choice ("##base", &s.base, names, "(this body has no bases)")) s.site = 0;
		if (s.base < (int)b.bases.size()) {
			std::vector<std::string> sites = Sites (b.bases[s.base]);
			Label ("Pad or runway", lw);
			Choice ("##site", &s.site, sites, "(none: at the base's centre)");
			if (s.site < (int)b.bases[s.base].pads.size() || sites.empty()) {
				Label ("Heading [deg]", lw);
				Number ("##hdg", &s.heading, "%.1f", 0.0, 360.0);
			}
		}
	} else if (s.where == ON_SURFACE) {
		Label ("Longitude [deg]", lw);
		Number ("##lng", &s.lng, "%.5f", -180.0, 180.0);
		Label ("Latitude [deg]", lw);
		Number ("##lat", &s.lat, "%.5f", -90.0, 90.0);
		Label ("Heading [deg]", lw);
		Number ("##hdg", &s.heading, "%.1f", 0.0, 360.0);
	} else if (s.where == IN_ORBIT) {
		Label ("Periapsis [km]", lw);
		Number ("##alt", &s.alt, "%.1f", 1.0, 1e9);
		Label ("Eccentricity", lw);
		Number ("##ecc", &s.ecc, "%.4f", 0.0, 0.99);
		Label ("Inclination [deg]", lw);
		Number ("##inc", &s.inc, "%.3f", 0.0, 180.0);
		Label ("Ascending node [deg]", lw);
		Number ("##lan", &s.lan, "%.3f", 0.0, 360.0);
		Label ("Arg. of periapsis [deg]", lw);
		Number ("##argp", &s.argp, "%.3f", 0.0, 360.0);
		Label ("True anomaly [deg]", lw);
		Number ("##anom", &s.anom, "%.3f", 0.0, 360.0);
		// (what it comes to)
		double e = std::clamp (s.ecc, 0.0, 0.99), a = (b.size + s.alt * 1e3) / (1.0 - e);
		double T = 2.0 * PI_ * std::sqrt (a * a * a / (GGRAV_ * b.mass));
		ImGui::Dummy (ImVec2(0, ui::Px (4)));
		ui::TextDim ("Over %s's equator. Apoapsis %.1f km, period %.1f min.", b.name.c_str(), (a * (1.0 + e) - b.size) * 1e-3, T / 60.0);
	} else {
		std::vector<std::string> names;
		std::vector<int> index;
		for (int i = 0; i < (int)ships.size(); i++) if (i != idx) names.push_back (ships[i].name), index.push_back (i);
		int k = 0;
		for (int i = 0; i < (int)index.size(); i++) if (index[i] == s.beside) k = i;
		Label ("Ship", lw);
		Choice ("##beside", &k, names);
		if (!index.empty()) s.beside = index[k];
		Label ("Distance [m]", lw);
		Number ("##dist", &s.dist, "%.0f", -1e6, 1e6);
		double d;
		const Ship *r = Root (s, d);
		ImGui::Dummy (ImVec2(0, ui::Px (4)));
		ImGui::PushTextWrapPos (0.0f);
		if (!r) ui::TextDim ("These ships are each beside the other: one of them needs a place of its own.");
		else if (r->where == IN_ORBIT) ui::TextDim ("On the same orbit, that far behind it.");
		else ui::TextDim ("On the ground, that far to its east.");
		ImGui::PopTextWrapPos ();
	}
}

ScenarioBuilder::Result ScenarioBuilder::Draw (float x, float y, float w, float h, bool canPreview)
{
	Result res = NONE;
	// (tests: ORBITER_LAUNCHPAD_BUILD=<name> - a scenario with a ship of each kind of start, saved and launched)
	if (const char *t = getenv ("ORBITER_LAUNCHPAD_BUILD")) {
		if (*t && !testDone) {
			testDone = true;
			MakeTestScenario ();
			snprintf (name, sizeof(name), "%s", t);
			std::string err;
			if (Save (err)) return LAUNCH;
			fprintf (stderr, "Scenario builder: %s\n", err.c_str());
		}
	}
	const ui::Fonts &f = ui::GetFonts();
	float gap = ui::Px (20);
	float lw = std::clamp (w * 0.27f, ui::Px (340), ui::Px (520));
	float mw = std::clamp (w * 0.30f, ui::Px (380), ui::Px (560));
	float rw = w - lw - mw - 2 * gap;
	if (sel >= (int)ships.size()) sel = 0;

	ImGui::SetCursorScreenPos (ImVec2(x, y));
	if (ui::BeginPanel ("##bgeneral", ImVec2(lw, h), 0.95f, 0, true)) {
		DrawGeneral (lw);
		DrawShips (lw);
	}
	ui::EndPanel ();

	ImGui::SetCursorScreenPos (ImVec2(x + lw + gap, y));
	if (ui::BeginPanel ("##bship", ImVec2(mw, h), 0.95f, 0, true)) {
		if (!ships.empty()) DrawShip (ships[sel], sel, mw);
	}
	ui::EndPanel ();

	ImGui::SetCursorScreenPos (ImVec2(x + lw + mw + 2 * gap, y));
	if (ui::BeginPanel ("##bfile", ImVec2(rw, h))) {
		float footer = ImGui::GetFrameHeight () * 2.6f + ui::Px (40);
		ui::SectionHeader ("Scenario file");
		std::string text = Text ();
		ImGui::BeginChild ("##btext", ImVec2(0, -footer), ImGuiChildFlags_None);
		ui::PushFont (f.mono, ui::SIZE_SMALL);
		ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (ui::TEXT_DIM));
		ImGui::TextUnformatted (text.c_str());
		ImGui::PopStyleColor ();
		ui::PopFont ();
		ui::ScrollFade ();
		ImGui::EndChild ();

		ImGui::Dummy (ImVec2(0, ui::Px (4)));
		if (!status.empty()) {
			ImGui::PushStyleColor (ImGuiCol_Text, ui::Col (statusError ? ui::WARNING : ui::TEXT_DIM));
			ImGui::PushTextWrapPos (0.0f);
			ImGui::TextUnformatted (status.c_str());
			ImGui::PopTextWrapPos ();
			ImGui::PopStyleColor ();
		} else ui::TextDim ("Saves to Scenarios/%s/%s.scn", CUSTOM_DIR, FileName ().c_str());
		ImGui::Dummy (ImVec2(0, ui::Px (6)));
		float bh = ImGui::GetFrameHeight () * 1.5f, sp = ImGui::GetStyle().ItemSpacing.x;
		float bw = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3.0f;
		std::string err;
		ImGui::BeginDisabled (!canPreview);
		if (ui::SecondaryButton (ICON_LC_EYE "  Preview", ImVec2(bw, bh))) {
			// (the scenario as it is, behind the launchpad)
			std::error_code ec;
			fs::create_directories (scnDir + "/" + CUSTOM_DIR, ec);
			std::string file = scnDir + "/" + CUSTOM_DIR + "/" + PREVIEW_NAME + ".scn";
			std::ofstream ofs (file);
			ofs << Text ();
			if (ofs.good ()) {
				previewFile = fs::absolute (file, ec).string ();
				status = "Shown behind the launchpad.", statusError = false;
				res = PREVIEW;
			} else status = "Could not write the preview's file.", statusError = true;
		}
		ImGui::EndDisabled ();
		if (!canPreview && ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip ("Needs the launchpad's live backdrop (Settings)");
		uianchor::SetItem ("builder.preview");
		ImGui::SameLine ();
		if (ui::SecondaryButton (ICON_LC_SAVE "  Save", ImVec2(bw, bh))) {
			if (Save (err)) status = "Saved: Scenarios/" + saved + ".scn", statusError = false;
			else status = err, statusError = true;
		}
		uianchor::SetItem ("builder.save");
		ImGui::SameLine ();
		ImGui::PushStyleVar (ImGuiStyleVar_FrameRounding, ui::Px (8));
		if (ui::PrimaryButton (ICON_LC_PLAY "  Save and launch", ImVec2(bw, bh))) {
			if (Save (err)) res = LAUNCH;
			else status = err, statusError = true;
		}
		ImGui::PopStyleVar ();
		uianchor::SetItem ("builder.launch");
	}
	ui::EndPanel ();
	return res;
}
