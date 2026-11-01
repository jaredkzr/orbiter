// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Named stars: the targets of interstellar travel (see StarTargets.h)
// =======================================================================

#include "StarTargets.h"
#include "Psys.h"
#include "Orbiter.h"
#include "Log.h"
#include "GalacticFrame.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

extern Orbiter *g_pOrbiter;
extern TimeData td;
extern PlanetarySystem *g_psys;

namespace startargets {

	namespace {
		const double LY_PER_PC = 3.2615638;          // light years a parsec
		const double PC_PER_KMS_YEAR = 1.0227122e-6; // 1 km/s for a year [pc]
		const double TSUN = 5772.0, MV_SUN = 4.83;
		const double GC_ARRIVAL = 1.0;               // [pc] arrival distance at the Galactic Centre

		// a CSV line's fields (quoted fields may hold commas)
		std::vector<std::string> Fields (const std::string &line)
		{
			std::vector<std::string> f (1);
			bool q = false;
			for (char c : line) {
				if (c == '"') q = !q;
				else if (c == ',' && !q) f.emplace_back ();
				else f.back() += c;
			}
			return f;
		}

		std::string Key (const char *s)
		{
			std::string k;
			for (; *s; s++) k += (*s == '_') ? ' ' : (char)std::tolower ((unsigned char)*s);
			return k;
		}

		// fraction of a black body's light in the V band (505-595 nm) at T [K]
		double VFraction (double T)
		{
			const double c2 = 1.438777e-2, sigma = 5.670374e-8, c1 = 3.741771852e-16; // (B_lambda pi: c1 / l^5 / (e^x - 1))
			double s = 0.0, dl = 90e-9 / 16.0;
			for (int k = 0; k < 16; k++) {
				double l = 505e-9 + dl * (k + 0.5);
				s += c1 / std::pow (l, 5.0) / std::expm1 (c2 / (l * T)) * dl;
			}
			return s / (sigma * T * T * T * T);
		}
	}

	bool Catalogue::Load (const char *namedCsv, const char *starsBin)
	{
		stars.clear ();
		// (the epoch of the positions: stars.bin's header)
		if (FILE *f = fopen (starsBin, "rb")) {
			char hdr[28];
			if (fread (hdr, 1, sizeof (hdr), f) == sizeof (hdr) && !memcmp (hdr, "OSTR", 4)) {
				float e;
				memcpy (&e, hdr + 24, 4);
				epoch = e;
			}
			fclose (f);
		}
		// the Galactic Centre of the sky's model
		Star gc;
		gc.name = "Galactic Centre";
		gc.designation = "Sgr A* (the model's centre)";
		gc.pos[0] = galactic::R0, gc.pos[2] = -galactic::ZSUN;
		stars.push_back (gc);

		std::ifstream ifs (namedCsv);
		if (!ifs) {
			LOGOUT_WARN ("Interstellar travel: %s not found: no named stars to travel to (Utils/SkyKit/build_names.py)", namedCsv);
			return false;
		}
		std::string line;
		bool header = false;
		while (std::getline (ifs, line)) {
			if (line.empty() || line[0] == '#') continue;
			if (!header) { header = true; continue; } // (the column names)
			auto f = Fields (line);
			if (f.size() < 14) continue;
			Star s;
			s.name = f[0], s.designation = f[1], s.con = f[2];
			s.index = std::atoll (f[3].c_str());
			s.id = std::strtoull (f[4].c_str(), nullptr, 10);
			s.hip = (uint32_t)std::atol (f[5].c_str());
			for (int k = 0; k < 3; k++) s.pos[k] = std::atof (f[6 + k].c_str()), s.vel[k] = std::atof (f[9 + k].c_str());
			s.mv = std::atof (f[12].c_str());
			s.teff = std::atof (f[13].c_str());
			if (s.teff <= 0.0) continue;
			stars.push_back (s);
		}
		LOGOUT ("Interstellar travel: %zu named stars (epoch %.1f)", stars.size() - 1, epoch);
		return true;
	}

