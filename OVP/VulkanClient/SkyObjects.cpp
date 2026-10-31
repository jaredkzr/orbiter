// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SkyObjects.h"
#include "GalaxySky.h"
#include "GalacticFrame.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {
	constexpr int FLOATS = 18 * 4;   // per object (as SkyObject in skyobjects.glsl)
	constexpr int MAX_COMP = 4;
	constexpr double PI_ = 3.14159265358979323846;
	constexpr double D2R = PI_ / 180.0;
	enum { DISK = 1, BULGE = 2, BAR = 3, KNOT = 4, PLUMMER = 5 };
	const double ARMS_MEAN = 35.0 / 128.0; // mean of ((1 + cos) / 2)^4: the arms' share of a disk's light per contrast

	double Dot (const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	// A galaxy's frame (as Utils/SkyKit/skykit/galaxies.py): its centre; the
	// major axis a = cos PA N + sin PA E on the sky; the near side s = sin PA
	// N - cos PA E; the disk's second axis b = cos i s - sin i n (n: the line
	// of sight); its normal z = a x b
	struct Frame {
		double c[3], n[3], a[3], b[3], z[3], s[3], cosi;
		void Set (double ra, double dec, double dist, double incl, double pa)
		{
			galactic::EquatorialToGalactic (ra, dec, n);
			double north[3], east[3], en[3] = { -std::sin (dec * D2R) * std::cos (ra * D2R), -std::sin (dec * D2R) * std::sin (ra * D2R), std::cos (dec * D2R) };
			double ee[3] = { -std::sin (ra * D2R), std::cos (ra * D2R), 0.0 };
			for (int i = 0; i < 3; i++) {
				north[i] = east[i] = 0.0;
				for (int j = 0; j < 3; j++)
					north[i] += galactic::ICRS_TO_GALACTIC[i][j] * en[j], east[i] += galactic::ICRS_TO_GALACTIC[i][j] * ee[j];
			}
			double cp = std::cos (pa * D2R), sp = std::sin (pa * D2R), ci = std::cos (incl * D2R), si = std::sin (incl * D2R);
			for (int i = 0; i < 3; i++) {
				c[i] = n[i] * dist;
				a[i] = cp * north[i] + sp * east[i];
				s[i] = sp * north[i] - cp * east[i];
				b[i] = ci * s[i] - si * n[i];
			}
			z[0] = a[1] * b[2] - a[2] * b[1], z[1] = a[2] * b[0] - a[0] * b[2], z[2] = a[0] * b[1] - a[1] * b[0];
			cosi = std::max (std::fabs (ci), 0.05);
		}
		// where the line of sight to (ra, dec) meets the disk's plane, in the
		// frame (x, y, z)
		void InPlane (double ra, double dec, double out[3]) const
		{
			double u[3];
			galactic::EquatorialToGalactic (ra, dec, u);
			double t = Dot (c, z) / Dot (u, z), q[3];
			for (int i = 0; i < 3; i++) q[i] = u[i] * t - c[i];
			out[0] = Dot (q, a), out[1] = Dot (q, b), out[2] = Dot (q, z);
		}
		// the angle in the disk's plane (from a toward b) of the direction at
		// position angle pa on the sky
		double PlaneAngle (double pa) const
		{
			// (on the sky: a and s; s is b foreshortened by cos i)
			double va = std::cos ((pa - Pa ()) * D2R), vs = -std::sin ((pa - Pa ()) * D2R);
			return std::atan2 (vs / cosi, va);
		}
		double pa_ = 0.0;
		double Pa () const { return pa_; }
	};
}

