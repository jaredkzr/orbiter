// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The ground scatter of a planet drawn around the camera: rocks, boulders,
// trees, shrubs and grass where the core places them (oapiScatter*: the
// same objects vessels collide with). Cells of each layer within its range
// are read from the core (a time budget per frame, nearest first) and
// kept; each frame the instances in view are sorted into batches per mesh
// and level of detail, faded out towards the end of the range, and drawn
// instanced (depth prepass, opaque pass, shadow maps). Instances near the
// camera also go into the ray tracing scene.
// =======================================================================

#ifndef __SCATTERFIELD_H
#define __SCATTERFIELD_H

#include "ScatterAssets.h"
#include "VkContext.h"
#include <unordered_map>

namespace vkscatter {

	class Field {
	public:
		Field (Scene *scene, OBJHANDLE hPlanet);
		~Field ();
		/// the planet has ground scatter
		bool Active () const { return active; }
		/// Read cells, sort the instances, upload them (before the shadow maps)
		void Prepare ();
		void Render (RenderPass pass);
		void RenderShadow (ShadowMap &sm);

	private:
		struct Kind {          // a core asset
			const Asset *asset = nullptr; // its meshes (null: not drawn)
			double size = 0.0;   // largest extent at scale 1 [m]
			double scale = 1.0;  // mesh -> core size (plants)
			int kind = 0;        // SCATTERKIND
		};
		struct Inst {
			VECTOR3 pos;         // planet frame [m]
			float R[9];          // object -> planet frame, scaled (row-major)
			float size;          // largest extent [m]
			uint16_t kind;
		};
		struct Cell {
			int layer;
			std::vector<Inst> inst;
			VECTOR3 centre{};    // planet frame
			double radius = 0.0;
			uint64_t used = 0;   // frame
		};
		struct Batch { const VkMesh *mesh; uint32_t first, count; };

		void Fetch (int layer, int row, int col, Cell &c);
		VECTOR3 CellCentre (int layer, int row, int col) const;

		Scene *scene;
		OBJHANDLE hPlanet;
		double R;
		bool active = false;
		std::unique_ptr<Library> lib;
		std::vector<Kind> kinds;
		std::vector<SCATTERLAYER> layers;
		double maxRange = 0.0;
		std::unordered_map<uint64_t, Cell> cells;
		unsigned generation = 0;
		uint64_t frame = 0;

		// this frame
		std::vector<Batch> batches, shadowBatches;
		struct RTInst { const VkMesh *mesh; Mat4 M; }; // M: object -> global frame relative to the camera
		std::vector<RTInst> rtInst;
		VECTOR3 origin{};      // camera position the instances are relative to
		vkc::Buffer buf[vkc::FRAMES_IN_FLIGHT];
	};

} // namespace vkscatter

#endif // !__SCATTERFIELD_H
