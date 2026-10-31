// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The 3D star catalogue (Config/CSphere/Sky/stars.bin, built by
// Utils/SkyKit/build_stars.py from Gaia DR3 and Hipparcos): every star's
// place in space, light and motion, in the Galactic frame of
// GalacticFrame.h, so that the sky can be drawn from anywhere.
//
// Stars are grouped in cubic sectors (100 pc), brightest first within
// each; a star's position is stored relative to its sector's corner.
// Identifiers are stable across rebuilds: the Gaia DR3 source_id, or
// 2^63 + the Hipparcos number for stars Gaia lacks; the Sun is 2^63.
//
// File layout (little-endian):
//   char[4] 'OSTR', uint32 version (1), uint32 nStar, uint32 nSector,
//   uint32 nHip, float sector edge [pc], float epoch [Julian year],
//   float completeness: the catalogue holds every star brighter than this
//   G magnitude seen from the Sun, float and every star within this
//   distance of the Sun [pc], uint32[4] reserved
//   nSector x { int16 index[3], uint16 -, uint32 first star, uint32 count }
//   nStar x uint16[3]  position in the sector [edge / 65536]
//   nStar x int16      absolute visual magnitude M_V [0.001 mag]
//   nStar x uint16     log10 effective temperature [1e-4]
//   nStar x uint16     visual extinction from the Sun A_V [0.001 mag]
//   nStar x uint16     flags (StarCatalogue::Flag)
//   nStar x half[3]    heliocentric Galactic velocity [km/s]
//   nStar x uint64     identifier
//   nHip x { uint32 star, uint32 Hipparcos number } (by star)
// =======================================================================