bool SkyObjects::LoadGalaxies (const std::string &path, const GalaxySky &sky, std::vector<float> &out, int &count)
{
	std::ifstream f (path);
	if (!f) return false;
	std::string line;
	struct Comp { int type; double p[8]; };
	std::vector<Comp> comps;
	double dust[8] = {};
	double ra = 0, dec = 0, dist = 0, mv = 0, incl = 0, pa = 0;
	std::string name;
	bool open = false;
	while (std::getline (f, line)) {
		size_t sc = line.find (';');
		if (sc != std::string::npos) line.resize (sc);
		std::istringstream is (line);
		std::string key;
		if (!(is >> key)) continue;
		if (key == "GALAXY") {
			is >> name >> ra >> dec >> dist >> mv >> incl >> pa;
			comps.clear ();
			std::fill (dust, dust + 8, 0.0);
			open = true;
		}
		else if (!open) continue;
		else if (key == "DISK" || key == "BULGE" || key == "BAR" || key == "KNOT") {
			Comp c{};
			c.type = key == "DISK" ? DISK : key == "BULGE" ? BULGE : key == "BAR" ? BAR : KNOT;
			for (double &v : c.p) if (!(is >> v)) { v = 0.0; break; }
			if ((int)comps.size() < MAX_COMP) comps.push_back (c);
		}
		else if (key == "DUST") for (int k = 0; k < 5; k++) { if (!(is >> dust[k])) { dust[k] = 0.0; break; } }
		else if (key == "END") {
			open = false;
			Frame fr;
			fr.pa_ = pa;
			fr.Set (ra, dec, dist, incl, pa);
			float o[FLOATS] = {};
			for (int i = 0; i < 3; i++) {
				o[i] = (float)fr.c[i];
				o[4 + i] = (float)fr.a[i], o[8 + i] = (float)fr.b[i], o[12 + i] = (float)fr.z[i];
			}
			o[4 + 3] = 0.0f; // (kind: galaxy)
			o[8 + 3] = (float)comps.size();
			double LV = std::pow (10.0, -0.4 * (mv - 4.83)), shares = 0.0;
			for (auto &c : comps) shares += c.type == BAR ? c.p[6] : c.type == KNOT ? c.p[3] : c.p[2];
			double R = 0.0, Z = 0.0; // (the extent: bounding sphere, slab about the disk's plane)
			double armsN = 0.0, armsTan = 0.0, armsC = 0.0;
			for (size_t k = 0; k < comps.size(); k++) {
				const Comp &c = comps[k];
				float *g0 = o + 24 + k * 12, *g1 = g0 + 4, *lt = g0 + 8;
				double share, T, volume;
				g0[0] = (float)c.type;
				switch (c.type) {
				case DISK: // scale length, height, share, T, [arms n, pitch, contrast]
					share = c.p[2], T = c.p[3];
					g0[1] = (float)c.p[0], g0[2] = (float)c.p[1], g0[3] = c.p[4] > 0.0 ? 1.0f : 0.0f;
					if (c.p[4] > 0.0) armsN = c.p[4], armsTan = std::tan (c.p[5] * D2R), armsC = c.p[6];
					volume = 4.0 * PI_ * c.p[0] * c.p[0] * c.p[1] * (1.0 + (c.p[4] > 0.0 ? c.p[6] * ARMS_MEAN : 0.0));
					R = std::max (R, 9.0 * c.p[0]), Z = std::max (Z, 8.0 * c.p[1]);
					break;
				case BULGE: // scale, height / radius, share, T
					share = c.p[2], T = c.p[3];
					g0[1] = (float)c.p[0], g0[2] = (float)c.p[1];
					volume = 8.0 * PI_ * c.p[0] * c.p[0] * c.p[0] * c.p[1];
					R = std::max (R, 8.0 * c.p[0] * std::max (1.0, c.p[1])), Z = std::max (Z, 8.0 * c.p[0] * c.p[1]);
					break;
				case BAR: { // ra, dec, sigmas a, b, c, sky PA, share, T
					share = c.p[6], T = c.p[7];
					double off[3];
					fr.InPlane (c.p[0], c.p[1], off);
					g0[1] = (float)c.p[2], g0[2] = (float)c.p[3], g0[3] = (float)c.p[4];
					for (int i = 0; i < 3; i++) g1[i] = (float)off[i];
					g1[3] = (float)fr.PlaneAngle (c.p[5]);
					volume = std::pow (2.0 * PI_, 1.5) * c.p[2] * c.p[3] * c.p[4];
					R = std::max (R, std::sqrt (Dot (off, off)) + 4.0 * c.p[2]), Z = std::max (Z, 4.0 * c.p[4]);
					break; }
				default: { // KNOT: ra, dec, sigma, share, T
					share = c.p[3], T = c.p[4];
					double off[3];
					fr.InPlane (c.p[0], c.p[1], off);
					g0[1] = (float)c.p[2];
					for (int i = 0; i < 3; i++) g1[i] = (float)off[i];
					volume = std::pow (2.0 * PI_, 1.5) * c.p[2] * c.p[2] * c.p[2];
					R = std::max (R, std::sqrt (Dot (off, off)) + 4.0 * c.p[2]), Z = std::max (Z, 4.0 * c.p[2]);
					break; }
				}
				// (light per band at the centre [L_sun,band / pc^3])
				float bl[GalaxySky::NBAND];
				sky.BandLight (T, bl);
				double j = LV * (share / shares) / volume;
				for (int b = 0; b < GalaxySky::NBAND; b++) lt[b] = (float)(j * bl[b]);
			}
			o[3] = (float)R, o[12 + 3] = (float)Z;
			for (int k = 0; k < 4; k++) o[16 + k] = (float)dust[k];
			o[20] = (float)dust[4], o[21] = (float)armsN, o[22] = (float)armsTan, o[23] = (float)armsC;
			out.insert (out.end(), o, o + FLOATS);
			count++;
		}
	}
	return true;
}

