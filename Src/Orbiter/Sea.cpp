// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Sea.h"
#include "Planet.h"
#include "Astro.h"
#include "elevmgr.h"
#include "GroundScatter.h"
#include "ProcTerrain.h"
#include "Orbiter.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

extern Orbiter *g_pOrbiter;

namespace sea {

namespace {

	const double DATUM_TOL = 0.5;    // [m] terrain within this of the datum: the sea's surface (the data's)
	const size_t CACHE_MAX = 200000;

	void Trim (std::string &t)
	{
		size_t a = t.find_first_not_of (" \t\r\n"), b = t.find_last_not_of (" \t\r\n");
		t = a == std::string::npos ? "" : t.substr (a, b - a + 1);
	}

}

Config ReadConfig (const char *body)
{
	static std::map<std::string, Config> entries;
	static std::once_flag once;
	std::call_once (once, []() {
		std::ifstream ifs (g_pOrbiter ? g_pOrbiter->Cfg()->ConfigPathNoext ("Sea.cfg") : "Config/Sea.cfg");
		std::string line, section;
		while (std::getline (ifs, line)) {
			size_t c = line.find (';');
			if (c != std::string::npos) line.erase (c);
			Trim (line);
			if (line.empty ()) continue;
			if (line[0] == '[') {
				size_t e = line.find (']');
				section = line.substr (1, e == std::string::npos ? std::string::npos : e - 1);
				Trim (section);
				if (!section.empty()) entries[section].found = true;
				continue;
			}
			size_t eq = line.find ('=');
			if (eq == std::string::npos || section.empty()) continue;
			std::string key = line.substr (0, eq), val = line.substr (eq + 1);
			Trim (key); Trim (val);
			for (char &ch : key) ch = (char)tolower ((unsigned char)ch);
			Config &cf = entries[section];
			double v = atof (val.c_str());
			if (key == "wind") cf.wind = std::clamp (v, 0.0, 25.0);
			else if (key == "density") cf.density = std::max (v, 1.0);
			else if (key == "viscosity") cf.viscosity = std::max (v, 1e-9);
			else if (key == "floordepth") cf.floorDepth = std::max (v, 1.0);
			else if (key == "shelfslope") cf.shelfSlope = std::max (v, 1e-4);
			else if (key == "swellminwavelength") cf.lambdaMin = std::max (v, 0.5);
			else if (key == "seed") cf.seed = (uint32_t)strtoul (val.c_str(), nullptr, 10);
			else LOGOUT_WARN ("Config/Sea.cfg: unknown key %s in [%s]", key.c_str(), section.c_str());
		}
	});
	auto it = entries.find (body);
	return it == entries.end() ? Config() : it->second;
}

Sea::Sea (const Planet *p): planet (p)
{
	cfg = ReadConfig (p->Name());
	const char *off = getenv ("ORBITER_NO_WATER");
	if (off && *off && atoi (off)) cfg.found = false;
	active = cfg.found && p->HasSurface();
	if (!active) return;
	if (!cfg.seed) {
		uint32_t s = 0x5ea5eedu;
		for (const char *c = p->Name(); *c; c++) s = s * 31u + (uint8_t)*c;
		cfg.seed = s;
	}
	if (!p->ProcSurface()) cover = std::make_unique<scatter::Cover> (p);
	SwellParams sp;
	Wind (sp.wind, sp.bearing);
	double R = p->Size();
	sp.g = Ggrav * p->Mass() / (R * R);
	sp.radius = R;
	sp.seed = cfg.seed;
	sp.lambdaMin = cfg.lambdaMin;
	swell.Setup (sp);
	LOGOUT ("Sea of %s: density %.0f kg/m3, wind %.1f m/s towards %.0f deg, swell Hs %.2f m (of %.2f), Tp %.1f s, waves %.1f-%.1f m, crests below %.2f m, floor %.0f m",
		p->Name(), cfg.density, sp.wind, sp.bearing * 180.0 / Pi, swell.Hs(), swell.HsFull(), swell.Tp(), cfg.lambdaMin, swell.LambdaMax(), swell.CrestMax(), cfg.floorDepth);
}

Sea::~Sea () {}

void Sea::Wind (double &speed, double &bearing) const
{
	Vector w = planet->LayeredWind (10.0);
	double U = std::hypot (w.x, w.z);
	bearing = U > 1e-3 ? atan2 (w.x, w.z) : Pi05;
	speed = cfg.wind >= 0.0 ? cfg.wind : U;
	const char *e = getenv ("ORBITER_SEA_WIND"); // (tests: every body)
	if (e && *e) speed = atof (e);
	speed = std::clamp (speed, 0.0, 25.0);
}

bool Sea::MaskWater (double lat, double lng, bool &haveData)
{
	Vector rgb;
	double water = 0.0;
	haveData = cover && cover->Sample (lat, lng, rgb, water);
	if (!haveData) return false;
	if (water > 0.5) return true;
	if (!cover->HasMask (lat, lng)) {
		// (no mask there, e.g. Earth poleward of 56 deg: dark blue ground, as CityMap::Water)
		double lum = (rgb.x + rgb.y + rgb.z) / 3.0;
		return rgb.z > rgb.x * 1.15 && rgb.z >= rgb.y * 0.95 && lum < 0.3;
	}
	return false;
}

double Sea::ShoreDistance (double lat, double lng, double maxDist)
{
	stats.seabedSearches++;
	const double R = planet->Size();
	double clat = std::max (cos (lat), 1e-3);
	double prev = 0.0;
	for (double r = 25.0; ; r *= 2.0) {
		r = std::min (r, maxDist);
		int n = std::max (16, (int)(r / 100.0) * 4);
		for (int i = 0; i < n; i++) {
			double a = Pi2 * (i + 0.5 * (r > 30.0)) / n;
			double la = lat + r * cos (a) / R, lo = lng + r * sin (a) / (R * clat);
			bool have;
			if (!MaskWater (la, lo, have) && have) return 0.5 * (prev + r);
		}
		if (r >= maxDist) return maxDist;
		prev = r;
	}
}

Point Sea::At (double lat, double lng, bool wantSeabed)
{
	Point pt;
	if (!active) return pt;
	stats.queries++;
	ElevationManager *emgr = planet->ElevMgr();
	// (a deck over the water is ground)
	if (emgr && emgr->DeckElevation (lat, lng) > -1e9) return pt;
	const double q = 1.5e-6; // [rad] ~10 m on Earth
	uint64_t key = ((uint64_t)(uint32_t)(int32_t)floor (lat / q) << 32) | (uint32_t)(int32_t)floor (lng / q);
	auto it = cache.find (key);
	if (it != cache.end() && (!wantSeabed || it->second.hasSeabed || !it->second.water)) {
		stats.cacheHits++;
		pt.water = it->second.water, pt.datum = it->second.datum, pt.seabed = it->second.seabed;
		return pt;
	}
	if (cache.size() > CACHE_MAX) cache.clear();
	Cached c { false, false, 0.0, 0.0 };
	if (const ProcTerrain *proc = planet->ProcSurface()) {
		double slat = sin (lat), clat = cos (lat);
		Vector u (clat * cos (lng), slat, clat * sin (lng));
		ProcTerrain::Ground g = proc->GroundAt (u.x, u.y, u.z, planet->TidallyLocked());
		if (proc->Prm().cls == ProcTerrain::OCEAN) {
			// (open water: below the datum, not under the sea ice; the floes at its edge are water)
			c.water = g.seabed < 0.0 && g.edge < 0.0;
			c.datum = 0.0, c.seabed = g.seabed;
		} else if (proc->Prm().cls == ProcTerrain::ICE && proc->Prm().openWater > 0.0) {
			c.water = g.sea > 0.5;
			c.datum = emgr ? emgr->Elevation (lat, lng, 0, nullptr) : 0.0;
			c.seabed = c.datum - cfg.floorDepth;
		}
		c.hasSeabed = true;
	} else if (emgr) {
		double e = emgr->Elevation (lat, lng, 0, nullptr);
		if (fabs (e) <= DATUM_TOL) {
			bool have;
			c.water = MaskWater (lat, lng, have);
			// (no mask: the colour rule only where the data are at the datum exactly)
			if (c.water && !cover->HasMask (lat, lng) && fabs (e) > 0.01) c.water = false;
		}
		c.datum = 0.0;
		c.seabed = -cfg.floorDepth;
		if (c.water && wantSeabed) {
			double maxd = cfg.floorDepth / cfg.shelfSlope;
			c.seabed = -std::min (cfg.floorDepth, std::max (1.0, cfg.shelfSlope * ShoreDistance (lat, lng, maxd)));
			c.hasSeabed = true;
		}
	}
	cache[key] = c;
	pt.water = c.water, pt.datum = c.datum, pt.seabed = c.seabed;
	return pt;
}

} // namespace sea
