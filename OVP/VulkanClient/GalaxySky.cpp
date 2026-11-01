// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GalaxySky.h"
#include "ImageIO.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "sky_bake.comp.h"
#include "milkyway.vert.h"
#include "milkyway.frag.h"
#include "cosmicbg.frag.h"

namespace {
	constexpr int MAX_DISKS = 4, MAX_ARMS = 12, NPOP = 3, NLF = 257, NLEVEL = 3;
	constexpr float LF_M0 = -12.0f, LF_DM = 0.125f;
	constexpr double PI_ = 3.14159265358979323846;
	constexpr double TSUN = 5772.0;      // [K]
	constexpr double MV_SUN = 4.83;
	constexpr double REBAKE_DIST = 0.5;  // observer movement that starts a new bake [pc]
	constexpr float FADE_TIME = 0.75f;   // [s]
	const char *POP_NAMES[NPOP] = { "young", "thin", "old" };

	// Planck spectral radiance (arbitrary scale), lambda [nm]
	double Planck (double lambda, double T)
	{
		double x = 1.438777e7 / (lambda * T);
		if (x > 700.0) return 0.0;
		return 1.0 / (std::pow (lambda * 1e-3, 5.0) * (std::exp (x) - 1.0));
	}

	double BandPower (double lo, double hi, double T)
	{
		double s = 0.0;
		for (double l = lo + 0.5; l < hi; l += 1.0) s += Planck (l, T);
		return s;
	}

	// CIE 1931 2-degree colour matching functions, multi-lobe fit of Wyman,
	// Sloan & Shirley (2013)
	double Lobe (double l, double mu, double s1, double s2)
	{
		double t = (l - mu) / (l < mu ? s1 : s2);
		return std::exp (-0.5 * t * t);
	}
	void CMF (double l, double xyz[3])
	{
		xyz[0] = 1.056 * Lobe (l, 599.8, 37.9, 31.0) + 0.362 * Lobe (l, 442.0, 16.0, 26.7) - 0.065 * Lobe (l, 501.1, 20.4, 26.2);
		xyz[1] = 0.821 * Lobe (l, 568.8, 46.9, 40.5) + 0.286 * Lobe (l, 530.9, 16.3, 31.1);
		xyz[2] = 1.217 * Lobe (l, 437.0, 11.8, 36.0) + 0.681 * Lobe (l, 459.0, 26.0, 13.8);
	}

	float HalfToFloat (uint16_t h)
	{
		uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
		uint32_t f;
		if (e == 0) {
			if (!m) f = s;
			else { // subnormal
				e = 127 - 14;
				while (!(m & 0x400)) m <<= 1, e--;
				f = s | (e << 23) | ((m & 0x3ff) << 13);
			}
		} else if (e == 31) f = s | 0x7f800000u | (m << 13);
		else f = s | ((e + 112) << 23) | (m << 13);
		float r;
		memcpy (&r, &f, 4);
		return r;
	}

	// Vulkan cube face and coordinates [-1,1] of a direction
	void CubeFace (const double d[3], int &face, double &u, double &v)
	{
		double ax = std::fabs (d[0]), ay = std::fabs (d[1]), az = std::fabs (d[2]);
		double sc, tc, ma;
		if (ax >= ay && ax >= az) { face = d[0] > 0 ? 0 : 1; ma = ax; sc = d[0] > 0 ? -d[2] : d[2]; tc = -d[1]; }
		else if (ay >= az) { face = d[1] > 0 ? 2 : 3; ma = ay; sc = d[0]; tc = d[1] > 0 ? d[2] : -d[2]; }
		else { face = d[2] > 0 ? 4 : 5; ma = az; sc = d[2] > 0 ? d[0] : -d[0]; tc = -d[1]; }
		u = sc / ma, v = tc / ma;
	}
}

// (must match GalaxyModel in galaxy.glsl)
struct GalaxySky::Model {
	float geom[4], halo[4], bar[4], barAxis[4], dust[4], dust2[4], extinction[4], misc[4], neb[3][4];
	struct { float a[4], b[4]; } disks[MAX_DISKS];
	struct { float a[4], b[4]; } arms[MAX_ARMS];
	float levelOrigin[NLEVEL][4], levelSize[NLEVEL][4];
	float lf[NPOP * NLF][4];
};

