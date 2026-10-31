// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The catalogue stars of the star field on the GPU (as StarField.cpp packs
// them from Config/CSphere/Sky/stars.bin)

struct Star {
	vec3 pos;    // heliocentric Galactic [pc], at the observer's epoch
	uint packed; // M_V: bits 0-11 ((M_V + 12) * 100); temperature index: 12-19;
	             // A_V from the Sun: 20-30 (* 100); the Sun: bit 31
};

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

// 5 log10 (r / 10 pc)
float StarDistModulus (float r) { return 1.50515 * log2 (max (r, 1e-6)) - 5.0; }
