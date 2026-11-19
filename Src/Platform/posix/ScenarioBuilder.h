// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Scenario builder of the launchpad: a scenario made without editing
// files. The date, the ships (each with its class and where it starts:
// on a base's pad or runway, on a body's surface by longitude, latitude
// and heading, in orbit by its elements over the body's equator, or
// beside another ship), the ship in focus and the camera - written as a
// scenario file (Scenarios/Custom/<name>.scn), previewed behind the
// launchpad, launched.
//
// What there is to choose from is read from the configuration files (the
// planetary system's bodies, their bases with pads and runways, the vessel
// classes): no session is needed.
// =======================================================================

#ifndef __SCENARIOBUILDER_H
#define __SCENARIOBUILDER_H

#include <string>
#include <vector>

class Orbiter;

class ScenarioBuilder {
public:
	enum Result { NONE, PREVIEW, LAUNCH };

	explicit ScenarioBuilder (Orbiter *orbiter);

	/// Draw the builder in the rectangle given (within the launchpad's
	/// window). canPreview: a scene is drawn behind the launchpad, which
	/// can show the scenario. Returns what was asked for: PREVIEW (the file
	/// PreviewFile() behind the launchpad), LAUNCH (the scenario Saved())
	Result Draw (float x, float y, float w, float h, bool canPreview);

	/// The scenario saved last: its path relative to the scenario directory, without extension
	const std::string &Saved () const { return saved; }
	/// The preview's scenario file (absolute path)
	const std::string &PreviewFile () const { return previewFile; }

	/// The scenario as text (preview: without a description, time stopped from running away: as it is)
	std::string Text () const;
	/// Write it to Scenarios/Custom/<name>.scn (false: err says why not)
	bool Save (std::string &err);

	// (tests) the default scenario with a ship of each kind of start
	void MakeTestScenario ();

	struct Runway { double x1, z1, x2, z2, overrun1, overrun2; std::string desig1, desig2; };
	struct Pad { double x, z; }; // (base frame: x south, z east [m])
	struct BaseRec { std::string name; double lng = 0, lat = 0; std::vector<Pad> pads; std::vector<Runway> runways; }; // [deg]
	struct Body {
		std::string name, parent;
		bool star = false;
		double size = 0, mass = 0, obliquity = 0, lan = 0; // [m], [kg], [rad]
		bool basesRead = false;
		std::vector<BaseRec> bases;
	};
	enum Where { AT_BASE, ON_SURFACE, IN_ORBIT, BESIDE, NWHERE };
	struct Ship {
		char name[40] = "";
		int cls = 0;
		int where = AT_BASE;
		int body = 0;          // (index into bodies)
		int base = 0, site = 0; // site: pads first (0..npad-1), then each runway's two ends
		double lng = 0, lat = 0, heading = 0; // [deg]
		double alt = 300, ecc = 0, inc = 0, lan = 0, argp = 0, anom = 0; // [km above the mean radius, at periapsis], [deg]
		int beside = 0;        // (index into ships)
		double dist = 200;     // [m] behind it (in orbit), east of it (landed)
	};

private:
	void ReadSystem ();
	void ReadBases (Body &b);
	void ReadClasses ();
	void DrawGeneral (float w);
	void DrawShips (float w);
	void DrawShip (Ship &s, int idx, float w);
	std::string ShipText (const Ship &s) const;
	/// the ship a chain of BESIDEs ends at, and how far along it [m] (null: the chain has no end)
	const Ship *Root (const Ship &s, double &dist) const;
	bool LandedAt (const Ship &s, double &lng, double &lat, double &heading) const; // [deg]
	std::vector<std::string> Sites (const BaseRec &b) const;
	double Mjd () const;
	std::string Summary () const;
	std::string FileName () const;

	Orbiter *orbiter;
	std::string cfgDir, scnDir;
	std::vector<Body> bodies;
	std::vector<std::string> classes;
	std::vector<Ship> ships;
	int focus = 0, sel = 0;
	char name[64] = "My scenario";
	char descr[256] = "";
	char system[32] = "Sol";
	int date[5] = { 2001, 3, 17, 12, 0 }; // year, month, day, hour, minute (UTC)
	int camera = 0;                       // 0 cockpit, 1 outside
	std::string saved, previewFile, status;
	bool statusError = false;
	bool testDone = false;
};

#endif // !__SCENARIOBUILDER_H