bool SkyObjects::LoadClusters (const std::string &path, const GalaxySky &sky, std::vector<float> &out, int &count)
{
	std::ifstream f (path);
	if (!f) return false;
	// V light of the Sun's old population per unit of its density [L_sun / pc^3]
	double lvOld = 0.0;
	for (const auto &r : sky.LFRows ())
		if (r.pop == 2) lvOld += std::pow (10.0, r.logn) * std::pow (10.0, -0.4 * (r.M - 4.83));
	if (lvOld <= 0.0) return false;
	std::string line;
	while (std::getline (f, line)) {
		if (line.empty () || line[0] == '#') continue;
		std::istringstream is (line);
		std::string name, field;
		std::getline (is, name, ',');
		double v[7] = {};
		int n = 0;
		while (n < 7 && std::getline (is, field, ',')) v[n++] = field.empty () ? 0.0 : atof (field.c_str ());
		if (n < 6) continue;
		Cluster c;
		c.name = name;
		for (int k = 0; k < 3; k++) c.pos[k] = v[k];
		c.a = v[4], c.rt = v[5];
		c.volume = std::pow (10.0, -0.4 * (v[3] - 4.83)) / lvOld;
		c.seed = (uint32_t)(0x9E3779B9u * (clusters.size() + 1));
		clusters.push_back (c);
		float o[FLOATS] = {};
		for (int k = 0; k < 3; k++) o[k] = (float)c.pos[k];
		o[3] = (float)c.rt;
		o[4] = 1.0f, o[4 + 3] = 1.0f; // (kind: cluster)
		o[8 + 3] = 1.0f;
		o[24] = (float)PLUMMER, o[25] = (float)c.a, o[26] = (float)c.rt, o[27] = (float)c.volume;
		out.insert (out.end(), o, o + FLOATS);
		count++;
	}
	return true;
}

void SkyObjects::Init (vkc::Context *c, const GalaxySky &sky)
{
	ctx = c;
	std::vector<float> data (4, 0.0f); // (header: count)
	int nGal = 0, nCl = 0;
	if (!sky.Settings ().galaxies) data.resize (4 + FLOATS, 0.0f); // (none)
	if (data.size() == 4) {
		LoadGalaxies ("Config/CSphere/Sky/galaxies.cfg", sky, data, nGal);
		LoadClusters ("Config/CSphere/Sky/globulars.csv", sky, data, nCl);
		if (data.size() == 4) data.resize (4 + FLOATS, 0.0f);
	}
	uint32_t n = (uint32_t)(nGal + nCl);
	memcpy (data.data(), &n, 4);
	buffer = ctx->CreateDeviceBuffer (data.data(), data.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	oapiWriteLogV ("VulkanClient: sky objects: %d galaxies, %d globular clusters", nGal, nCl);
}

void SkyObjects::Shutdown ()
{
	if (!ctx) return;
	ctx->DestroyBuffer (buffer);
	clusters.clear ();
	ctx = nullptr;
}
