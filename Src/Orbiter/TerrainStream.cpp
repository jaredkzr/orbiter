// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TerrainStream.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef ORBITER_TERRAIN_STREAMING
#include <curl/curl.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
#endif

namespace fs = std::filesystem;
using namespace terrain;

namespace {
	const double PI = 3.14159265358979323846;
	const int SURF_N = 512;           // surface tile size [texels]
	const int ELEV_N = 259;           // elevation grid (256 intervals and a one-sample border)
	const int WORKERS = 4;
	const size_t MAX_QUEUE = 512;
	const size_t SRC_CACHE = 192;     // decoded source tiles kept in memory
	const double RETRY_FAILED = 300.0; // [s] before a failed tile is tried again

	Stream *g_stream = nullptr;

	double Now ()
	{
		using namespace std::chrono;
		return duration<double> (steady_clock::now().time_since_epoch()).count();
	}

	const char *LayerName (Layer l) { return l == SURF ? "Surf" : "Elev"; }
	const char *LayerExt (Layer l) { return l == SURF ? ".dds" : ".elv"; }

	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n");
		if (a == std::string::npos) return "";
		return s.substr (a, s.find_last_not_of (" \t\r\n") - a + 1);
	}

	bool WriteFileAtomic (const std::string &path, const std::vector<uint8_t> &data)
	{
		std::error_code ec;
		fs::create_directories (fs::path (path).parent_path(), ec);
		std::string tmp = path + ".part";
		FILE *f = fopen (tmp.c_str(), "wb");
		if (!f) return false;
		bool ok = fwrite (data.data(), 1, data.size(), f) == data.size();
		fclose (f);
		if (ok) ok = rename (tmp.c_str(), path.c_str()) == 0;
		if (!ok) remove (tmp.c_str());
		return ok;
	}

	void Put32 (std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back ((uint8_t)(x >> (8*i))); }

	// ------------------------------------------------------------------
	// DXT1 (BC1) encoding: per 4x4 block, the end points span the colours
	// along their principal direction (inset slightly), each texel takes
	// the nearest of the four palette colours

	uint16_t To565 (const float c[3])
	{
		int r = std::clamp ((int)lround (c[0] * 31.0f / 255.0f), 0, 31);
		int g = std::clamp ((int)lround (c[1] * 63.0f / 255.0f), 0, 63);
		int b = std::clamp ((int)lround (c[2] * 31.0f / 255.0f), 0, 31);
		return (uint16_t)((r << 11) | (g << 5) | b);
	}
	void From565 (uint16_t v, float c[3])
	{
		c[0] = ((v >> 11) & 31) * 255.0f / 31.0f;
		c[1] = ((v >> 5) & 63) * 255.0f / 63.0f;
		c[2] = (v & 31) * 255.0f / 31.0f;
	}

	void EncodeBlock (const uint8_t *px[16], std::vector<uint8_t> &out)
	{
		float mean[3] = {0, 0, 0};
		for (int i = 0; i < 16; i++) for (int k = 0; k < 3; k++) mean[k] += px[i][k] / 16.0f;
		// principal direction by power iteration on the covariance
		float cov[6] = {0, 0, 0, 0, 0, 0};
		for (int i = 0; i < 16; i++) {
			float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
			cov[0] += d[0]*d[0], cov[1] += d[0]*d[1], cov[2] += d[0]*d[2];
			cov[3] += d[1]*d[1], cov[4] += d[1]*d[2], cov[5] += d[2]*d[2];
		}
		float ax[3] = {0.577f, 0.577f, 0.577f};
		for (int it = 0; it < 4; it++) {
			float n[3] = { cov[0]*ax[0] + cov[1]*ax[1] + cov[2]*ax[2],
			               cov[1]*ax[0] + cov[3]*ax[1] + cov[4]*ax[2],
			               cov[2]*ax[0] + cov[4]*ax[1] + cov[5]*ax[2] };
			float l = sqrtf (n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
			if (l < 1e-6f) break;
			for (int k = 0; k < 3; k++) ax[k] = n[k] / l;
		}
		float tmin = 1e9f, tmax = -1e9f;
		for (int i = 0; i < 16; i++) {
			float t = (px[i][0] - mean[0])*ax[0] + (px[i][1] - mean[1])*ax[1] + (px[i][2] - mean[2])*ax[2];
			tmin = std::min (tmin, t), tmax = std::max (tmax, t);
		}
		float inset = (tmax - tmin) / 32.0f;
		tmin += inset, tmax -= inset;
		float c0[3], c1[3];
		for (int k = 0; k < 3; k++) c0[k] = mean[k] + ax[k]*tmax, c1[k] = mean[k] + ax[k]*tmin;
		uint16_t e0 = To565 (c0), e1 = To565 (c1);
		if (e0 < e1) std::swap (e0, e1);
		float pal[4][3];
		From565 (e0, pal[0]), From565 (e1, pal[1]);
		uint32_t idx = 0;
		if (e0 != e1) {
			for (int k = 0; k < 3; k++) {
				pal[2][k] = (2*pal[0][k] + pal[1][k]) / 3.0f;
				pal[3][k] = (pal[0][k] + 2*pal[1][k]) / 3.0f;
			}
			for (int i = 0; i < 16; i++) {
				int best = 0;
				float bd = 1e30f;
				for (int p = 0; p < 4; p++) {
					float dr = px[i][0] - pal[p][0], dg = px[i][1] - pal[p][1], db = px[i][2] - pal[p][2];
					float d = dr*dr*0.3f + dg*dg*0.59f + db*db*0.11f;
					if (d < bd) bd = d, best = p;
				}
				idx |= (uint32_t)best << (2*i);
			}
		}
		out.push_back ((uint8_t)e0), out.push_back ((uint8_t)(e0 >> 8));
		out.push_back ((uint8_t)e1), out.push_back ((uint8_t)(e1 >> 8));
		Put32 (out, idx);
	}

	// DDS file (DXT1 with all mip levels) from an RGB image (w, h powers of two)
	std::vector<uint8_t> EncodeDDS (std::vector<uint8_t> rgb, int w, int h)
	{
		int nmip = 1;
		for (int m = std::max (w, h); m > 4; m >>= 1) nmip++;
		std::vector<uint8_t> out = { 'D', 'D', 'S', ' ' };
		Put32 (out, 124);
		Put32 (out, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); // caps, height, width, pixel format, mipmap count, linear size
		Put32 (out, h);
		Put32 (out, w);
		Put32 (out, (uint32_t)(w / 4) * (h / 4) * 8);
		Put32 (out, 0);
		Put32 (out, nmip);
		for (int i = 0; i < 11; i++) Put32 (out, 0);
		Put32 (out, 32);          // pixel format
		Put32 (out, 0x4);         // fourcc
		out.insert (out.end(), { 'D', 'X', 'T', '1' });
		for (int i = 0; i < 5; i++) Put32 (out, 0);
		Put32 (out, 0x1000 | 0x400000 | 0x8); // texture, mipmap, complex
		for (int i = 0; i < 4; i++) Put32 (out, 0);

		for (int m = 0; m < nmip; m++) {
			for (int by = 0; by < h; by += 4)
				for (int bx = 0; bx < w; bx += 4) {
					const uint8_t *px[16];
					for (int j = 0; j < 4; j++)
						for (int i = 0; i < 4; i++)
							px[j*4 + i] = rgb.data() + ((size_t)std::min (by + j, h - 1) * w + std::min (bx + i, w - 1)) * 3;
					EncodeBlock (px, out);
				}
			if (m + 1 < nmip) { // next level: 2x2 average
				int w2 = std::max (1, w / 2), h2 = std::max (1, h / 2);
				std::vector<uint8_t> n ((size_t)w2 * h2 * 3);
				for (int y = 0; y < h2; y++)
					for (int x = 0; x < w2; x++)
						for (int k = 0; k < 3; k++) {
							int s = rgb[((size_t)(2*y) * w + 2*x) * 3 + k] + rgb[((size_t)(2*y) * w + 2*x + 1) * 3 + k] +
							        rgb[((size_t)(2*y + 1) * w + 2*x) * 3 + k] + rgb[((size_t)(2*y + 1) * w + 2*x + 1) * 3 + k];
							n[((size_t)y * w2 + x) * 3 + k] = (uint8_t)((s + 2) / 4);
						}
				rgb.swap (n);
				w = w2, h = h2;
			}
		}
		return out;
	}

	// .elv file (int16 samples, rows from south to north)
	std::vector<uint8_t> EncodeElv (const std::vector<float> &h, double latmin, double latmax, double lngmin, double lngmax)
	{
		float emin = *std::min_element (h.begin(), h.end()), emax = *std::max_element (h.begin(), h.end());
		double mean = 0.0;
		for (float v : h) mean += v;
		mean /= h.size();
		double offset = 0.5 * (emin + emax);
		double scale = std::max (0.1, (emax - emin) / 60000.0);
		scale = scale <= 0.1 ? 0.1 : scale <= 0.25 ? 0.25 : scale <= 0.5 ? 0.5 : 1.0;
		std::vector<uint8_t> out = { 'E', 'L', 'E', 1 };
		auto pi32 = [&](int32_t v) { Put32 (out, (uint32_t)v); };
		auto pf64 = [&](double v) { uint8_t b[8]; memcpy (b, &v, 8); out.insert (out.end(), b, b + 8); };
		pi32 (100);              // header size
		pi32 (-16);              // int16
		pi32 (ELEV_N), pi32 (ELEV_N);
		pi32 (1), pi32 (1);      // padding
		pf64 (scale), pf64 (offset);
		pf64 (latmin), pf64 (latmax), pf64 (lngmin), pf64 (lngmax);
		pf64 (emin), pf64 (emax), pf64 (mean);
		for (float v : h) {
			int16_t s = (int16_t)std::clamp (lround ((v - offset) / scale), -32767L, 32766L);
			out.push_back ((uint8_t)s), out.push_back ((uint8_t)((uint16_t)s >> 8));
		}
		return out;
	}

	// Web Mercator pixel coordinates at zoom z (256 px tiles)
	void Mercator (double lat, double lng, int z, double &x, double &y)
	{
		double n = 256.0 * (double)(1u << z);
		lat = std::clamp (lat, -85.05112878 * PI / 180.0, 85.05112878 * PI / 180.0);
		x = (lng / (2.0 * PI) + 0.5) * n;
		y = (1.0 - log (tan (lat) + 1.0 / cos (lat)) / PI) * 0.5 * n;
	}

#ifdef ORBITER_TERRAIN_STREAMING
	size_t CurlWrite (char *p, size_t size, size_t n, void *ud)
	{
		auto *v = (std::vector<uint8_t>*)ud;
		v->insert (v->end(), (uint8_t*)p, (uint8_t*)p + size * n);
		return size * n;
	}

	// HTTP GET; returns the status (0: no response)
	long HttpGet (const std::string &url, std::vector<uint8_t> &out)
	{
		thread_local CURL *h = nullptr;
		if (!h) h = curl_easy_init ();
		if (!h) return 0;
		out.clear();
		curl_easy_reset (h);
		curl_easy_setopt (h, CURLOPT_URL, url.c_str());
		curl_easy_setopt (h, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt (h, CURLOPT_TIMEOUT, 30L);
		curl_easy_setopt (h, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt (h, CURLOPT_USERAGENT, "Orbiter Space Flight Simulator (terrain streaming)");
		curl_easy_setopt (h, CURLOPT_WRITEFUNCTION, CurlWrite);
		curl_easy_setopt (h, CURLOPT_WRITEDATA, &out);
		curl_easy_setopt (h, CURLOPT_NOSIGNAL, 1L);
		if (curl_easy_perform (h) != CURLE_OK) return 0;
		long status = 0;
		curl_easy_getinfo (h, CURLINFO_RESPONSE_CODE, &status);
		return status;
	}
#endif
}

// =======================================================================

struct Stream::SrcTile {
	int w = 0, h = 0;
	std::vector<uint8_t> rgb;   // RGB sources
	std::vector<float> elev;    // elevation sources [m]
	bool empty = true;          // no data
};

int Source::MaxLevel () const
{
	// source pixels match Orbiter texels at level z+2 (surface: 512 texels
	// per tile) or z+3 (elevation: 256 intervals per tile)
	return layer == SURF ? maxZoom + 2 : maxZoom + 3;
}

void terrain::TileExtent (int lvl, int ilat, int ilng, double &latmin, double &latmax, double &lngmin, double &lngmax)
{
	int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	latmax = PI * 0.5 - PI * ilat / nlat;
	latmin = PI * 0.5 - PI * (ilat + 1) / nlat;
	lngmin = -PI + 2.0 * PI * ilng / nlng;
	lngmax = -PI + 2.0 * PI * (ilng + 1) / nlng;
}

Stream *terrain::Get () { return g_stream; }
void terrain::Set (Stream *s) { g_stream = s; }

Stream::Stream (const std::string &dir, double gb): cacheDir(dir), cacheBytes(gb * 1e9)
{
}

Stream *Stream::Create (const std::string &cfgFile, const std::string &cacheDir, double cacheGB)
{
#ifndef ORBITER_TERRAIN_STREAMING
	return nullptr;
#else
	std::ifstream ifs (cfgFile);
	if (!ifs) return nullptr;
	auto *s = new Stream (cacheDir, cacheGB);
	std::string line;
	// SOURCE <body> <Surf|Elev> <rgb|terrarium> <maxzoom> <minlevel> <latmin> <latmax> <lngmin> <lngmax> <id> <url> [attribution]
	while (std::getline (ifs, line)) {
		line = Trim (line);
		if (line.empty() || line[0] == ';' || line[0] == '#') continue;
		std::istringstream is (line);
		std::string kw, layer, enc;
		Source src;
		is >> kw >> src.body >> layer >> enc >> src.maxZoom >> src.minLevel >> src.latmin >> src.latmax >> src.lngmin >> src.lngmax >> src.id >> src.url;
		if (kw != "SOURCE" || !is) continue;
		std::getline (is, src.attribution);
		src.attribution = Trim (src.attribution);
		src.layer = (layer == "Elev" ? ELEV : SURF);
		src.enc = (enc == "terrarium" ? Source::TERRARIUM : Source::RGB);
		s->sources.push_back (src);
	}
	if (s->sources.empty()) {
		delete s;
		return nullptr;
	}
	curl_global_init (CURL_GLOBAL_DEFAULT);
	for (const Source &src : s->sources)
		LOGOUT ("Terrain streaming: %s %s up to level %d from %s", src.body.c_str(), LayerName (src.layer), src.MaxLevel(), src.id.c_str());
	for (int i = 0; i < WORKERS; i++) s->workers.emplace_back (&Stream::Worker, s);
	s->Prune ();
	return s;
#endif
}

Stream::~Stream ()
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		quit = true;
	}
	cv.notify_all();
	for (auto &w : workers) w.join();
}

