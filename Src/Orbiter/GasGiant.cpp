// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GasGiant.h"
#include "OrbiterAPI.h"
#include "Util.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace {
	const double DEEP_LIMIT = -2000e3; // [m] below this the profile is held (far beyond any vessel)

	uint32_t Pcg (uint32_t v)
	{
		uint32_t state = v * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}
	double Unit16 (uint32_t h) { return (h & 0xffffu) / 65535.0; }
	double SmoothStep01 (double a, double b, double x)
	{
		double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// a keyword's value (case-insensitive key, ';' comments)
	bool Item (std::istream &is, const char *key, std::string &val)
	{
		is.clear(); is.seekg (0);
		std::string line, k(key);
		std::transform (k.begin(), k.end(), k.begin(), ::tolower);
		while (std::getline (is, line)) {
			size_t c = line.find (';');
			if (c != std::string::npos) line.erase (c);
			size_t e = line.find ('=');
			if (e == std::string::npos) continue;
			std::string a = line.substr (0, e);
			a.erase (0, a.find_first_not_of (" \t"));
			a.erase (a.find_last_not_of (" \t\r") + 1);
			std::transform (a.begin(), a.end(), a.begin(), ::tolower);
			if (a == k) { val = line.substr (e + 1); return true; }
		}
		return false;
	}
	void Real (std::istream &is, const char *key, double &v)
	{
		std::string s;
		if (Item (is, key, s)) { std::istringstream ss (s); double x; if (ss >> x) v = x; }
	}
}

bool GasGiant::Load (std::istream &cfg, double _R, double _gamma, double _g)
{
	std::string s;
	if (!Item (cfg, "GasGiant", s)) return false;
	s.erase (0, s.find_first_not_of (" \t"));
	if (s.empty() || (toupper (s[0]) != 'T' && s[0] != '1')) return false;
	R = _R, gamma = _gamma, g = _g;
	Real (cfg, "Size", radius);
	Real (cfg, "GGPressure1", P1);
	Real (cfg, "GGTemp1", T1);
	Real (cfg, "GGLapseRate", lapseDown);
	Real (cfg, "GGLapseRateUp", lapseUp);
	Real (cfg, "GGTropopauseTemp", Ttrop);
	Real (cfg, "GGCrushPressure", crushP);
	Real (cfg, "GGWindDecay", windDecay);
	Real (cfg, "GGWindDeep", windDeep);
	Real (cfg, "GGWindDeepAlt", windDeepAlt);
	Real (cfg, "GGCloudAlt", cloudAlt);
	Real (cfg, "GGTurbulence", turb);
	Real (cfg, "GGStormDensity", stormDensity);
	Real (cfg, "GGStormSize", stormSize);
	Real (cfg, "GGStormUpdraft", stormUpdraft);
	Real (cfg, "GGStormTurbulence", stormTurb);
	if (lapseDown <= 0.0) lapseDown = g * (gamma - 1.0) / (gamma * R); // (dry adiabat: g/cp)
	jetLat.clear(), jetU.clear();
	if (Item (cfg, "GGJets", s)) {
		std::istringstream ss (s);
		double la, u;
		while (ss >> la >> u) jetLat.push_back (la * RAD), jetU.push_back (u);
	}
	return true;
}

double GasGiant::Tz (double alt) const
{
	alt = std::max (alt, DEEP_LIMIT);
	if (alt <= 0.0) return T1 - lapseDown * alt;
	return std::max (Ttrop, T1 - lapseUp * alt);
}

void GasGiant::AtmParam (double alt, ATMPARAM *prm) const
{
	alt = std::max (alt, DEEP_LIMIT);
	double T = Tz (alt), p;
	if (alt <= 0.0) {
		// (polytrope: p ~ T^(g/(R lapse)))
		p = P1 * pow (T / T1, g / (R * lapseDown));
	} else {
		double ztrop = lapseUp > 0.0 ? (T1 - Ttrop) / lapseUp : 0.0;
		if (alt <= ztrop) p = P1 * pow (T / T1, g / (R * lapseUp));
		else {
			double ptrop = ztrop > 0.0 ? P1 * pow (Ttrop / T1, g / (R * lapseUp)) : P1;
			p = ptrop * exp (-(alt - ztrop) * g / (R * Ttrop));
		}
	}
	prm->T = T;
	prm->p = p;
	prm->rho = p / (R * T);
}

