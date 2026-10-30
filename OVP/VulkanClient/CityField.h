// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The procedural cities of a planet drawn around the camera: the buildings
// the core places (oapiCity*: the same ones vessels collide with).
//
// Cells are read in tiers by distance: near ones with all their buildings,
// farther ones with only the taller. The buildings in reach go into a
// static instance buffer (positions relative to an anchor, planet frame),
// built over a few frames when the camera has moved from the anchor, and
// drawn without per-instance work: batches by kind and colour. The near
// ones also cast into the shadow maps and go into the ray tracing scene.
//
// The streets about the anchor are drawn into a road map (two levels: fine
// near the anchor, coarse further out) on a worker thread, which the tile
// shader lays over the terrain (Scene::SetCityMap), with the light of their
// street lamps; the lamps themselves glow at night (Scene::DrawLamps).
// Windows light up at night by the local time (building.frag).
// =======================================================================

#ifndef __CITYFIELD_H
#define __CITYFIELD_H

#include "VkContext.h"
#include "VkMesh.h"
#include "Scene.h"
#include "CityKit.h"
#include <future>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vkcity {

	class Field {
	public:
		Field (Scene *scene, OBJHANDLE hPlanet);
		~Field ();
		/// the planet has cities
		bool Active () const { return active; }
		/// Read cells, continue the instance set (before the shadow maps)
		void Prepare ();
		void Render (RenderPass pass);
		void RenderShadow (ShadowMap &sm);

	private:
		struct Inst {
			VECTOR3 pos;   // planet frame [m]
			BInst bi;      // (its position relative to the set's anchor filled in there)
			uint8_t shape;
		};
		struct Cell {
			int tier;
			std::vector<Inst> inst;
			unsigned set = 0; // last set it went into
		};
		struct Batch { uint32_t first, count; int shape; };

		struct Set {
			unsigned id = 0;
			VECTOR3 anchor{};
			vkc::Buffer buf;
			std::vector<Batch> batches, shadow;
			std::vector<float> rt; // near buildings for ray tracing: boxes (rows of 3x4, scaled; relative to the anchor)
		} set;
		struct Build {
			bool active = false;
			unsigned id = 0;
			VECTOR3 anchor{};
			std::vector<uint64_t> keys;
			size_t next = 0;
			std::vector<std::vector<BInst>> bucket, sbucket; // per shape
			std::vector<float> rtRows;                        // near buildings (Set::rt)
		} build;
		void Continue ();

		// the road map
		struct Seg { VECTOR3 a, b; float width; int kind; };
		struct RoadMap {
			VECTOR3 centre{}, X{}, Y{};
			std::vector<uint8_t> fine, coarse, far; // (far: the built-up share in r)
			std::vector<float> lamps; // street lamps: position relative to centre (planet frame), white LED (1) or sodium (0)
		};
		std::unordered_map<uint64_t, std::vector<Seg>> streets; // per cell (row, column)
		std::vector<Seg> roadSegs;          // of the set being built
		std::future<RoadMap> roadJob;
		VkTexture *roadTex[3] = {};         // fine, coarse, far
		vkc::Buffer lampBuf;                // the street lamps (RoadMap::lamps)
		uint32_t lampCount = 0;
		VECTOR3 lampCentre{};
		void StartRoads (const VECTOR3 &anchor);
		void FinishRoads ();
		void Fetch (uint64_t key, Cell &c);
		VECTOR3 CellCentre (int row, int col) const;

		Scene *scene;
		OBJHANDLE hPlanet;
		double R, cellSize = 0.0;
		bool active = false;
		std::unique_ptr<VkMesh> box;   // (ray tracing)
		std::unique_ptr<Kit> kit;
		std::unordered_map<uint64_t, Cell> cells;
		unsigned generation = 0;
		uint64_t frame = 0;
		std::vector<std::pair<const VkMesh*, Mat4>> rtThis; // this frame
		VECTOR3 origin{};
	};

} // namespace vkcity

#endif // !__CITYFIELD_H