std::string Stream::Path (const std::string &body, Layer layer, int lvl, int ilat, int ilng) const
{
	char buf[128];
	snprintf (buf, sizeof(buf), "/%s/%02d/%06d/%06d%s", LayerName (layer), lvl, ilat, ilng, LayerExt (layer));
	return cacheDir + "/" + body + buf;
}

const Source *Stream::Find (const std::string &body, Layer layer, int lvl, int ilat, int ilng) const
{
	double latmin, latmax, lngmin, lngmax;
	TileExtent (lvl, ilat, ilng, latmin, latmax, lngmin, lngmax);
	const double d = 180.0 / PI;
	for (const Source &s : sources) {
		if (s.body != body || s.layer != layer || lvl < s.minLevel || lvl > s.MaxLevel()) continue;
		// the tile must lie within the source's coverage
		if (latmin * d < s.latmin || latmax * d > s.latmax || lngmin * d < s.lngmin || lngmax * d > s.lngmax) continue;
		return &s;
	}
	return nullptr;
}

bool Stream::Covers (const std::string &body, Layer layer, int lvl, int ilat, int ilng) const
{
	return Find (body, layer, lvl, ilat, ilng) != nullptr;
}

State Stream::Request (const std::string &body, Layer layer, int lvl, int ilat, int ilng, std::string *path)
{
	std::string p = Path (body, layer, lvl, ilat, ilng);
	std::error_code ec;
	if (fs::exists (p, ec)) {
		if (path) *path = p;
		return READY;
	}
	if (!Find (body, layer, lvl, ilat, ilng)) return UNAVAILABLE;
	std::lock_guard<std::mutex> lock (mtx);
	auto f = failed.find (p);
	if (f != failed.end()) {
		if (Now() - f->second < RETRY_FAILED) return UNAVAILABLE;
		failed.erase (f);
	}
	if (queued.count (p)) {
		// (move it to the front: it is still wanted)
		for (auto it = queue.begin(); it != queue.end(); ++it)
			if (it->key == p) {
				if (it != queue.begin()) { Job j = *it; queue.erase (it); queue.push_front (j); }
				break;
			}
		return PENDING;
	}
	queue.push_front (Job{body, layer, lvl, ilat, ilng, p});
	queued.insert (p);
	while (queue.size() > MAX_QUEUE) { // (the oldest requests are dropped)
		queued.erase (queue.back().key);
		queue.pop_back();
	}
	cv.notify_one();
	return PENDING;
}

