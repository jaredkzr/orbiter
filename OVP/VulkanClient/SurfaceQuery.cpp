// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SurfaceQuery.h"
#include "PlanetSurface.h"
#include "Scene.h"
#include "VObjects.h"
#include "LegacyTex.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vksurf {

namespace {

	const double PI_ = 3.14159265358979323846;

	uint32_t U32 (const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

	bool LogOn ()
	{
		static const bool on = getenv ("ORBITER_VK_SURFACE_LOG") != nullptr;
		return on;
	}

	/// a tile's top level as RGBA8: block compressed, or uncompressed 32 bit
	bool DecodeRGBA (const std::vector<uint8_t> &dds, int &w, int &h, std::vector<uint8_t> &rgba)
	{
		if (dds.size() < 128) return false;
		uint32_t pf = U32 (dds.data() + 80);
		if (pf & 4) {
			uint32_t uw, uh;
			if (!legacytex::Decode (dds, uw, uh, rgba)) return false;
			w = (int)uw, h = (int)uh;
			return true;
		}
		if ((pf & 0x40) && U32 (dds.data() + 88) == 32) {
			h = (int)U32 (dds.data() + 12), w = (int)U32 (dds.data() + 16);
			if (w <= 0 || h <= 0 || dds.size() < 128 + (size_t)w * h * 4) return false;
			uint32_t mk[4] = { U32 (dds.data() + 92), U32 (dds.data() + 96), U32 (dds.data() + 100), U32 (dds.data() + 104) };
			int sh[4];
			for (int c = 0; c < 4; c++) {
				sh[c] = -1;
				for (int b = 0; b < 4; b++) if (mk[c] == (0xFFu << (8 * b))) sh[c] = 8 * b;
			}
			if (sh[0] < 0 || sh[1] < 0 || sh[2] < 0 || (mk[3] && sh[3] < 0)) return false;
			rgba.resize ((size_t)w * h * 4);
			const uint8_t *s = dds.data() + 128;
			for (size_t i = 0; i < (size_t)w * h; i++, s += 4) {
				uint32_t px = U32 (s);
				for (int c = 0; c < 3; c++) rgba[4*i+c] = (uint8_t)(px >> sh[c]);
				rgba[4*i+3] = mk[3] ? (uint8_t)(px >> sh[3]) : 255; // (no alpha mask: opaque)
			}
			return true;
		}
		return false;
	}

	/// tile indices of a point at a level
	void TileAt (int lvl, double lat, double lng, int &ilat, int &ilng, double &u, double &v)
	{
		int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
		double fy = (PI_ / 2 - lat) / PI_ * nlat, fx = (lng + PI_) / (2 * PI_) * nlng;
		ilat = std::clamp ((int)fy, 0, nlat - 1);
		ilng = std::clamp ((int)fx, 0, nlng - 1);
		u = fx - ilng, v = fy - ilat;
	}

	double NormLng (double lng)
	{
		lng = fmod (lng + PI_, 2 * PI_);
		if (lng < 0) lng += 2 * PI_;
		return lng - PI_;
	}
}

// ======================================================================
// TileProbe

TileProbe::TileProbe (std::shared_ptr<PlanetLayers> l, Layer lay, int maxLvl, const char *bodyName)
	: layers (std::move (l)), layer (lay), maxLevel (maxLvl), name (bodyName ? bodyName : "")
{
}

TileProbe::~TileProbe ()
{
	if (pending.valid ()) pending.wait ();
}

TileProbe::Entry TileProbe::Load (std::shared_ptr<PlanetLayers> layers, Layer layer, int maxLevel, double lat, double lng)
{
	auto t0 = std::chrono::steady_clock::now ();
	Entry e;
	auto done = [&]() {
		e.ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
		return e;
	};
	if (!layers || !layers->alive) return done ();
	TileLayer *tl = layer == Layer::Mask ? layers->mask.get () : layers->surf.get ();
	if (!tl || tl->Legacy ()) return done (); // (legacy patches: thread safety not established)
	for (int lvl = maxLevel; lvl >= 4; lvl--) {
		int ilat, ilng;
		double u, v;
		TileAt (lvl, lat, lng, ilat, ilng, u, v);
		std::vector<uint8_t> dds;
		if (!tl->HasTile (lvl, ilat, ilng) || !tl->Read (lvl, ilat, ilng, dds)) continue;
		int w, h;
		std::vector<uint8_t> px;
		if (!DecodeRGBA (dds, w, h, px)) continue;
		e.lvl = lvl, e.ilat = ilat, e.ilng = ilng, e.w = w, e.h = h;
		e.rgba = std::move (px);
		return done ();
	}
	return done ();
}

bool TileProbe::Sample (double lat, double lng, float rgba[4], int *lvl)
{
	lng = NormLng (lng);
	clock++;
	// a finished read into the cache (the least recently used tile out beyond six)
	if (pending.valid () && pending.wait_for (std::chrono::seconds (0)) == std::future_status::ready) {
		Entry e = pending.get ();
		if (LogOn ()) {
			if (e.w) oapiWriteLogV ("VulkanClient: surface probe %s %s tile %d/%d/%d (%dx%d) in %.1f ms", name.c_str (),
				layer == Layer::Mask ? "Mask" : "Surf", e.lvl, e.ilat, e.ilng, e.w, e.h, e.ms);
			else oapiWriteLogV ("VulkanClient: surface probe %s %s tile none in %.1f ms", name.c_str (),
				layer == Layer::Mask ? "Mask" : "Surf", e.ms);
		}
		e.used = clock;
		if (cache.size () >= 6) {
			size_t lru = 0;
			for (size_t i = 1; i < cache.size (); i++) if (cache[i].used < cache[lru].used) lru = i;
			cache.erase (cache.begin () + lru);
		}
		cache.push_back (std::move (e));
	}
	int klat, klng;
	double u, v;
	TileAt (maxLevel, lat, lng, klat, klng, u, v);
	Entry *hit = nullptr;
	for (Entry &e : cache)
		if (e.keyLvl == maxLevel && e.keyLat == klat && e.keyLng == klng) { hit = &e; break; }
	if (!hit) {
		if (!pending.valid ()) {
			// (the key of the tile asked for is set when the read is harvested: from the point itself)
			pending = std::async (std::launch::async, [lay = layers, l = layer, ml = maxLevel, lat, lng, klat, klng]() {
				Entry e = Load (lay, l, ml, lat, lng);
				e.keyLvl = ml, e.keyLat = klat, e.keyLng = klng;
				return e;
			});
		}
		return false;
	}
	hit->used = clock;
	if (hit->w == 0) return false;
	int ilat, ilng;
	TileAt (hit->lvl, lat, lng, ilat, ilng, u, v);
	double x = std::clamp (u * hit->w - 0.5, 0.0, hit->w - 1.0), y = std::clamp (v * hit->h - 0.5, 0.0, hit->h - 1.0);
	int x0 = (int)x, y0 = (int)y, x1 = std::min (x0 + 1, hit->w - 1), y1 = std::min (y0 + 1, hit->h - 1);
	float fx = (float)(x - x0), fy = (float)(y - y0);
	auto px = [&](int xx, int yy, int c) { return hit->rgba[((size_t)yy * hit->w + xx) * 4 + c] * (1.0f / 255.0f); };
	for (int c = 0; c < 4; c++) {
		float top = px (x0, y0, c) * (1 - fx) + px (x1, y0, c) * fx;
		float bot = px (x0, y1, c) * (1 - fx) + px (x1, y1, c) * fx;
		rgba[c] = top * (1 - fy) + bot * fy;
	}
	if (lvl) *lvl = hit->lvl;
	return true;
}

// ======================================================================
// Queries

bool HasSea (OBJHANDLE hp)
{
	SURFDETAILPARAM sdp{};
	if (oapiSurfaceDetailParams (hp, &sdp)) return sdp.sea;
	PROCSURFACEPARAM p{};
	return oapiProcSurfaceParams (hp, &p) && p.surfClass == 1;
}

Point Query (Scene *scene, OBJHANDLE hp, double lng, double lat)
{
	Point pt;
	pt.elev = oapiSurfaceElevation (hp, lng, lat);
	pt.sea = HasSea (hp);
	PROCSURFACEPARAM pp{};
	if (oapiProcSurfaceParams (hp, &pp)) pt.procClass = pp.surfClass;

	TileProbe *probe = nullptr;
	if (scene && oapiGetObjectType (hp) == OBJTP_PLANET)
		if (VObject *vo = scene->GetVisual (hp)) probe = static_cast<VPlanet*>(vo)->MaskProbe ();
	float px[4];
	int lvl = 0;
	if (probe && probe->Sample (lat, lng, px, &lvl)) {
		pt.water = 1.0f - px[3];
		pt.kind = px[3] < 0.5f ? Kind::Water : Kind::Ground;
		pt.source = Source::Mask, pt.maskLevel = lvl;
	} else {
		bool w = pt.sea && pt.elev <= 0.5 && pt.elev >= -0.5; // (the sea's surface is at 0: land below sea level is not sea while its mask tile is read)
		pt.kind = w ? Kind::Water : Kind::Ground;
		pt.water = w ? 1.0f : 0.0f;
	}
	if (pt.kind != Kind::Water) {
		if (pt.procClass == 1 && pt.sea && pt.elev <= 0.5 && pt.source == Source::Mask) pt.kind = Kind::Ice; // (pack ice)
		else if (pt.procClass == 2) pt.kind = Kind::Ice;
	}
	if (LogOn ()) {
		static auto last = std::chrono::steady_clock::time_point ();
		auto now = std::chrono::steady_clock::now ();
		if (now - last > std::chrono::seconds (2)) {
			last = now;
			char nm[64] = "";
			oapiGetObjectName (hp, nm, 64);
			char src[16];
			if (pt.source == Source::Mask) snprintf (src, 16, "mask L%d", pt.maskLevel); else strcpy (src, "fallback");
			oapiWriteLogV ("VulkanClient: surface %s lng %.4f lat %.4f (deg) elev %.2f m: %s water %.2f (%s, sea %d, class %d)",
				nm, lng * 180.0 / PI_, lat * 180.0 / PI_, pt.elev,
				pt.kind == Kind::Water ? "Water" : pt.kind == Kind::Ice ? "Ice" : "Ground", pt.water, src, (int)pt.sea, pt.procClass);
		}
	}
	return pt;
}

Point QueryGlobal (Scene *scene, OBJHANDLE hp, const VECTOR3 &gpos)
{
	VECTOR3 g = gpos, l;
	oapiGlobalToLocal (hp, &g, &l);
	double r = length (l);
	double lng = atan2 (l.z, l.x), lat = asin (std::clamp (r > 0.0 ? l.y / r : 0.0, -1.0, 1.0));
	return Query (scene, hp, lng, lat);
}

} // namespace vksurf