namespace {
	// Spiral arm profile (as GalArms in galaxy.glsl)
	double Smoothstep (double e0, double e1, double x)
	{
		double t = std::clamp ((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	double ArmProfile (const float (*arms)[2][4], int n, double growth, double R, double beta, bool young)
	{
		double best = 0.0;
		const double FADE = 0.349;
		for (int i = 0; i < n; i++) {
			const float *a = arms[i][0], *b = arms[i][1];
			double wgt = young ? 1.0 : b[3];
			if (wgt <= 0.0) continue;
			for (int k = -1; k <= 1; k++) {
				double be = beta + 2.0 * PI_ * k;
				if (be < b[1] - FADE || be > b[2] + FADE) continue;
				double tp = be <= a[0] ? a[2] : a[3];
				double Ra = std::exp (a[1] - (be - a[0]) * tp);
				if (Ra < 2500.0 || Ra > 18000.0) continue;
				double fade = std::clamp ((be - b[1]) / FADE + 1.0, 0.0, 1.0) * std::clamp ((b[2] - be) / FADE + 1.0, 0.0, 1.0);
				fade *= Smoothstep (2500.0, 3500.0, Ra) * (1.0 - Smoothstep (16000.0, 18000.0, Ra));
				double w = b[0] + growth * (Ra - std::exp (a[1]));
				double d = (R - Ra) / std::sqrt (1.0 + tp * tp);
				best = std::max (best, wgt * fade * std::exp (-0.5 * d * d / (w * w)));
			}
		}
		return best;
	}

	// (as Vertical in galaxy.glsl)
	double Vertical (double z, double h)
	{
		const double C = 0.3;
		return std::exp (C - std::sqrt (z * z / (h * h) + C * C));
	}
}

bool GalaxySky::LoadModel (const std::string &path, std::vector<float> &buf)
{
	std::ifstream f (path);
	if (!f) return false;
	Model m{};
	using Row = LFRow;
	std::vector<Row> &lf = lfRows;
	lf.clear ();
	double bands[NBAND][2] = { { 380, 460 }, { 460, 540 }, { 540, 620 }, { 620, 720 } };
	double R0 = galactic::R0, zSun = galactic::ZSUN, edge = 13000.0, edgeScale = 1500.0, growth = 0.042;
	struct Disk { int pop; double hR, hz, frac, armC, hole; };
	std::vector<Disk> disks;
	int nArm = 0;
	float arms[MAX_ARMS][2][4] = {};
	double halo[4] = { 1000, 2.5, 0.7, 0.0 }; int haloPop = 2;
	double bar[5] = { 1490, 580, 400, 27, 0 }; int barPop = 2;
	double dust[5] = { 0.0009, 3000, 110, 3.0, 2500 }, dustMap[2] = { 1700, 1990 };
	double ext[NBAND] = { 1.40, 1.12, 0.94, 0.80 };
	auto popIndex = [](const std::string &s) {
		for (int i = 0; i < NPOP; i++) if (s == POP_NAMES[i]) return i;
		return -1;
	};

	std::string line;
	while (std::getline (f, line)) {
		size_t c = line.find (';');
		if (c != std::string::npos) line.resize (c);
		std::istringstream is (line);
		std::string key;
		if (!(is >> key)) continue;
		if (key == "R0") is >> R0;
		else if (key == "ZSUN") is >> zSun;
		else if (key == "EDGE") is >> edge >> edgeScale;
		else if (key == "DISK") {
			std::string name, pop;
			Disk d;
			is >> name >> pop >> d.hR >> d.hz >> d.frac >> d.armC >> d.hole;
			d.pop = popIndex (pop);
			if (d.pop >= 0 && disks.size() < MAX_DISKS) disks.push_back (d);
		}
		else if (key == "HALO") {
			std::string name, pop;
			is >> name >> pop >> halo[0] >> halo[1] >> halo[2] >> halo[3];
			haloPop = std::max (0, popIndex (pop));
		}
		else if (key == "BAR") {
			std::string name, pop;
			is >> name >> pop >> bar[0] >> bar[1] >> bar[2] >> bar[3] >> bar[4];
			barPop = std::max (0, popIndex (pop));
		}
		else if (key == "ARM" && nArm < MAX_ARMS) {
			std::string name;
			double bk, Rk, p1, p2, w, b0, b1, st;
			if (is >> name >> bk >> Rk >> p1 >> p2 >> w >> b0 >> b1 >> st) {
				float *a = arms[nArm][0], *b = arms[nArm][1];
				a[0] = (float)(bk * PI_ / 180.0), a[1] = (float)std::log (Rk);
				a[2] = (float)std::tan (p1 * PI_ / 180.0), a[3] = (float)std::tan (p2 * PI_ / 180.0);
				b[0] = (float)w, b[1] = (float)(b0 * PI_ / 180.0), b[2] = (float)(b1 * PI_ / 180.0), b[3] = (float)st;
				nArm++;
			}
		}
		else if (key == "ARM_WIDTH_GROWTH") { is >> growth; growth *= 1e-3; }
		else if (key == "DUST") is >> dust[0] >> dust[1] >> dust[2] >> dust[3] >> dust[4];
		else if (key == "DUST_MAP") is >> dustMap[0] >> dustMap[1];
		else if (key == "EXTINCTION") for (int b = 0; b < NBAND; b++) is >> ext[b];
		else if (key == "BANDS") for (int b = 0; b < NBAND; b++) is >> bands[b][0] >> bands[b][1];
		else if (key == "NEB_GAS") is >> neb.gasPerDust >> neb.minGas >> neb.maxRadius;
		else if (key == "NEB_CLUMP") is >> neb.clumpSigma >> neb.clumpWavelength;
		else if (key == "NEB_DUST") is >> neb.albedo >> neb.g >> neb.localRadius;
		else if (key == "NEB_LIGHT") is >> neb.lightMV >> neb.reachMag;
		else if (key == "NEB_CLOUD") is >> neb.coreDensity >> neb.coreShare >> neb.envDensity >> neb.envShare;
		else if (key == "LF") {
			std::string pop, branch;
			Row r;
			if (is >> pop >> branch >> r.M >> r.logn >> r.T && (r.pop = popIndex (pop)) >= 0) lf.push_back (r);
		}
	}

	// geometry
	m.geom[0] = (float)R0, m.geom[1] = (float)zSun, m.geom[2] = (float)edge, m.geom[3] = (float)edgeScale;
	m.misc[0] = (float)nArm, m.misc[1] = (float)growth, m.misc[2] = (float)haloPop;
	memcpy (m.arms, arms, sizeof(arms));
	auto armAtSun = [&](bool young) { return ArmProfile (arms, nArm, growth, R0, 0.0, young); };
	auto hole = [](double R, double h) { return h > 0.0 ? 1.0 - std::exp (-std::pow (R / h, 4.0)) : 1.0; };

	// disks: normalised to their local fraction at the Sun
	for (size_t i = 0; i < disks.size(); i++) {
		const Disk &d = disks[i];
		bool young = d.pop == 0;
		double armN = 1.0 + d.armC * armAtSun (young);
		double atSun = hole (R0, d.hole) * Vertical (zSun, d.hz);
		m.disks[i].a[0] = (float)d.hR, m.disks[i].a[1] = (float)d.hz;
		m.disks[i].a[2] = (float)(d.frac / atSun), m.disks[i].a[3] = (float)d.armC;
		m.disks[i].b[0] = (float)d.hole, m.disks[i].b[1] = (float)d.pop, m.disks[i].b[2] = (float)armN;
	}
	m.barAxis[3] = (float)disks.size();
	for (int k = 0; k < 4; k++) m.halo[k] = (float)halo[k];
	for (int k = 0; k < 3; k++) m.bar[k] = (float)bar[k];
	m.bar[3] = (float)bar[4];
	m.barAxis[0] = (float)std::cos (bar[3] * PI_ / 180.0), m.barAxis[1] = (float)std::sin (bar[3] * PI_ / 180.0);
	m.barAxis[2] = (float)barPop;
	// dust: mid-plane value at the Sun's radius
	m.dust[0] = (float)(dust[0] / hole (R0, dust[4])), m.dust[1] = (float)dust[1], m.dust[2] = (float)dust[2], m.dust[3] = (float)dust[3];
	m.dust2[0] = (float)dust[4], m.dust2[1] = (float)dustMap[0], m.dust2[2] = (float)dustMap[1];
	m.dust2[3] = (float)(1.0 + dust[3] * armAtSun (true));
	for (int b = 0; b < NBAND; b++) m.extinction[b] = (float)ext[b];
	m.neb[0][0] = (float)neb.gasPerDust, m.neb[0][1] = (float)neb.minGas, m.neb[0][2] = (float)neb.maxRadius;
	m.neb[0][3] = (float)neb.clumpSigma;
	m.neb[1][0] = (float)neb.albedo, m.neb[1][1] = (float)neb.g, m.neb[1][2] = (float)neb.clumpWavelength;
	m.neb[2][0] = (float)neb.coreDensity, m.neb[2][1] = (float)neb.coreShare;
	m.neb[2][2] = (float)neb.envDensity, m.neb[2][3] = (float)neb.envShare;

	// unresolved light of each population: stars fainter than M, per band,
	// in units of the Sun's luminosity in the band
	double vSun = BandPower (505, 595, TSUN), bSun[NBAND];
	for (int b = 0; b < NBAND; b++) bSun[b] = BandPower (bands[b][0], bands[b][1], TSUN);
	for (const Row &r : lf) {
		double n = std::pow (10.0, r.logn);
		double LV = std::pow (10.0, -0.4 * (r.M - MV_SUN));
		double vT = BandPower (505, 595, r.T);
		double Lb[NBAND];
		for (int b = 0; b < NBAND; b++) Lb[b] = LV * (BandPower (bands[b][0], bands[b][1], r.T) / vT) / (bSun[b] / vSun);
		for (int i = 0; i < NLF; i++) {
			double Mc = LF_M0 + i * LF_DM;
			double frac = std::clamp (r.M + 0.5 - Mc, 0.0, 1.0); // of the 1-mag bin fainter than Mc
			for (int b = 0; b < NBAND; b++) m.lf[r.pop * NLF + i][b] += (float)(n * Lb[b] * frac);
		}
	}

	// band colours: the spectrum within a band taken as the Sun's; white
	// balance to the Sun (a solar spectrum of 1 S10 in every band: RGB 1)
	const double XYZ2RGB[3][3] = { { 3.2406, -1.5372, -0.4986 }, { -0.9689, 1.8758, 0.0415 }, { 0.0557, -0.2040, 1.0570 } };
	double rgb[NBAND][3], sum[3] = { 0, 0, 0 };
	for (int b = 0; b < NBAND; b++) {
		double xyz[3] = { 0, 0, 0 }, c[3];
		for (double l = bands[b][0] + 0.5; l < bands[b][1]; l += 1.0) {
			CMF (l, c);
			double p = Planck (l, TSUN);
			for (int k = 0; k < 3; k++) xyz[k] += c[k] * p;
		}
		for (int k = 0; k < 3; k++) {
			rgb[b][k] = XYZ2RGB[k][0] * xyz[0] + XYZ2RGB[k][1] * xyz[1] + XYZ2RGB[k][2] * xyz[2];
			sum[k] += rgb[b][k];
		}
	}
	for (int k = 0; k < 3; k++)
		for (int b = 0; b < NBAND; b++) bandColour[k][b] = (float)(rgb[b][k] / sum[k]);
	// a camera's colours (the enhanced display): red, green and blue
	// filters, all positive (the eye's, above, take light at 460-540 nm off
	// the red: a nebula's [O III] and H-beta cancel much of its H-alpha,
	// as it looks to the eye; a photograph shows it red), white balanced
	// to the Sun
	{
		const double F[3][2] = { { 615, 40 }, { 535, 35 }, { 460, 30 } }; // [nm]: centre, sigma
		for (int k = 0; k < 3; k++) {
			double r[NBAND], s = 0.0;
			for (int b = 0; b < NBAND; b++) {
				r[b] = 0.0;
				for (double l = bands[b][0] + 0.5; l < bands[b][1]; l += 1.0)
					r[b] += std::exp (-0.5 * (l - F[k][0]) * (l - F[k][0]) / (F[k][1] * F[k][1])) * Planck (l, TSUN);
				s += r[b];
			}
			for (int b = 0; b < NBAND; b++) cameraColour[k][b] = (float)(r[b] / s);
		}
	}

	memcpy (this->bands, bands, sizeof(bands));
	// (the relativistic view's spectral shift has them built in: relsky.glsl)
	{
		const double REL_BANDS[NBAND][2] = { { 380, 460 }, { 460, 540 }, { 540, 620 }, { 620, 720 } };
		for (int b = 0; b < NBAND; b++)
			if (bands[b][0] != REL_BANDS[b][0] || bands[b][1] != REL_BANDS[b][1]) {
				oapiWriteLog ((char*)"VulkanClient: galaxy.cfg BANDS differ from Shaders/relsky.glsl's: the relativistic view's colours are off");
				break;
			}
	}
	for (int b = 0; b < NBAND; b++) extinction[b] = (float)ext[b];

	buf.resize (sizeof(Model) / sizeof(float));
	memcpy (buf.data(), &m, sizeof(Model));
	oapiWriteLogV ("VulkanClient: galaxy model: %zu disks, %d arms, %zu luminosity function rows", disks.size(), nArm, lf.size());
	return true;
}

void GalaxySky::BandLight (double T, float out[NBAND]) const
{
	double vSun = BandPower (505, 595, TSUN), vT = BandPower (505, 595, T);
	for (int b = 0; b < NBAND; b++)
		out[b] = (float)((BandPower (bands[b][0], bands[b][1], T) / vT) / (BandPower (bands[b][0], bands[b][1], TSUN) / vSun));
}

double GalaxySky::BolometricRatio (double T) const
{
	// (the whole black body spectrum: 50 nm to 100 um, in 0.5% steps)
	auto bol = [](double T) {
		double s = 0.0;
		for (double l = 50.0; l < 1e5; l *= 1.005) s += Planck (l, T) * l * 0.005;
		return s;
	};
	return (bol (T) / BandPower (505, 595, T)) / (bol (TSUN) / BandPower (505, 595, TSUN));
}

double GalaxySky::SolarBandFraction (int b) const
{
	double s = 0.0;
	for (double l = 50.0; l < 1e5; l *= 1.005) s += Planck (l, TSUN) * l * 0.005;
	return BandPower (bands[b][0], bands[b][1], TSUN) / s;
}

bool GalaxySky::LoadDust (const std::string &path)
{
	std::ifstream f (path, std::ios::binary);
	if (!f) return false;
	char magic[4];
	uint32_t version, nlevel;
	float logMin, logMax, coverage;
	f.read (magic, 4);
	f.read ((char*)&version, 4);
	f.read ((char*)&nlevel, 4);
	f.read ((char*)&logMin, 4), f.read ((char*)&logMax, 4), f.read ((char*)&coverage, 4);
	if (!f || memcmp (magic, "ODST", 4) || version != 1 || nlevel != NLEVEL || logMin != -7.5f || logMax != 0.0f) {
		oapiWriteLogV ("VulkanClient: %s: unsupported dust map", path.c_str());
		return false;
	}
	struct Level { float cell; uint32_t n[3]; float origin[3]; } lv[NLEVEL];
	for (auto &l : lv) f.read ((char*)&l, sizeof(Level));
	Model *m = (Model*)model.mapped;
	for (int i = 0; i < NLEVEL; i++) {
		const Level &l = lv[i];
		size_t n = (size_t)l.n[0] * l.n[1] * l.n[2];
		std::vector<uint8_t> data (n);
		f.read ((char*)data.data(), n);
		if (!f) return false;
		for (int k = 0; k < 3; k++) {
			m->levelOrigin[i][k] = l.origin[k];
			m->levelSize[i][k] = l.cell * l.n[k];
		}
		m->levelSize[i][3] = l.cell;

		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_3D, ici.format = VK_FORMAT_R8_UNORM;
		ici.extent = { l.n[0], l.n[1], l.n[2] };
		ici.mipLevels = 1, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ctx->Share (ici); // (the bake, on the async compute queue)
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &dust[i].image, &dust[i].alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = dust[i].image, vci.viewType = VK_IMAGE_VIEW_TYPE_3D, vci.format = VK_FORMAT_R8_UNORM;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &dust[i].view));

		vkc::Buffer staging = ctx->CreateBuffer (n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		memcpy (staging.mapped, data.data(), n);
		VkImage img = dust[i].image;
		VkExtent3D ext = ici.extent;
		ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
			vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkBufferImageCopy r{};
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.imageExtent = ext;
			vkCmdCopyBufferToImage (cmd, staging.buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
		ctx->DestroyBuffer (staging);
	}
	// (the map's coverage ends where the file says)
	m->dust2[2] = std::min (m->dust2[2], coverage);
	m->dust2[1] = std::min (m->dust2[1], m->dust2[2] - 1.0f);
	return true;
}

void GalaxySky::CubeBarrier (VkCommandBuffer cmd, int c, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = srcStage, b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage, b.dstAccessMask = dstAccess;
	b.oldLayout = from, b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = cubes[c].image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

void GalaxySky::CreateCubes ()
{
	for (auto &c : cubes) {
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_R16G16B16A16_SFLOAT, ici.extent = { SIZE, SIZE, 1 };
		ici.mipLevels = 1, ici.arrayLayers = 6, ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ctx->Share (ici);
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &c.image, &c.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = c.image, vci.format = ici.format;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
		vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &c.cubeView));
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &c.arrayView));
	}
	ctx->ImmediateSubmit ([this](VkCommandBuffer cmd) {
		for (int c = 0; c < 2; c++) {
			CubeBarrier (cmd, c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkClearColorValue black{{ 0.0f, 0.0f, 0.0f, 0.0f }};
			VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
			vkCmdClearColorImage (cmd, cubes[c].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
			CubeBarrier (cmd, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		}
	});
}

void GalaxySky::CreatePipelines ()
{
	// bake: model, dust grids, target cube
	{
		VkDescriptorSetLayoutBinding b[7] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NLEVEL, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // nebula sources
			{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // their fronts
			{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // the bake's lists of them
			{ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } }; // galaxies and clusters
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 7, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &bakeLayout));
	}
	// draw: front and back cubes, the zodiacal light
	{
		VkDescriptorSetLayoutBinding b[3] = {
			{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 3, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &drawLayout));
	}
	VkDescriptorPoolSize ps[3] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 5 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * NLEVEL + 4 * 3 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 6, pci.poolSizeCount = 3, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	for (int c = 0; c < 2; c++) {
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = pool, dai.descriptorSetCount = 1;
		dai.pSetLayouts = &bakeLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &bakeSet[c]));
		dai.pSetLayouts = &drawLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &drawSet[c][0]));
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &drawSet[c][1]));

		VkDescriptorBufferInfo bi{ model.buffer, 0, VK_WHOLE_SIZE };
		VkDescriptorBufferInfo nb[3] = { { nebulae.Sources (), 0, VK_WHOLE_SIZE }, { nebulae.Fronts (), 0, VK_WHOLE_SIZE },
			{ objects.Buffer (), 0, VK_WHOLE_SIZE } };
		VkDescriptorImageInfo di[NLEVEL];
		for (int l = 0; l < NLEVEL; l++) di[l] = { dustSampler, dust[l].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo oi{ VK_NULL_HANDLE, cubes[c].arrayView, VK_IMAGE_LAYOUT_GENERAL };
		// drawSet[c]: cube c in front, alone (the other may be baking) or
		// with the other behind
		VkDescriptorImageInfo fi[2] = { { sampler, cubes[c].cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ sampler, cubes[1 - c].cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
		VkWriteDescriptorSet wr[12]{};
		for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.descriptorCount = 1;
		for (int k = 0; k < 3; k++) {
			wr[7 + k].dstSet = bakeSet[c], wr[7 + k].dstBinding = k < 2 ? 3 + k : 6, wr[7 + k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			wr[7 + k].pBufferInfo = &nb[k];
		}
		wr[0].dstSet = bakeSet[c], wr[0].dstBinding = 0, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[0].pBufferInfo = &bi;
		wr[1].dstSet = bakeSet[c], wr[1].dstBinding = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[1].pImageInfo = di;
		wr[1].descriptorCount = NLEVEL;
		wr[2].dstSet = bakeSet[c], wr[2].dstBinding = 2, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[2].pImageInfo = &oi;
		for (int k = 0; k < 4; k++) {
			VkWriteDescriptorSet &w = wr[3 + k];
			w.dstSet = drawSet[c][k / 2], w.dstBinding = k % 2, w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			w.pImageInfo = &fi[k == 3 ? 1 : 0];
		}
		VkDescriptorImageInfo zi{ sampler, zodiacal.View (), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		for (int k = 0; k < 2; k++) {
			wr[10 + k].dstSet = drawSet[c][k], wr[10 + k].dstBinding = 2;
			wr[10 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[10 + k].pImageInfo = &zi;
		}
		vkUpdateDescriptorSets (ctx->device, 12, wr, 0, nullptr);
		BindNebulae (c);
	}

	// bake pipeline
	{
		VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * sizeof(float) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &bakeLayout;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &bakePL));
		VkShaderModule cs = ctx->CreateShaderModule (spv_sky_bake_comp, spv_sky_bake_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = bakePL;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &bakePipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
	}
	// draw pipeline: full-screen triangle at infinity, added to the scene
	{
		VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 * sizeof(float) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &drawLayout;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &drawPL));
		VkShaderModule vs = ctx->CreateShaderModule (spv_milkyway_vert, spv_milkyway_vert_size);
		VkShaderModule fs = ctx->CreateShaderModule (spv_milkyway_frag, spv_milkyway_frag_size);
		VkPipelineShaderStageCreateInfo stages[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vps.viewportCount = 1, vps.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = ctx->sceneSamples;
		VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = 0xF;
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1, cb.pAttachments = &cba;
		VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
		VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rci.colorAttachmentCount = 1;
		rci.pColorAttachmentFormats = &ctx->sceneFormat;
		rci.depthAttachmentFormat = ctx->depthFormat;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = stages;
		gpi.pVertexInputState = &vi, gpi.pInputAssemblyState = &ia, gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs, gpi.pMultisampleState = &ms, gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cb, gpi.pDynamicState = &dy;
		gpi.layout = drawPL;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &drawPipe));
		// (the extragalactic background seen moving: alike, its own shader)
		VkShaderModule bfs = ctx->CreateShaderModule (spv_cosmicbg_frag, spv_cosmicbg_frag_size);
		stages[1].module = bfs;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &bgPipe));
		vkDestroyShaderModule (ctx->device, bfs, nullptr);
		vkDestroyShaderModule (ctx->device, vs, nullptr);
		vkDestroyShaderModule (ctx->device, fs, nullptr);
	}
}