void Stream::Worker ()
{
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock (mtx);
			cv.wait (lock, [this] { return quit || !queue.empty(); });
			if (quit) return;
			job = queue.front();
			queue.pop_front();
		}
		bool ok = Make (job);
		{
			std::lock_guard<std::mutex> lock (mtx);
			queued.erase (job.key);
			if (!ok) failed[job.key] = Now();
		}
		if (ok) {
			version++;
			if (written > cacheBytes * 0.05) Prune ();
		}
	}
}

bool Stream::Make (const Job &job)
{
	const Source *s = Find (job.body, job.layer, job.lvl, job.ilat, job.ilng);
	if (!s) return false;
	return job.layer == SURF ? MakeSurf (*s, job) : MakeElev (*s, job);
}

std::shared_ptr<const Stream::SrcTile> Stream::Fetch (const Source &s, int z, int x, int y)
{
	char key[256];
	snprintf (key, sizeof(key), "%s/%d/%d/%d", s.id.c_str(), z, x, y);
	{
		std::lock_guard<std::mutex> lock (srcMtx);
		auto it = srcCache.find (key);
		if (it != srcCache.end()) {
			it->second.second = ++srcClock;
			return it->second.first;
		}
	}
	auto t = std::make_shared<SrcTile>();
#ifdef ORBITER_TERRAIN_STREAMING
	// the source file: from the disk cache, or downloaded (an empty file
	// marks a tile without data)
	std::string file = cacheDir + "/src/" + key;
	std::vector<uint8_t> data;
	std::error_code ec;
	if (fs::exists (file, ec)) {
		std::ifstream f (file, std::ios::binary);
		data.assign (std::istreambuf_iterator<char> (f), {});
	} else {
		std::string url = s.url;
		auto rep = [&](const char *k, int v) { size_t p = url.find (k); if (p != std::string::npos) url.replace (p, 3, std::to_string (v)); };
		rep ("{z}", z), rep ("{x}", x), rep ("{y}", y);
		long status = HttpGet (url, data);
		if (status == 0 || status >= 500 || status == 429) return nullptr; // (transient: not cached)
		if (status != 200) data.clear();
		WriteFileAtomic (file, data);
		written += data.size();
	}
	if (!data.empty()) {
		int w, h, n;
		uint8_t *px = stbi_load_from_memory (data.data(), (int)data.size(), &w, &h, &n, 3);
		if (px) {
			t->w = w, t->h = h;
			size_t np = (size_t)w * h;
			if (s.enc == Source::TERRARIUM) {
				t->elev.resize (np);
				for (size_t i = 0; i < np; i++)
					t->elev[i] = (float)(px[i*3] * 256.0 + px[i*3+1] + px[i*3+2] / 256.0 - 32768.0);
				t->empty = false;
			} else {
				t->rgb.assign (px, px + np * 3);
				// (a tile of a single colour has no data: e.g. "not available" fills)
				uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
				for (size_t i = 0; i < np; i++)
					for (int k = 0; k < 3; k++) lo[k] = std::min (lo[k], px[i*3+k]), hi[k] = std::max (hi[k], px[i*3+k]);
				t->empty = (hi[0] - lo[0] < 3 && hi[1] - lo[1] < 3 && hi[2] - lo[2] < 3);
			}
			stbi_image_free (px);
		}
	}
#endif
	std::lock_guard<std::mutex> lock (srcMtx);
	srcCache[key] = { t, ++srcClock };
	while (srcCache.size() > SRC_CACHE) {
		auto oldest = std::min_element (srcCache.begin(), srcCache.end(),
			[](const auto &a, const auto &b) { return a.second.second < b.second.second; });
		srcCache.erase (oldest);
	}
	return t;
}

