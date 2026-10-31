// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Milky Way model of the simulated sky (Config/CSphere/Sky/galaxy.cfg):
// stellar emission of the components' populations and dust extinction at
// a point. Positions are heliocentric Galactic [pc] (x toward the centre,
// y toward l = 90 deg, z toward the north Galactic pole).

// (must match GalaxySky::Model)
const int GAL_MAX_DISKS = 4;
const int GAL_MAX_ARMS = 12;
const int GAL_NPOP = 3;      // young, thin, old
const int GAL_NLF = 257;     // unresolved-light table entries per population
const float GAL_LF_M0 = -12.0, GAL_LF_DM = 0.125;
const int GAL_NLEVEL = 3;    // dust grid levels

struct GalDisk {
	vec4 a; // scale length, scale height, normalisation, arm contrast
	vec4 b; // hole radius, population, arm normalisation (1 + c arm(Sun)), -
};

struct GalArm {
	vec4 a; // beta kink [rad], ln R kink, tan psi<, tan psi>
	vec4 b; // width [pc], beta min, beta max [rad], weight in the old stars
};

layout(std430, set = 0, binding = 0) readonly buffer GalaxyModel {
	vec4 geom;       // R0, zSun, edge radius, edge scale
	vec4 halo;       // core, power, flattening, normalisation
	vec4 bar;        // x0, y0, z0, central density
	vec4 barAxis;    // cos, sin of the angle, population, number of disks
	vec4 dust;       // mid-plane A_V/pc, scale length, scale height, arm contrast
	vec4 dust2;      // hole, map blend start, end, arm normalisation
	vec4 extinction; // A(band)/A(V)
	vec4 misc;       // number of arms, arm width growth [pc/pc], halo population, -
	vec4 neb[3];     // nebulae (nebula.glsl): gas per dust [cm^-3 per A_V/pc], least gas density [cm^-3],
	                 // largest ionised radius [pc], clump sigma; dust albedo, asymmetry g, clump wavelength [pc], -;
	                 // natal cloud: core's central density [cm^-3], share of the ionising photons it takes,
	                 // envelope's density, share
	GalDisk disks[GAL_MAX_DISKS];
	GalArm arms[GAL_MAX_ARMS];
	vec4 levelOrigin[GAL_NLEVEL]; // xyz: corner of the grid [pc], w: 1 / cell
	vec4 levelSize[GAL_NLEVEL];   // xyz: extent [pc], w: cell [pc]
	vec4 lf[GAL_NPOP * GAL_NLF];  // light of the stars fainter than M (per band) [L_sun,band / pc^3]
} gal;

layout(set = 0, binding = 1) uniform sampler3D dustGrid[GAL_NLEVEL]; // log-coded A_V per pc

const float GAL_PI = 3.14159265;

// Galactocentric position of a heliocentric one
vec3 GalCentric (vec3 h)
{
	return vec3 (h.x - gal.geom.x, h.y, h.z + gal.geom.y);
}

// Spiral arm profile at radius R, azimuth beta [rad]: the strongest arm's
// Gaussian across its width (young: all arms alike; else weighted)
float GalArms (float R, float beta, bool young)
{
	float best = 0.0;
	int n = int (gal.misc.x);
	const float FADE = 0.349; // 20 deg
	for (int i = 0; i < n; i++) {
		GalArm a = gal.arms[i];
		float wgt = young ? 1.0 : a.b.w;
		if (wgt <= 0.0) continue;
		for (int k = -1; k <= 1; k++) {
			float b = beta + 2.0 * GAL_PI * float (k);
			if (b < a.b.y - FADE || b > a.b.z + FADE) continue;
			float tp = b <= a.a.x ? a.a.z : a.a.w;
			float lnRa = a.a.y - (b - a.a.x) * tp;
			float Ra = exp (lnRa);
			if (Ra < 2500.0 || Ra > 18000.0) continue;
			float fade = clamp ((b - a.b.y) / FADE + 1.0, 0.0, 1.0) * clamp ((a.b.z - b) / FADE + 1.0, 0.0, 1.0);
			fade *= smoothstep (2500.0, 3500.0, Ra) * (1.0 - smoothstep (16000.0, 18000.0, Ra));
			float w = a.b.x + gal.misc.y * (Ra - exp (a.a.y));
			float d = (R - Ra) * inversesqrt (1.0 + tp * tp); // across the arm
			best = max (best, wgt * fade * exp (-0.5 * d * d / (w * w)));
		}
	}
	return best;
}

