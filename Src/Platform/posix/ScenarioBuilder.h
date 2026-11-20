// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Scenario builder of the launchpad: a scenario made without editing
// files. The planetary system, the date, the ships (each with its class
// and where it starts: on a base's pad or runway, on a body's surface by
// longitude, latitude and heading or at a launch site, in orbit by its
// elements over the body's equator or so as to pass over a base a given
// time after the start, beside another ship, or docked to one), their
// fuel and landing gear, the ship in focus and the camera - written as a
// scenario file (Scenarios/Custom/<name>.scn), previewed behind the
// launchpad, launched. Stations (the ISS, Mir, ...) are added in their
// orbits, over the base of the ship selected: a launch to a rendezvous.
//
// What there is to choose from is read from the configuration files (the
// planetary systems, their bodies with their axes and rotation, the bases
// of their base directories with pads and runways, the vessel classes
// with their docking ports): no session is needed.
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
		double size = 0, mass = 0, obliquity = 0, lan = 0; // [m], [kg], [rad] (the axis against its precession's reference)
		// (its rotation and precession, as CelestialBody has them)
		double rotT = 86400, rotOff = 0, lanMjd = 51544.5, precT = 0, precLan = 0, precObl = 0, epochMjd = 51544.5;
		bool basesRead = false;
		std::vector<BaseRec> bases;
	};
	enum Where { AT_BASE, ON_SURFACE, IN_ORBIT, BESIDE, DOCKED, NWHERE };
	struct Ship {
		char name[40] = "";
		int cls = 0;
		int where = AT_BASE;
		int body = 0;          // (index into bodies)
		int base = 0, site = 0; // site: pads first (0..npad-1), then each runway's two ends
		double lng = 0, lat = 0, heading = 0; // [deg]
		double alt = 300, ecc = 0, inc = 0, lan = 0, argp = 0, anom = 0; // [km above the mean radius, at periapsis], [deg]
		int orbitBy = 0;       // in orbit: 0 by the elements, 1 over a base (the node and anomaly from it)
		int obase = 0;         // that base (of the body)
		double passMin = 10;   // [min] after the start it passes over it,
		int southbound = 0;    // heading north (0) or south (1)
		int beside = 0;        // (index into ships: the ship it is beside or docked to)
		double dist = 200;     // [m] behind it (in orbit), east of it (landed)
		int port = 0, tport = 0; // docked: its docking port, the other ship's
		double fuel = 100;     // [%] in all its tanks
		int gear = 0;          // landing gear: 0 as the start has it (down on the ground), 1 up, 2 down
		char extra[512] = "";  // further lines of its scenario block (as the class reads them)
	};

private:
	void ReadSystems ();
	void ReadSystem ();
	/// the body's axis at a date: its node and tilt on the ecliptic [rad], its rotation angle [rad]
	void Axis (const Body &b, double mjd, double &lan, double &obl, double &rot) const;
	/// an orbit's node and true anomaly [rad] at the start (by its elements, or over its base)
	bool OrbitAngles (const Ship &s, const Body &b, double &lan, double &anom) const;
	void AddStation (int k);
	int Docks (int cls) const;
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
	std::vector<int> classDocks; // (their docking ports)
	std::vector<std::string> systems;
	int sysSel = 0;
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