	int Catalogue::Find (const char *name) const
	{
		std::string k = Key (name);
		for (size_t i = 0; i < stars.size(); i++)
			if (Key (stars[i].name.c_str()) == k) return (int)i;
		return -1;
	}

	void Catalogue::GalacticPos (int i, double simt, double p[3]) const
	{
		const Star &s = stars[i];
		double years = (td.MJD_ref + simt / 86400.0 - 51544.5) / 365.25 + 2000.0 - epoch;
		double d0 = std::sqrt (s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1] + s.pos[2] * s.pos[2]) * LY_PER_PC;
		for (int k = 0; k < 3; k++) p[k] = s.pos[k] + s.vel[k] * PC_PER_KMS_YEAR * (years + d0);
	}

	void Catalogue::State (int i, double simt, Vector &x, Vector &v) const
	{
		// (as its place relative to the global frame's origin when the origin
		// last moved, constant between the moves (its rounding too), and its
		// motion since: smooth in time near it. Its place at the epoch and
		// its motion since the epoch (light-time years: 1e13 m and more)
		// added each step would jitter by millimetres, its velocity by m/s;
		// their sum in heliocentric metres would be 8 m coarse at 2 pc)
		if (stars[i].follow >= 0 && stars[i].follow != i) { State (stars[i].follow, simt, x, v); return; }
		const Star &s = stars[i];
		double R[3][3];
		galactic::EclipticToGalactic (R);
		auto toEcl = [&](const double *g, double scale) {
			return Vector ((R[0][0] * g[0] + R[1][0] * g[1] + R[2][0] * g[2]) * scale,
			               (R[0][1] * g[0] + R[1][1] * g[1] + R[2][1] * g[2]) * scale,
			               (R[0][2] * g[0] + R[1][2] * g[1] + R[2][2] * g[2]) * scale);
		};
		double t0 = g_psys ? g_psys->OriginTime () : 0.0;
		double years0 = (td.MJD_ref - 51544.5) / 365.25 + 2000.0 - epoch + t0 / (365.25 * 86400.0);
		double d0 = std::sqrt (s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1] + s.pos[2] * s.pos[2]) * LY_PER_PC;
		v = toEcl (s.vel, 1e3);
		x = toEcl (s.pos, galactic::PARSEC);
		if (g_psys) x -= g_psys->Origin ();
		x += v * ((years0 + d0) * 365.25 * 86400.0);
		x += v * (simt - t0);
	}

	double Catalogue::Luminosity (int i) const
	{
		const Star &s = stars[i];
		if (!s.IsStar ()) return 0.0;
		double lv = std::pow (10.0, -0.4 * (s.mv - MV_SUN));
		return lv * VFraction (TSUN) / VFraction (s.teff);
	}

	double Catalogue::Radius (int i) const
	{
		const Star &s = stars[i];
		if (!s.IsStar ()) return 0.0;
		const double RSUN = 6.957e8;
		return RSUN * std::sqrt (Luminosity (i)) * (TSUN / s.teff) * (TSUN / s.teff);
	}

	double Catalogue::ArrivalDistance (int i) const
	{
		if (!stars[i].IsStar ()) return GC_ARRIVAL * galactic::PARSEC;
		return std::clamp (std::sqrt (Luminosity (i)), 0.02, 1e4) * AU;
	}

	int Catalogue::Add (const Star &s)
	{
		int i = Find (s.name.c_str ());
		if (i > 0) { stars[i] = s; return i; }
		stars.push_back (s);
		return (int)stars.size () - 1;
	}

	double Catalogue::AbsMagV (double lum, double teff)
	{
		if (lum <= 0.0 || teff <= 0.0) return 99.0;
		return MV_SUN - 2.5 * std::log10 (lum * VFraction (teff) / VFraction (TSUN));
	}

	int AddEntry (const Star &s)
	{
		return const_cast<Catalogue&> (Get ()).Add (s);
	}

	const Catalogue &Get ()
	{
		static Catalogue cat;
		static bool loaded = false;
		if (!loaded) {
			cat.Load ("Config/CSphere/Sky/stars_named.csv", "Config/CSphere/Sky/stars.bin");
			loaded = true;
		}
		return cat;
	}

} // namespace startargets
