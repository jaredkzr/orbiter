// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Planet surfaces from tiled quadtree layers (Orbiter 2016 format):
// level-of-detail selection, background tile loading, elevation meshes
// =======================================================================

#ifndef __PLANETSURFACE_H
#define __PLANETSURFACE_H

#include "VkContext.h"
#include "VkMath.h"
#include "TileArchive.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Scene;
class VkTexture;
class TextureManager;

/// The tile layers of one planet, shared with the loader thread
struct PlanetLayers {
	std::unique_ptr<TileLayer> surf, mask, elev, elevMod;
	std::atomic<bool> alive{ true };
};

/// Elevation grid of one tile: 259x259 heights [m], rows from south to
/// north, columns from west to east, with a one-node padding border
struct ElevGrid {
	static constexpr int N = 259;
	std::vector<float> h;
	float emin = 0.0f, emax = 0.0f;
};

/// Background thread reading and decompressing tiles
class TileLoader {
public:
	enum Kind { SURF, MASK, ELEV };
	struct Request {
		std::shared_ptr<PlanetLayers> planet;
		Kind kind;
		int lvl, ilat, ilng;
	};
	struct Result {
		Request req;
		bool ok = false;
		std::vector<uint8_t> data;  // texture file (SURF, MASK)
		ElevGrid elev;              // ELEV
	};

	TileLoader ();
	~TileLoader ();
	void Push (Request &&req);
	/// Take a finished request of the given planet
	bool Pop (Result &res, const PlanetLayers *owner);
	/// Discard all requests of a planet
	void Drop (const PlanetLayers *owner);
	size_t Outstanding () const { return outstanding; }

private:
	void Run ();
	void Process (const Request &req, Result &res);
	std::thread thread;
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<Request> queue;
	std::deque<Result> done;
	std::atomic<size_t> outstanding{ 0 };
	bool quit = false;
};

/// Shared GPU resources for rendering tiles (owned by the scene)
class TileRenderer {
public:
	static constexpr int GRID = 32; // quads per tile edge

	bool Init (vkc::Context *ctx, TextureManager *texmgr, VkDescriptorSetLayout frameLayout);
	void Shutdown ();
	TileLoader &Loader () { return *loader; }
	VkPipeline Pipeline () const { return pipeline; }
	VkPipelineLayout Layout () const { return layout; }
	VkBuffer IndexBuffer () const { return ibuf.buffer; }
	uint32_t IndexCount () const { return nIdx; }

private:
	vkc::Context *ctx = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	vkc::Buffer ibuf;
	uint32_t nIdx = 0;
	std::unique_ptr<TileLoader> loader;
};

class PlanetSurface {
public:
	/// Returns nullptr if the planet has no tiled surface data
	static std::unique_ptr<PlanetSurface> Create (OBJHANDLE hPlanet, Scene *scene);
	~PlanetSurface ();

	/// Accept loaded tiles (call before rendering)
	void Update ();
	void Render ();

private:
	struct Node {
		int lvl, ilat, ilng;
		Node *parent = nullptr;
		std::unique_ptr<Node> child[4];
		double latmin, latmax, lngmin, lngmax;
		VECTOR3 centre;       // tile centre on the mean sphere, planet frame
		double angRadius;     // angular radius of the tile [rad]
		VkTexture *surf = nullptr, *mask = nullptr;
		std::unique_ptr<ElevGrid> elev;
		uint8_t surfState = 0, maskState = 0, elevState = 0; // 0: unknown, 1: requested, 2: loaded, 3: missing
		vkc::Buffer vbuf;
		int meshElevLvl = -1; // level of the elevation tile the mesh was built from (0: sphere)
		uint64_t lastUsed = 0;
	};

	PlanetSurface (OBJHANDLE hPlanet, Scene *scene, std::shared_ptr<PlanetLayers> layers);
	Node *NewNode (int lvl, int ilat, int ilng, Node *parent);
	void FreeNode (Node *n);
	void PruneChildren (Node *n);
	void Select (Node *n);
	void Request (Node *n, TileLoader::Kind kind);
	Node *Find (int lvl, int ilat, int ilng) const;
	const Node *ElevSource (const Node *n) const;
	double SampleElev (const Node *src, double lat, double lng) const;
	void BuildMesh (Node *n, const Node *elevSrc);
	void DrawNode (Node *n);

	OBJHANDLE hPlanet;
	Scene *scene;
	vkc::Context *ctx;
	TextureManager *texmgr;
	TileRenderer *tr;
	std::shared_ptr<PlanetLayers> layers;
	std::unique_ptr<Node> root[2];
	double radius;
	double maxElev = 0.0;
	int maxLevel = 18;
	uint64_t frame = 0;
	int requestsThisFrame = 0;

	// per-frame camera state in the planet frame
	VECTOR3 ppos{};      // planet global position
	MATRIX3 prot{};      // planet rotation (planet frame -> global)
	VECTOR3 camLocal{};  // camera position, planet frame
	double camDist = 0.0;
	double pixPerRad = 1.0;
	std::vector<Node*> drawList;
};

#endif // !__PLANETSURFACE_H