void GalaxySky::BindNebulae (int c)
{
	VkDescriptorBufferInfo bi{ nebulae.Tiles (c), 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = bakeSet[c], w.dstBinding = 5, w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w.pBufferInfo = &bi;
	vkUpdateDescriptorSets (ctx->device, 1, &w, 0, nullptr);
}

uint64_t GalaxySky::CacheKey () const
{
	// (FNV-1a) over what the bake depends on: the shader, the model and
	// the objects' files, the data files' sizes and times, the settings
	uint64_t h = 1469598103934665603ull;
	auto add = [&](const void *p, size_t n) {
		for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
	};
	const uint32_t version = 1;
	add (&version, sizeof(version));
	add (spv_sky_bake_comp, spv_sky_bake_comp_size);
	for (const char *f : { "Config/CSphere/Sky/galaxy.cfg", "Config/CSphere/Sky/galaxies.cfg", "Config/CSphere/Sky/globulars.csv" }) {
		std::ifstream ifs (f, std::ios::binary);
		std::string d ((std::istreambuf_iterator<char> (ifs)), std::istreambuf_iterator<char> ());
		add (d.data (), d.size ());
	}
	for (const char *f : { "Config/CSphere/Sky/dust.bin", "Config/CSphere/Sky/stars.bin" }) {
		std::error_code ec;
		uint64_t size = std::filesystem::file_size (f, ec);
		int64_t time = std::filesystem::last_write_time (f, ec).time_since_epoch ().count ();
		add (&size, sizeof(size));
		add (&time, sizeof(time));
	}
	const float lim = starLimit;
	const uint8_t flags[2] = { settings.nebulae, settings.galaxies };
	const uint32_t size = SIZE;
	add (&lim, sizeof(lim));
	add (flags, sizeof(flags));
	add (&size, sizeof(size));
	return h;
}

namespace {
	struct CacheHeader { char magic[4]; uint32_t size; uint64_t key; double obs[3]; };
	const VkDeviceSize CUBE_BYTES = (VkDeviceSize)GalaxySky::SIZE * GalaxySky::SIZE * 8 * 6;
}

bool GalaxySky::LoadCache (const galactic::Observer &obs)
{
	FILE *f = fopen (CACHE, "rb");
	if (!f) return false;
	CacheHeader h;
	bool ok = fread (&h, sizeof(h), 1, f) == 1 && !memcmp (h.magic, "OSKY", 4) && h.size == SIZE && h.key == CacheKey ();
	if (ok) {
		double d2 = 0.0;
		for (int k = 0; k < 3; k++) d2 += (h.obs[k] - obs.pos[k]) * (h.obs[k] - obs.pos[k]);
		ok = d2 < 0.25 * REBAKE_DIST * REBAKE_DIST;
	}
	vkc::Buffer staging;
	if (ok) {
		staging = ctx->CreateBuffer (CUBE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		ok = fread (staging.mapped, 1, CUBE_BYTES, f) == CUBE_BYTES;
	}
	fclose (f);
	if (ok) {
		vmaFlushAllocation (ctx->allocator, staging.alloc, 0, VK_WHOLE_SIZE);
		ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
			CubeBarrier (cmd, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkBufferImageCopy r{};
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6 };
			r.imageExtent = { SIZE, SIZE, 1 };
			vkCmdCopyBufferToImage (cmd, staging.buffer, cubes[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			CubeBarrier (cmd, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
	}
	if (staging.buffer) ctx->DestroyBuffer (staging);
	return ok;
}

void GalaxySky::SaveCache ()
{
	std::error_code ec;
	std::filesystem::create_directories (std::filesystem::path (CACHE).parent_path (), ec);
	vkc::Buffer rb = ctx->CreateReadbackBuffer (CUBE_BYTES);
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		CubeBarrier (cmd, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy r{};
		r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6 };
		r.imageExtent = { SIZE, SIZE, 1 };
		vkCmdCopyImageToBuffer (cmd, cubes[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
		CubeBarrier (cmd, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	vmaInvalidateAllocation (ctx->allocator, rb.alloc, 0, VK_WHOLE_SIZE);
	// (written whole, then renamed: a cut-off file is never read)
	std::string tmp = std::string (CACHE) + ".tmp";
	bool ok = false;
	if (FILE *f = fopen (tmp.c_str (), "wb")) {
		CacheHeader h{ { 'O', 'S', 'K', 'Y' }, SIZE, CacheKey (), { cubes[0].obs.pos[0], cubes[0].obs.pos[1], cubes[0].obs.pos[2] } };
		ok = fwrite (&h, sizeof(h), 1, f) == 1 && fwrite (rb.mapped, 1, CUBE_BYTES, f) == CUBE_BYTES;
		ok = fclose (f) == 0 && ok;
	}
	if (ok) std::filesystem::rename (tmp, CACHE, ec);
	else std::filesystem::remove (tmp, ec);
	ctx->DestroyBuffer (rb);
}

bool GalaxySky::Init (vkc::Context *c, const galactic::Observer &obs, const galactic::StarCatalogue &cat, const SkySettings &set)
{
	ctx = c;
	settings = set;
	starLimit = settings.starLimit;
	std::vector<float> buf;
	if (!LoadModel ("Config/CSphere/Sky/galaxy.cfg", buf)) {
		oapiWriteLogError ("VulkanClient: bad galaxy model (Config/CSphere/Sky/galaxy.cfg)");
		return false;
	}
	model = ctx->CreateBuffer (sizeof(Model), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true);
	memcpy (model.mapped, buf.data(), sizeof(Model));
	if (!LoadDust ("Config/CSphere/Sky/dust.bin")) {
		oapiWriteLogError ("VulkanClient: bad dust map (Config/CSphere/Sky/dust.bin)");
		ctx->DestroyBuffer (model);
		for (auto &d : dust) ctx->DestroyImage (d);
		return false;
	}
	if (const char *e = getenv ("ORBITER_VK_SKY_ROWS")) fixedRows = std::max (8, atoi (e) / 8 * 8);
	VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qci.queryCount = 2 * vkc::FRAMES_IN_FLIGHT;
	VK_CHECK(vkCreateQueryPool (ctx->device, &qci, nullptr, &queries));
	// later bakes on the async compute queue, if there is one (and the rows
	// per frame are not fixed)
	if (const char *e = getenv ("ORBITER_VK_SKY_ASYNC_ROWS")) asyncRows = std::max (8, atoi (e) / 8 * 8);
	if (const char *e = getenv ("ORBITER_VK_SKY_ASYNC_CHAIN")) asyncChain = std::max (1, atoi (e));
	if (ctx->AsyncCompute () && !fixedRows) {
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = ctx->ComputeFamily ();
		VK_CHECK(vkCreateCommandPool (ctx->device, &pci, nullptr, &asyncPool));
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = asyncPool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers (ctx->device, &cai, &asyncCmd));
		VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		sci.pNext = &tci;
		VK_CHECK(vkCreateSemaphore (ctx->device, &sci, nullptr, &asyncDone));
	}
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &dustSampler));
	nebulae.Init (ctx, *this, settings.nebulae ? &cat : nullptr);
	objects.Init (ctx, *this);
	zodiacal.Init (ctx, *this);
	zodiacal.Update (obs.pos, true);
	for (int k = 0; k < 2; k++) nebulae.Bin (obs, k); // (both slots: their buffers)
	CreateCubes ();
	CreatePipelines ();

	// the first bake at once (or from the cache: the last session's, if
	// from about here with the same data and settings)
	auto t0 = std::chrono::steady_clock::now ();
	cubes[0].obs = obs, cubes[0].limit = starLimit;
	bool useCache = true;
	if (const char *e = getenv ("ORBITER_VK_SKY_CACHE")) useCache = atoi (e) != 0;
	if (useCache && LoadCache (obs)) {
		double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
		oapiWriteLogV ("VulkanClient: Milky Way from (%.1f, %.1f, %.1f) pc loaded from %s in %.0f ms", obs.pos[0], obs.pos[1], obs.pos[2], CACHE, ms);
		msPerRow = 0.08; // (about, on a fast GPU)
		ready = true;
		return true;
	}
	for (int f = 0; f < 6; f++)
		ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
			if (f == 0) CubeBarrier (cmd, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
			BakeRows (cmd, 0, f, 0, SIZE, obs, starLimit);
			if (f == 5) CubeBarrier (cmd, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
	double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
	oapiWriteLogV ("VulkanClient: Milky Way baked from (%.1f, %.1f, %.1f) pc in %.0f ms", obs.pos[0], obs.pos[1], obs.pos[2], ms);
	msPerRow = ms / (6.0 * SIZE); // (the first estimate)
	if (useCache) SaveCache ();
	ready = true;
	return true;
}

void GalaxySky::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	vkDestroyQueryPool (ctx->device, queries, nullptr);
	queries = VK_NULL_HANDLE;
	if (asyncPool) vkDestroyCommandPool (ctx->device, asyncPool, nullptr);
	if (asyncDone) vkDestroySemaphore (ctx->device, asyncDone, nullptr);
	asyncPool = VK_NULL_HANDLE, asyncCmd = VK_NULL_HANDLE, asyncDone = VK_NULL_HANDLE;
	vkDestroyPipeline (ctx->device, bakePipe, nullptr);
	vkDestroyPipeline (ctx->device, drawPipe, nullptr);
	vkDestroyPipeline (ctx->device, bgPipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, bakePL, nullptr);
	vkDestroyPipelineLayout (ctx->device, drawPL, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, bakeLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, drawLayout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	vkDestroySampler (ctx->device, dustSampler, nullptr);
	for (auto &c : cubes) {
		vkDestroyImageView (ctx->device, c.cubeView, nullptr);
		vkDestroyImageView (ctx->device, c.arrayView, nullptr);
		if (c.image) vmaDestroyImage (ctx->allocator, c.image, c.alloc);
		c = Cube{};
	}
	for (auto &d : dust) ctx->DestroyImage (d);
	ctx->DestroyBuffer (model);
	nebulae.Shutdown ();
	objects.Shutdown ();
	zodiacal.Shutdown ();
	ready = false;
	ctx = nullptr;
}

void GalaxySky::BakeRows (VkCommandBuffer cmd, int c, int face, int row0, int nRows, const galactic::Observer &o, float mLim)
{
	struct { float obs[4]; int face, row0, size, pad; } p;
	for (int k = 0; k < 3; k++) p.obs[k] = (float)o.pos[k];
	p.obs[3] = mLim;
	p.face = face, p.row0 = row0, p.size = (int)SIZE, p.pad = 0;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakePL, 0, 1, &bakeSet[c], 0, nullptr);
	vkCmdPushConstants (cmd, bakePL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, (SIZE + 7) / 8, ((uint32_t)nRows + 7) / 8, 1);
}

void GalaxySky::SetStarLimit (float m)
{
	if (m == starLimit) return;
	starLimit = m;
	bakeAgain = true;
}

void GalaxySky::ReadBakeTime ()
{
	// the GPU time of this frame slot's last block (its fence has been
	// waited for)
	uint32_t fi = ctx->FrameIndex ();
	if (slotRows[fi] > 0) {
		uint64_t t[2];
		if (vkGetQueryPoolResults (ctx->device, queries, 2 * fi, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
			double ms = (double)(t[1] - t[0]) * ctx->deviceProps.limits.timestampPeriod * 1e-6;
			msPerRow = ms / slotRows[fi];
			bakeStats[0] += ms, bakeStats[1] = std::max (bakeStats[1], ms), bakeStats[2] += 1.0;
		}
		slotRows[fi] = 0;
	}
	// (a finished bake's, all blocks measured)
	if (!baking && bakeFrames && std::none_of (slotRows, slotRows + vkc::FRAMES_IN_FLIGHT, [](int n) { return n > 0; })) {
		if (getenv ("ORBITER_VK_SKY_LOG"))
			oapiWriteLogV ("VulkanClient: Milky Way rebaked from (%.1f, %.1f, %.1f) pc: %d frames, GPU %.0f ms (%.2f ms per frame, at most %.2f)",
				bakeObs.pos[0], bakeObs.pos[1], bakeObs.pos[2],
				bakeFrames, bakeStats[0], bakeStats[0] / std::max (bakeStats[2], 1.0), bakeStats[1]);
		bakeFrames = 0, bakeStats[0] = bakeStats[1] = bakeStats[2] = 0.0;
	}
}

int GalaxySky::BakeBlock ()
{
	if (fixedRows) return fixedRows;
	// rows of the time per row of the latest measure (the rows nearby cost
	// alike; the cost differs across the sky), in blocks of 8 rows (a
	// workgroup's), at most doubling from one block to the next (the
	// measure lags a frame or two)
	int n = (int)(settings.bakeBudget / std::max (msPerRow, 1e-4)) / 8 * 8;
	if (lastRows) n = std::min (n, 2 * lastRows);
	return std::clamp (n, 8, (int)SIZE);
}

void GalaxySky::Update (const galactic::Observer &obs, double dt)
{
	if (!ready) return;
	ReadBakeTime ();
	zodiacal.Update (obs.pos);
	if (fade > 0.0f) {
		fade += (float)(dt / FADE_TIME);
		if (fade >= 1.0f) front = 1 - front, fade = 0.0f;
		return;
	}
	if (!baking) {
		// (a bake under way for an old star limit is finished first)
		const double *a = obs.pos, *b = cubes[front].obs.pos;
		double d = std::sqrt ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
		if (d < REBAKE_DIST && !bakeAgain) return;
		baking = true, bakeAgain = false, bakeFace = 0, bakeRow = 0, bakeObs = obs, bakeLimit = starLimit;
		if (nebulae.Bin (bakeObs, 1 - front)) BindNebulae (1 - front);
		bakeStart = std::chrono::steady_clock::now ();
		// (the frames that may have drawn the back cube done first)
		bakeAfterFrame = ctx->FrameValue ();
	}
	if (asyncCmd) {
		UpdateAsync ();
		return;
	}
	int back = 1 - front;
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd ();
	if (bakeFace == 0 && bakeRow == 0)
		CubeBarrier (cmd, back, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	int n = std::min (BakeBlock (), (int)SIZE - bakeRow);
	uint32_t fi = ctx->FrameIndex ();
	vkCmdResetQueryPool (cmd, queries, 2 * fi, 2);
	vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 2 * fi);
	BakeRows (cmd, back, bakeFace, bakeRow, n, bakeObs, bakeLimit);
	vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 2 * fi + 1);
	slotRows[fi] = n, lastRows = n, bakeFrames++;
	bakeRow += n;
	if (bakeRow >= (int)SIZE) bakeRow = 0, bakeFace++;
	if (bakeFace == 6) {
		CubeBarrier (cmd, back, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		cubes[back].obs = bakeObs, cubes[back].limit = bakeLimit;
		baking = false;
		fade = 1e-3f;
	}
}

void GalaxySky::UpdateAsync ()
{
	// one block at a time on the async compute queue; the frames go on
	if (asyncValue) {
		uint64_t v = 0;
		vkGetSemaphoreCounterValue (ctx->device, asyncDone, &v);
		if (v < asyncValue) return;
	}
	int back = 1 - front;
	if (bakeFace == 6) {
		// done: the frame that starts to show it waits for its writes
		ctx->WaitBeforeFrame (asyncDone, asyncValue, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		cubes[back].obs = bakeObs, cubes[back].limit = bakeLimit;
		baking = false;
		fade = 1e-3f;
		if (getenv ("ORBITER_VK_SKY_LOG"))
			oapiWriteLogV ("VulkanClient: Milky Way rebaked from (%.1f, %.1f, %.1f) pc on the async compute queue: %d blocks, %.0f ms",
				bakeObs.pos[0], bakeObs.pos[1], bakeObs.pos[2], bakeFrames,
				std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - bakeStart).count ());
		bakeFrames = 0;
		return;
	}
	VK_CHECK(vkResetCommandBuffer (asyncCmd, 0));
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer (asyncCmd, &bi));
	bool first = bakeFace == 0 && bakeRow == 0;
	if (first)
		CubeBarrier (asyncCmd, back, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	// small dispatches one after the other: the bake's long-running rays
	// hold few of the GPU's units at a time, which the frames' work then
	// still finds free
	for (int k = 0; k < asyncChain && bakeFace < 6; k++) {
		if (k) {
			VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
			mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
			vkCmdPipelineBarrier2 (asyncCmd, &dep);
		}
		int n = std::min (asyncRows, (int)SIZE - bakeRow);
		BakeRows (asyncCmd, back, bakeFace, bakeRow, n, bakeObs, bakeLimit);
		bakeRow += n;
		if (bakeRow >= (int)SIZE) bakeRow = 0, bakeFace++;
	}
	bakeFrames++;
	if (bakeFace == 6)
		CubeBarrier (asyncCmd, back, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
	VK_CHECK(vkEndCommandBuffer (asyncCmd));
	VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	wait.semaphore = ctx->FrameTimeline (), wait.value = bakeAfterFrame;
	wait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	signal.semaphore = asyncDone, signal.value = ++asyncValue;
	signal.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = asyncCmd;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = first ? 1 : 0, si.pWaitSemaphoreInfos = &wait;
	si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
	si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &signal;
	VK_CHECK(vkQueueSubmit2 (ctx->AsyncCompute (), 1, &si, VK_NULL_HANDLE));
}

void GalaxySky::Render (VkCommandBuffer cmd, const double R[3][3], float fx, float fy, const Display &dsp, const float relBetaView[3])
{
	if (!ready) return;
	struct { float rows[3][4]; float proj[4]; float colour[3][4]; float tone[4]; } p{};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) p.rows[i][j] = (float)R[i][j];
	for (int i = 0; i < 3; i++) p.rows[i][3] = relBetaView ? relBetaView[i] : 0.0f; // (relativistic view: the camera's velocity)
	p.proj[0] = 1.0f / fx, p.proj[1] = 1.0f / fy, p.proj[2] = fade, p.proj[3] = dsp.zodiacal && settings.zodiacal ? 1.0f : 0.0f;
	for (int k = 0; k < 3; k++)
		for (int b = 0; b < NBAND; b++) p.colour[k][b] = (dsp.camera ? cameraColour : bandColour)[k][b] * dsp.gain;
	// (a solar spectrum's S10 has luminance 1)
	p.tone[0] = PIVOT_S10 * dsp.gain, p.tone[1] = dsp.contrast, p.tone[2] = dsp.saturation, p.tone[3] = dsp.highlight;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPL, 0, 1, &drawSet[front][fade > 0.0f ? 1 : 0], 0, nullptr);
	vkCmdPushConstants (cmd, drawPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

void GalaxySky::RenderBackground (VkCommandBuffer cmd, float fx, float fy, const Display &dsp, const float betaView[3])
{
	if (!ready || !bgPipe) return;
	struct { float beta[4]; float proj[4]; float colour[3][4]; } p{};
	for (int i = 0; i < 3; i++) p.beta[i] = betaView[i];
	p.proj[0] = 1.0f / fx, p.proj[1] = 1.0f / fy;
	for (int k = 0; k < 3; k++)
		for (int b = 0; b < NBAND; b++) p.colour[k][b] = (dsp.camera ? cameraColour : bandColour)[k][b] * dsp.gain;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bgPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPL, 0, 1, &drawSet[front][0], 0, nullptr);
	vkCmdPushConstants (cmd, drawPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

bool GalaxySky::Probe (const std::vector<std::array<double, 2>> &lb, std::vector<std::array<float, NBAND>> &out, const char *png)
{
	if (!ready) return false;
	const VkDeviceSize faceBytes = (VkDeviceSize)SIZE * SIZE * 8;
	vkc::Buffer rb = ctx->CreateReadbackBuffer (faceBytes * 6);
	VkImage img = cubes[front].image;
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		CubeBarrier (cmd, front, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy r{};
		r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6 };
		r.imageExtent = { SIZE, SIZE, 1 };
		vkCmdCopyImageToBuffer (cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
		CubeBarrier (cmd, front, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	vmaInvalidateAllocation (ctx->allocator, rb.alloc, 0, VK_WHOLE_SIZE);
	const uint16_t *px = (const uint16_t*)rb.mapped;
	auto texel = [&](const double d[3], float v[NBAND]) {
		int f; double u, w;
		CubeFace (d, f, u, w);
		int x = std::clamp ((int)((u * 0.5 + 0.5) * SIZE), 0, (int)SIZE - 1);
		int y = std::clamp ((int)((w * 0.5 + 0.5) * SIZE), 0, (int)SIZE - 1);
		const uint16_t *t = px + ((size_t)f * SIZE * SIZE + (size_t)y * SIZE + x) * 4;
		for (int b = 0; b < NBAND; b++) v[b] = HalfToFloat (t[b]);
	};
	out.clear ();
	for (auto &q : lb) {
		double l = q[0] * PI_ / 180.0, b = q[1] * PI_ / 180.0;
		double d[3] = { std::cos (b) * std::cos (l), std::cos (b) * std::sin (l), std::sin (b) };
		std::array<float, NBAND> v;
		texel (d, v.data());
		out.push_back (v);
	}
	if (png) {
		// equirectangular, l = 180 deg at the left edge to -180 at the right;
		// brightness: x / (1 + x) of S10 / 300, gamma 2.2
		const uint32_t W = 2048, H = 1024;
		std::vector<uint8_t> rgb ((size_t)W * H * 3);
		std::vector<float> raw ((size_t)W * H * NBAND); // (also as raw floats: png + ".f32")
		for (uint32_t j = 0; j < H; j++) {
			double b = (90.0 - (j + 0.5) * 180.0 / H) * PI_ / 180.0;
			for (uint32_t i = 0; i < W; i++) {
				double l = (180.0 - (i + 0.5) * 360.0 / W) * PI_ / 180.0;
				double d[3] = { std::cos (b) * std::cos (l), std::cos (b) * std::sin (l), std::sin (b) };
				float v[NBAND];
				texel (d, v);
				memcpy (&raw[((size_t)j * W + i) * NBAND], v, sizeof(v));
				for (int k = 0; k < 3; k++) {
					double c = 0.0;
					for (int n = 0; n < NBAND; n++) c += bandColour[k][n] * v[n];
					c = std::max (0.0, c / 300.0);
					rgb[((size_t)j * W + i) * 3 + k] = (uint8_t)std::lround (255.0 * std::pow (c / (1.0 + c), 1.0 / 2.2));
				}
			}
		}
		image::WritePNG (png, W, H, rgb.data());
		if (FILE *f = fopen ((std::string (png) + ".f32").c_str (), "wb")) {
			fwrite (raw.data(), sizeof(float), raw.size(), f);
			fclose (f);
		}
	}
	ctx->DestroyBuffer (rb);
	return true;
}
