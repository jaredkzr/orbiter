// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The catalogue stars of the star field on the GPU (as StarField.cpp packs
// them from Config/CSphere/Sky/stars.bin)

struct Star {
	float px, py, pz; // heliocentric Galactic [pc]: as seen from the Sun at the catalogue's epoch
	uint packed;      // M_V: bits 0-11 ((M_V + 12) * 100); temperature index: 12-19;
	                  // A_V from the Sun: 20-30 (* 100); the Sun: bit 31
	uint vel;         // velocity relative to the Sun (Galactic, km/s): x + 1024 in bits 0-10,
	                  // y + 1024 in 11-21, z + 512 in 22-31
};
const uint STAR_VEL_ZERO = 1024u | (1024u << 11) | (512u << 22);
const float STAR_LY_PER_PC = 3.2615638, STAR_PC_PER_KMS_YEAR = 1.0227122e-6;

vec3 StarPos0 (Star s) { return vec3 (s.px, s.py, s.pz); }
vec3 StarVel (Star s) { return vec3 (float (s.vel & 0x7ffu) - 1024.0, float ((s.vel >> 11) & 0x7ffu) - 1024.0, float (s.vel >> 22) - 512.0); }

// A star's position [pc] as seen from obs, `years' after the catalogue's
// epoch: where it was when the light now arriving left it (the catalogue
// has it where it was seen from the Sun, d0/c after its light left).
// Moving at v: now at X = p0 + v (years + d0/c); seen at p0 + v (years +
// (d0 - |X - obs|)/c).
vec3 StarSeenPos (Star s, vec3 obs, float years)
{
	vec3 p0 = StarPos0 (s);
	if (s.vel == STAR_VEL_ZERO) return p0;
	vec3 v = StarVel (s) * STAR_PC_PER_KMS_YEAR; // [pc/yr]
	float d0 = length (p0) * STAR_LY_PER_PC;    // [yr]
	vec3 X = p0 + v * (years + d0);
	return p0 + v * (years + d0 - length (X - obs) * STAR_LY_PER_PC);
}

// the two glare halos' shares of a star's light
const vec2 STAR_GLARE_FRAC = vec2 (0.08, 0.02);

// header of a selection list: the indirect draw's arguments, then the
// list's capacity
struct StarListHeader {
	uint vertexCount, instanceCount, firstVertex, firstInstance;
	uint capacity, pad0, pad1, pad2;
};

// header of the buffer of generated stars
struct StarGenHeader {
	uint count, capacity, pad0, pad1;
};

// a list entry's star: generated (index into the generated stars), else
// the catalogue's
const uint STAR_GENERATED = 0x80000000u;

const int STAR_NTEMP = 256;
const float STAR_LOGT0 = 3.2, STAR_LOGT1 = 4.7; // temperature index range (log10 K)

float StarAbsMag (Star s) { return float (s.packed & 0xfffu) * 0.01 - 12.0; }
uint StarTempIndex (Star s) { return (s.packed >> 12) & 0xffu; }
float StarAvSun (Star s) { return float ((s.packed >> 20) & 0x7ffu) * 0.01; }
bool StarIsSun (Star s) { return (s.packed >> 31) != 0u; }

// temperature [K] of a (fractional) temperature index
float StarTempOfIndex (float i) { return exp2 (3.321928 * (STAR_LOGT0 + (STAR_LOGT1 - STAR_LOGT0) * i / float (STAR_NTEMP - 1))); }
// the temperature index of a temperature times D (relativistic view: a star's colour seen at Doppler factor D)
float StarTempIndexShifted (uint i, float D) { return float (i) + log2 (D) * 0.30103 * float (STAR_NTEMP - 1) / (STAR_LOGT1 - STAR_LOGT0); }

// 5 log10 (r / 10 pc)
float StarDistModulus (float r) { return 1.50515 * log2 (max (r, 1e-6)) - 5.0; }