// Samples a source at the given latitudes/longitudes (bilinear in the
// source's pixels); returns the fraction of samples without data
namespace {
	template <typename Get>
	double SampleGrid (int z, int nx, int ny, const std::vector<double> &lat, const std::vector<double> &lng, Get get)
	{
		int missing = 0;
		for (int i = 0; i < ny; i++)
			for (int j = 0; j < nx; j++) {
				double x, y;
				Mercator (lat[i], lng[j], z, x, y);
				if (!get (i, j, x - 0.5, y - 0.5)) missing++;
			}
		return (double)missing / ((double)nx * ny);
	}
}

bool Stream::MakeSurf (const Source &s, const Job &job)
{
	double latmin, latmax, lngmin, lngmax;
	TileExtent (job.lvl, job.ilat, job.ilng, latmin, latmax, lngmin, lngmax);
	int z = std::min (s.maxZoom, job.lvl - 2);
	std::vector<double> lat (SURF_N), lng (SURF_N);
	for (int i = 0; i < SURF_N; i++) {
		lat[i] = latmax - (i + 0.5) / SURF_N * (latmax - latmin);   // (rows from north)
		lng[i] = lngmin + (i + 0.5) / SURF_N * (lngmax - lngmin);
	}
	std::vector<uint8_t> rgb ((size_t)SURF_N * SURF_N * 3);
	int ntile = 1 << z;
	bool fetchFailed = false;
	std::shared_ptr<const SrcTile> last;
	int lastX = -1, lastY = -1;
	auto texel = [&](int px, int py, uint8_t *c) -> bool {
		int tx = ((px >> 8) % ntile + ntile) % ntile, ty = std::clamp (py >> 8, 0, ntile - 1);
		if (tx != lastX || ty != lastY) {
			last = Fetch (s, z, tx, ty);
			lastX = tx, lastY = ty;
			if (!last) fetchFailed = true;
		}
		if (!last || last->empty) return false;
		int u = std::clamp (px & 255, 0, last->w - 1), v = std::clamp (py - ty * 256, 0, last->h - 1);
		const uint8_t *p = last->rgb.data() + ((size_t)v * last->w + u) * 3;
		c[0] = p[0], c[1] = p[1], c[2] = p[2];
		return true;
	};
	double miss = SampleGrid (z, SURF_N, SURF_N, lat, lng, [&](int i, int j, double x, double y) {
		int x0 = (int)floor (x), y0 = (int)floor (y);
		double fx = x - x0, fy = y - y0;
		uint8_t c[4][3];
		bool ok = texel (x0, y0, c[0]) & texel (x0 + 1, y0, c[1]) & texel (x0, y0 + 1, c[2]) & texel (x0 + 1, y0 + 1, c[3]);
		uint8_t *o = rgb.data() + ((size_t)i * SURF_N + j) * 3;
		for (int k = 0; k < 3; k++)
			o[k] = (uint8_t)lround ((c[0][k] * (1 - fx) + c[1][k] * fx) * (1 - fy) + (c[2][k] * (1 - fx) + c[3][k] * fx) * fy);
		return ok;
	});
	if (fetchFailed || miss > 0.01) return false;
	auto dds = EncodeDDS (std::move (rgb), SURF_N, SURF_N);
	if (!WriteFileAtomic (job.key, dds)) return false;
	written += dds.size();
	return true;
}

