// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Streamed terrain: planetary surface tiles finer than the installed
// texture archives, made on demand from online data sets (map tiles in
// the Web Mercator projection, e.g. elevation from the Tilezen terrain
// tiles and imagery from the USGS National Map) and written in Orbiter's
// own tile formats (surface: DXT1 DDS, elevation: .elv) into a disk
// cache (Cache/Terrain/<body>/<layer>/<lvl>/<ilat>/<ilng>.<ext>), which
// is limited in size (least recently used tiles are removed). The graphics
// client and the elevation manager (ground contact) read the cache.
//
// The sources are listed in Config/TerrainStream.cfg.
// =======================================================================

#ifndef __TERRAINSTREAM_H
#define __TERRAINSTREAM_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace terrain {

	enum Layer { SURF = 0, ELEV = 1 };
	enum State { UNAVAILABLE = -1, PENDING = 0, READY = 1 };

	struct Source {
		std::string body;
		Layer layer;
		enum Encoding { RGB, TERRARIUM, S2COG } enc = RGB;
		std::string url;         // template with {z}, {x}, {y} (map tiles) or {cell}, {latband} (S2COG)
		int maxZoom = 0;         // finest zoom level of the source (S2COG: finest Orbiter level)
		int minLevel = 10;       // coarsest Orbiter level to make
		double latmin = -90, latmax = 90, lngmin = -180, lngmax = 180; // coverage [deg]
		std::string id;          // cache folder name
		std::string attribution;
		int MaxLevel () const;   // finest Orbiter level the source supports
	};

	class Stream {
	public:
		/// Reads the source list; returns null if there are no sources (or
		/// streaming is not available in this build)
		static Stream *Create (const std::string &cfgFile, const std::string &cacheDir, double cacheGB);
		~Stream ();

		/// State of a tile: READY (in the cache; path set), PENDING (queued
		/// or being made) or UNAVAILABLE (no source, or it failed). Requests
		/// a missing tile. Thread-safe.
		State Request (const std::string &body, Layer layer, int lvl, int ilat, int ilng, std::string *path = nullptr);

		/// Can a source provide this tile at all?
		bool Covers (const std::string &body, Layer layer, int lvl, int ilat, int ilng) const;

		/// Cache path of a tile
		std::string Path (const std::string &body, Layer layer, int lvl, int ilat, int ilng) const;

		/// Incremented whenever a tile was written
		unsigned Version () const { return version; }

		const std::vector<Source> &Sources () const { return sources; }

	private:
		Stream (const std::string &cacheDir, double cacheGB);
		struct Job { std::string body; Layer layer; int lvl, ilat, ilng; std::string key; };
		struct SrcTile;
		void Worker ();
		bool Make (const Job &job);
		bool MakeSurf (const Source &s, const Job &job);
		bool MakeElev (const Source &s, const Job &job);
		const Source *Find (const std::string &body, Layer layer, int lvl, int ilat, int ilng) const;
		std::shared_ptr<const SrcTile> Fetch (const Source &s, int z, int x, int y);
		bool MakeSurfCog (const Source &s, const Job &job);
		bool FillHoles (const std::string &body, const Job &job, std::vector<uint8_t> &rgb, const std::vector<uint8_t> &valid);
		struct CogHeader;
		std::shared_ptr<const CogHeader> CogHead (const Source &s, int clat, int clng);
		std::shared_ptr<const SrcTile> CogBlock (const Source &s, const CogHeader &h, int clat, int clng, int k, int bx, int by);
		std::shared_ptr<const SrcTile> Cached (const std::string &key);
		void Cache (const std::string &key, std::shared_ptr<const SrcTile> t, size_t bytes);
		void Prune ();

		std::vector<Source> sources;
		std::string cacheDir;
		double cacheBytes;

		std::mutex mtx;
		std::condition_variable cv;
		std::deque<Job> queue;         // newest first
		std::set<std::string> queued;  // keys of queued and running jobs
		std::map<std::string, double> failed; // keys of failed tiles (time of failure)
		std::vector<std::thread> workers;
		std::atomic<bool> quit{false};
		std::atomic<unsigned> version{0};
		std::atomic<unsigned long long> written{0}; // bytes written since the last pruning

		std::mutex srcMtx;             // in-memory cache of source tiles
		struct CacheEntry { std::shared_ptr<const SrcTile> tile; unsigned used; size_t bytes; };
		std::map<std::string, CacheEntry> srcCache;
		size_t srcBytes = 0;
		unsigned srcClock = 0;
		std::map<std::string, std::shared_ptr<const CogHeader>> cogHeads;
	};

	/// The stream of the application (null if none)
	Stream *Get ();
	void Set (Stream *s);

	/// Orbiter tile extent [rad]
	void TileExtent (int lvl, int ilat, int ilng, double &latmin, double &latmax, double &lngmin, double &lngmax);

} // namespace terrain

#endif // !__TERRAINSTREAM_H