#ifndef __STARCATALOGUE_H
#define __STARCATALOGUE_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace galactic {

	class StarCatalogue {
	public:
		enum Flag : uint16_t {
			SUN = 1,             ///< the Sun (at the origin)
			HIPPARCOS = 2,       ///< astrometry or photometry from Hipparcos
			NEARBY = 4,          ///< within 25 pc
			DIST_UNCERTAIN = 8,  ///< distance drawn from a posterior wider than 10%
			NO_COLOUR = 16,      ///< temperature guessed (no measured colour)
			NO_RV = 32,          ///< no radial velocity (velocity tangential only)
			MULTIPLE = 64,       ///< known or suspected multiple
			SPECTRAL = 128,      ///< temperature (and extinction from the Sun) from the spectral type
			EVOLVED = 256,       ///< (with SPECTRAL) a giant, supergiant or Wolf-Rayet star
			MEMBER = 512         ///< of a globular cluster or the Magellanic Clouds: placed at its distance
		};
		static const uint64_t ID_HIP = 0x8000000000000000ull; ///< + Hipparcos number

		struct Sector { int16_t index[3]; uint16_t pad; uint32_t first, count; };

		/// Read a catalogue; false (and empty) if missing or unsupported
		bool Load (const char *path)
		{
			Clear ();
			FILE *f = fopen (path, "rb");
			if (!f) return false;
			char magic[4];
			uint32_t hdr[4], reserved[4];
			float geo[4];
			bool ok = fread (magic, 4, 1, f) == 1 && fread (hdr, 4, 4, f) == 4 && fread (geo, 4, 4, f) == 4 &&
				fread (reserved, 4, 4, f) == 4 && !memcmp (magic, "OSTR", 4) && hdr[0] == 1;
			if (ok) {
				uint32_t n = hdr[1];
				sectorEdge = geo[0], epoch = geo[1], completeMag = geo[2], completeDist = geo[3];
				sectors.resize (hdr[2]);
				pos.resize ((size_t)n * 3);
				mv.resize (n), logT.resize (n), av.resize (n), flags.resize (n);
				vel.resize ((size_t)n * 3);
				ids.resize (n);
				hip.resize ((size_t)hdr[3] * 2);
				ok = Read (f, sectors) && Read (f, pos) && Read (f, mv) && Read (f, logT) && Read (f, av) &&
					Read (f, flags) && Read (f, vel) && Read (f, ids) && Read (f, hip);
				sectorOf.resize (n);
				for (uint32_t s = 0; ok && s < sectors.size(); s++) {
					const Sector &sc = sectors[s];
					if ((uint64_t)sc.first + sc.count > n) ok = false;
					else for (uint32_t i = 0; i < sc.count; i++) sectorOf[sc.first + i] = s;
				}
			}
			fclose (f);
			if (!ok) Clear ();
			return ok;
		}

		void Clear ()
		{
			sectors.clear (), pos.clear (), mv.clear (), logT.clear (), av.clear ();
			flags.clear (), vel.clear (), ids.clear (), hip.clear (), sectorOf.clear ();
		}

		size_t Count () const { return mv.size(); }
		float SectorEdge () const { return sectorEdge; }
		float Epoch () const { return epoch; }   ///< of the positions [Julian year]
		/// the catalogue holds every star brighter than CompleteMag (G, seen
		/// from the Sun) and every star within CompleteDist [pc] of the Sun
		/// (others are left to be generated)
		float CompleteMag () const { return completeMag; }
		float CompleteDist () const { return completeDist; }
		const std::vector<Sector> &Sectors () const { return sectors; }

		/// heliocentric Galactic position [pc] at the catalogue's epoch
		void Position (size_t i, double p[3]) const
		{
			const Sector &s = sectors[sectorOf[i]];
			for (int k = 0; k < 3; k++)
				p[k] = (s.index[k] + pos[i * 3 + k] / 65536.0) * sectorEdge;
		}
		/// heliocentric Galactic velocity [km/s]
		void Velocity (size_t i, double v[3]) const
		{
			for (int k = 0; k < 3; k++) v[k] = HalfToFloat (vel[i * 3 + k]);
		}
		double AbsMag (size_t i) const { return mv[i] * 1e-3; }               ///< M_V
		double Teff (size_t i) const { return std::pow (10.0, logT[i] * 1e-4); } ///< [K]
		double ExtinctionFromSun (size_t i) const { return av[i] * 1e-3; }    ///< A_V
		uint16_t Flags (size_t i) const { return flags[i]; }
		uint64_t Id (size_t i) const { return ids[i]; }
		/// Hipparcos number (0: none)
		uint32_t Hip (size_t i) const
		{
			size_t lo = 0, hi = hip.size() / 2;
			while (lo < hi) {
				size_t m = (lo + hi) / 2;
				if (hip[m * 2] < i) lo = m + 1; else hi = m;
			}
			return lo < hip.size() / 2 && hip[lo * 2] == i ? hip[lo * 2 + 1] : 0;
		}

		/// raw arrays (for uploading)
		const uint16_t *RawPos () const { return pos.data(); }
		const uint32_t *SectorOf () const { return sectorOf.data(); }
		const int16_t *RawAbsMag () const { return mv.data(); }
		const uint16_t *RawLogT () const { return logT.data(); }
		const uint16_t *RawFlags () const { return flags.data(); }

	private:
		template<class T> static bool Read (FILE *f, std::vector<T> &v)
		{
			return v.empty() || fread (v.data(), sizeof(T), v.size(), f) == v.size();
		}
		static float HalfToFloat (uint16_t h)
		{
			int e = (h >> 10) & 0x1f, m = h & 0x3ff;
			float v = e == 0 ? std::ldexp ((float)m, -24) : e == 31 ? INFINITY : std::ldexp ((float)(m | 0x400), e - 25);
			return (h & 0x8000) ? -v : v;
		}

		float sectorEdge = 100.0f, epoch = 2016.0f, completeMag = 0.0f, completeDist = 0.0f;
		std::vector<Sector> sectors;
		std::vector<uint16_t> pos, logT, av, flags, vel;
		std::vector<int16_t> mv;
		std::vector<uint64_t> ids;
		std::vector<uint32_t> hip, sectorOf;
	};

} // namespace galactic

#endif // !__STARCATALOGUE_H