// Vertical profile of a disk of scale height h: exponential, rounded off
// within 0.3 h of the mid-plane (the column on each side is ~h)
float Vertical (float z, float h)
{
	const float C = 0.3;
	return exp (C - sqrt (z * z / (h * h) + C * C));
}

// Unresolved light per band of population pop with number density n,
// for stars fainter than absolute magnitude mCut
vec4 GalLight (int pop, float n, float mCut)
{
	float f = clamp ((mCut - GAL_LF_M0) / GAL_LF_DM, 0.0, float (GAL_NLF - 1) - 0.001);
	int i = int (f);
	int base = pop * GAL_NLF;
	return n * mix (gal.lf[base + i], gal.lf[base + i + 1], f - float (i));
}

// (a population's component of a vector; dynamic indexing of a local
// vector can spill it to memory)
vec3 GalPopMask (float pop) { return vec3 (equal (vec3 (0.0, 1.0, 2.0), vec3 (round (pop)))); }

// Number density of each population's stars at heliocentric h, relative
// to its luminosity function (the local one: the Sun's neighbourhood: 1)
vec3 GalPopDensity (vec3 h)
{
	vec3 g = GalCentric (h);
	float R = length (g.xy);
	float beta = atan (g.y, -g.x);
	vec3 n = vec3 (0.0);
	float edge = R > gal.geom.z ? exp (-(R - gal.geom.z) / gal.geom.w) : 1.0;

	// disks
	float armY = -1.0, armO = -1.0;
	int nd = int (gal.barAxis.w);
	for (int i = 0; i < nd; i++) {
		GalDisk d = gal.disks[i];
		float v = Vertical (g.z, d.a.y);
		if (v < 1e-6) continue;
		float rad = exp (-(R - gal.geom.x) / d.a.x) * edge;
		if (d.b.x > 0.0) rad *= 1.0 - exp (-pow (R / d.b.x, 4.0));
		float arm = 1.0;
		if (d.a.w > 0.0) {
			bool young = d.b.y < 0.5;
			float p;
			if (young) { if (armY < 0.0) armY = GalArms (R, beta, true); p = armY; }
			else { if (armO < 0.0) armO = GalArms (R, beta, false); p = armO; }
			arm = (1.0 + d.a.w * p) / d.b.z;
		}
		n += GalPopMask (d.b.y) * (d.a.z * rad * v * arm);
	}
	// halo
	{
		float rc2 = gal.halo.x * gal.halo.x;
		float r2 = R * R + g.z * g.z / (gal.halo.z * gal.halo.z);
		n += GalPopMask (gal.misc.z) * (gal.halo.w * pow ((rc2 + gal.geom.x * gal.geom.x) / (rc2 + r2), 0.5 * gal.halo.y));
	}
	// bar / bulge (boxy: Dwek et al. 1995, model G2)
	{
		float xb = -g.x * gal.barAxis.x + g.y * gal.barAxis.y;
		float yb = g.x * gal.barAxis.y + g.y * gal.barAxis.x;
		vec3 q = vec3 (xb, yb, g.z) / gal.bar.xyz;
		float s2 = sqrt ((q.x * q.x + q.y * q.y) * (q.x * q.x + q.y * q.y) + q.z * q.z * q.z * q.z); // r_s^2
		if (s2 < 60.0) n += GalPopMask (gal.barAxis.z) * (gal.bar.w * exp (-0.5 * s2));
	}
	return n;
}