double GasGiant::Jet (double lat) const
{
	size_t n = jetLat.size();
	if (!n) return 0.0;
	if (lat <= jetLat[0]) return jetU[0];
	if (lat >= jetLat[n-1]) return jetU[n-1];
	size_t i = std::upper_bound (jetLat.begin(), jetLat.end(), lat) - jetLat.begin();
	double t = (lat - jetLat[i-1]) / (jetLat[i] - jetLat[i-1]);
	return jetU[i-1] + (jetU[i] - jetU[i-1]) * t;
}

GasGiant::Storm GasGiant::StormAt (double lng, double lat, double alt, double t) const
{
	Storm st{0.0, 0.0};
	if (stormDensity <= 0.0 || stormSize <= 0.0) return st;
	// cells in latitude bands, each band drifting with its jet
	double dlat = stormSize / radius;
	int j0 = (int)floor (lat / dlat);
	double best = 0.0, up = 0.0;
	for (int j = j0 - 1; j <= j0 + 1; j++) {
		double latc = (j + 0.5) * dlat;
		if (fabs (latc) >= Pi05) continue;
		double cl = cos (latc);
		int n = std::max (1, (int)floor (Pi2 * cl / dlat));
		double dl = Pi2 / n;
		double drift = fmod (Jet (latc) * t / (radius * cl), Pi2); // (the band's turn with its jet)
		double ld = lng - drift;
		ld -= Pi2 * floor (ld / Pi2);
		int i0 = (int)floor (ld / dl);
		for (int di = -1; di <= 1; di++) {
			int i = ((i0 + di) % n + n) % n;
			uint32_t h0 = Pcg ((uint32_t)i + Pcg ((uint32_t)(j + 100000) + Pcg (0x5717u)));
			if (Unit16 (h0) >= stormDensity) continue;
			uint32_t h1 = Pcg (h0), h2 = Pcg (h1), h3 = Pcg (h2);
			// centre, radius, life cycle
			double cy = latc + (Unit16 (h1) - 0.5) * 0.6 * dlat;
			double cx = (i + 0.2 + 0.6 * Unit16 (h1 >> 16)) * dl;
			double rad = (0.15 + 0.25 * Unit16 (h2)) * stormSize;
			double period = (6.0 + 24.0 * Unit16 (h2 >> 16)) * 3600.0;
			double phase = Unit16 (h3);
			double dx = ld - cx;
			dx -= Pi2 * floor (dx / Pi2 + 0.5);
			double ex = dx * cl * radius, ey = (lat - cy) * radius;
			double d2 = (ex * ex + ey * ey) / (rad * rad);
			if (d2 > 4.0) continue;
			double life = sin (Pi * (t / period + phase));
			life *= life;
			double s = life * exp (-d2);
			if (s > best) best = s, up = stormUpdraft * life * exp (-2.0 * d2);
		}
	}
	// (the updraft rises from the water clouds through the upper decks)
	double vert = SmoothStep01 (-150e3, -60e3, alt) * (1.0 - SmoothStep01 (0.0, 40e3, alt - cloudAlt));
	st.strength = best;
	st.updraft = up * vert;
	return st;
}

Vector GasGiant::Wind (double lng, double lat, double alt, double t) const
{
	double u = Jet (lat);
	if (alt > cloudAlt) u *= exp (-(alt - cloudAlt) / windDecay);
	else                u *= 1.0 + windDeep * SmoothStep01 (cloudAlt, windDeepAlt, alt);
	return Vector (u, StormAt (lng, lat, alt, t).updraft, 0.0);
}

double GasGiant::Turbulence (double lng, double lat, double alt, double t) const
{
	double vert = SmoothStep01 (-150e3, -60e3, alt) * (1.0 - SmoothStep01 (0.0, 40e3, alt - cloudAlt));
	return turb * exp (-std::max (0.0, alt - cloudAlt) / windDecay) + stormTurb * StormAt (lng, lat, alt, t).strength * vert;
}