bool Stream::MakeElev (const Source &s, const Job &job)
{
	double latmin, latmax, lngmin, lngmax;
	TileExtent (job.lvl, job.ilat, job.ilng, latmin, latmax, lngmin, lngmax);
	int z = std::min (s.maxZoom, job.lvl - 3);
	std::vector<double> lat (ELEV_N), lng (ELEV_N);
	for (int i = 0; i < ELEV_N; i++) {
		lat[i] = latmin + (i - 1) / 256.0 * (latmax - latmin);     // (rows from south)
		lng[i] = lngmin + (i - 1) / 256.0 * (lngmax - lngmin);
	}
	std::vector<float> h ((size_t)ELEV_N * ELEV_N);
	int ntile = 1 << z;
	bool fetchFailed = false;
	std::shared_ptr<const SrcTile> last;
	int lastX = -1, lastY = -1;
	auto sample = [&](int px, int py, float &e) -> bool {
		int tx = ((px >> 8) % ntile + ntile) % ntile, ty = std::clamp (py >> 8, 0, ntile - 1);
		if (tx != lastX || ty != lastY) {
			last = Fetch (s, z, tx, ty);
			lastX = tx, lastY = ty;
			if (!last) fetchFailed = true;
		}
		if (!last || last->empty) return false;
		int u = std::clamp (px & 255, 0, last->w - 1), v = std::clamp (py - ty * 256, 0, last->h - 1);
		e = last->elev[(size_t)v * last->w + u];
		return true;
	};
	double miss = SampleGrid (z, ELEV_N, ELEV_N, lat, lng, [&](int i, int j, double x, double y) {
		int x0 = (int)floor (x), y0 = (int)floor (y);
		double fx = x - x0, fy = y - y0;
		float e[4] = {0, 0, 0, 0};
		bool ok = sample (x0, y0, e[0]) & sample (x0 + 1, y0, e[1]) & sample (x0, y0 + 1, e[2]) & sample (x0 + 1, y0 + 1, e[3]);
		h[(size_t)i * ELEV_N + j] = (float)((e[0] * (1 - fx) + e[1] * fx) * (1 - fy) + (e[2] * (1 - fx) + e[3] * fx) * fy);
		return ok;
	});
	if (fetchFailed || miss > 0.0) return false;
	auto elv = EncodeElv (h, latmin, latmax, lngmin, lngmax);
	if (!WriteFileAtomic (job.key, elv)) return false;
	written += elv.size();
	return true;
}

// Keeps the cache within its size: the least recently written files go first
void Stream::Prune ()
{
	static std::mutex pruneMtx;
	std::unique_lock<std::mutex> lock (pruneMtx, std::try_to_lock);
	if (!lock.owns_lock()) return;
	written = 0;
	std::error_code ec;
	std::vector<std::pair<fs::file_time_type, std::pair<std::string, uintmax_t>>> files;
	uintmax_t total = 0;
	for (auto it = fs::recursive_directory_iterator (cacheDir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment (ec)) {
		if (!it->is_regular_file (ec)) continue;
		uintmax_t sz = it->file_size (ec);
		total += sz;
		files.push_back ({it->last_write_time (ec), {it->path().string(), sz}});
	}
	if (total <= cacheBytes) return;
	std::sort (files.begin(), files.end());
	uintmax_t target = (uintmax_t)(cacheBytes * 0.9);
	size_t removed = 0;
	for (const auto &f : files) {
		if (total <= target) break;
		if (fs::remove (f.second.first, ec)) total -= f.second.second, removed++;
	}
	LOGOUT ("Terrain streaming: cache pruned (%zu files removed)", removed);
}