// Stellar emission [L_sun,band / pc^3] at heliocentric h of the stars
// fainter than absolute magnitude mCut[pop] (seen as diffuse light)
vec4 GalEmission (vec3 h, vec3 mCut)
{
	vec3 n = GalPopDensity (h);
	vec4 j = vec4 (0.0);
	if (n.x > 0.0) j += GalLight (0, n.x, mCut.x);
	if (n.y > 0.0) j += GalLight (1, n.y, mCut.y);
	if (n.z > 0.0) j += GalLight (2, n.z, mCut.z);
	return j;
}

// Dust of the model [A_V / pc] at heliocentric h (beyond the map)
float GalDustModel (vec3 h)
{
	vec3 g = GalCentric (h);
	float R = length (g.xy);
	float v = Vertical (g.z, gal.dust.z);
	if (v < 1e-6) return 0.0;
	float rad = exp (-(R - gal.geom.x) / gal.dust.y);
	if (R > gal.geom.z) rad *= exp (-(R - gal.geom.z) / gal.geom.w);
	if (gal.dust2.x > 0.0) rad *= 1.0 - exp (-pow (R / gal.dust2.x, 4.0));
	float arm = (1.0 + gal.dust.w * GalArms (R, atan (g.y, -g.x), true)) / gal.dust2.w;
	return gal.dust.x * rad * v * arm;
}

// Dust of the map [A_V / pc] at heliocentric h, from the finest grid that
// holds it; < 0 outside all grids
float GalDustMap (vec3 h, out float cell)
{
	for (int l = 0; l < GAL_NLEVEL; l++) {
		vec3 u = (h - gal.levelOrigin[l].xyz) / gal.levelSize[l].xyz;
		if (all (greaterThanEqual (u, vec3 (0.0))) && all (lessThan (u, vec3 (1.0)))) {
			cell = gal.levelSize[l].w;
			float c = textureLod (dustGrid[l], u, 0.0).r * 255.0;
			// code 0: none; 1..255: log10 density from -7.5 to 0 (between 0
			// and 1, interpolated toward an empty voxel: linear)
			return c < 1.0 ? c * 3.16e-8 : pow (10.0, -7.5 + (c - 1.0) * (7.5 / 254.0));
		}
	}
	cell = 1e9;
	return -1.0;
}

// Dust [A_V / pc] at heliocentric h: the map near the Sun, blending into
// the model at the edge of its coverage; cell: the map's resolution there
float GalDust (vec3 h, out float cell)
{
	float r = length (h);
	float m = r < gal.dust2.z ? GalDustMap (h, cell) : -1.0;
	if (m < 0.0) { cell = 1e9; return GalDustModel (h); }
	float w = smoothstep (gal.dust2.y, gal.dust2.z, r);
	return w > 0.0 ? mix (m, GalDustModel (h), w) : m;
}

// Visual extinction A_V [mag] along the straight path from a to b: the
// midpoint rule over GAL_EXT_STEPS samples (as Utils/SkyKit/skykit/
// galdust.py, which dereddens the catalogue stars with it)
const int GAL_EXT_STEPS = 160;
float GalExtinction (vec3 a, vec3 b)
{
	vec3 d = b - a;
	float sum = 0.0, cell;
	for (int i = 0; i < GAL_EXT_STEPS; i++)
		sum += GalDust (a + d * ((float (i) + 0.5) / float (GAL_EXT_STEPS)), cell);
	return sum * length (d) / float (GAL_EXT_STEPS);
}

// (the same with fewer samples: n)
float GalExtinctionN (vec3 a, vec3 b, int n)
{
	vec3 d = b - a;
	float sum = 0.0, cell;
	for (int i = 0; i < n; i++)
		sum += GalDust (a + d * ((float (i) + 0.5) / float (n)), cell);
	return sum * length (d) / float (n);
}
