// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The building kit of the procedural cities: how the boxes the core places
// (oapiCityCell) look.
//
// Shapes: a few vertices each (walls, roofs: flat with parapets, gable,
// hip, mansard, setback towers, sheds), stretched to each building by the
// vertex shader (building.glsl): a vertex's position is a share of the
// building's width, depth and walls' height, an offset in metres (eaves,
// parapets, insets), a share of the roof's height and an x offset in
// shares of the depth (hip ridges).
//
// Materials: a texture array made here at start (no files): per material
// an albedo layer and a normal and roughness layer, tiles of a few metres
// (plaster, bricks, concrete, stone, siding, metal cladding; roof tiles,
// slates, shingles, membranes, metal). Windows are drawn by the shader
// (building.frag) on the grid of storeys and bays.
//
// Looks: by the region's style (Config/Cities/<planet>.cfg STYLE names)
// and the building's kind, chosen from its seed.
// =======================================================================

#ifndef __CITYKIT_H
#define __CITYKIT_H

#include "VkContext.h"
#include "OrbiterAPI.h"
#include <cstdint>
#include <string>
#include <vector>

class TextureManager;
struct VkTexture;

namespace vkcity {

	/// Vertex of a shape (Scene::BUILDING_VERTEX bytes; building.glsl)
	struct BVertex {
		float unit[3];   // x, z: share of width, depth (-0.5 to 0.5); y: share of the walls' height
		float metric[3]; // offset [m]
		float shape[4];  // share of the roof's height, x offset in shares of the depth, part, facade's share of its side
		float normal[3]; // building frame (0: the surface's)
	};
	static_assert (sizeof(BVertex) == 52, "BVertex");

	/// Instance (Scene::BUILDING_INSTANCE bytes; building.vert)
	struct BInst {
		float row[12];   // building frame (x along the front, y up, z back; unscaled) and position, rows of 3x4
		float dim[4];    // width, depth, walls' height (from the base), roof height [m]
		float prm[2];    // storey height, part below the ground [m]
		uint32_t bits[2]; // looks (below)
	};
	static_assert (sizeof(BInst) == 80, "BInst");

	// BInst::bits[0]: storeys (8 bits), wall material (4), roof material (4),
	// window style (4), roof colour (4), flags: shops on the ground floor,
	// a door on the front, coated glass, front on -x (else -z), decoration (4)
	// BInst::bits[1]: wall colour (RGB 565), seed (16)

	/// parts of a shape (building.frag)
	enum Part { P_FACADE = 0, P_WALL = 1, P_ROOF_FLAT = 2, P_ROOF = 3, P_TRIM = 4, P_SOFFIT = 5 };

	/// shapes
	enum Shape { S_BOX, S_FLAT, S_PLANT, S_GABLE, S_HIP, S_MANSARD, S_SETBACK, S_SETBACK2, S_PYRAMID, S_SHED, NSHAPE };

	class Kit {
	public:
		Kit (vkc::Context *ctx, TextureManager *tm);
		~Kit ();
		VkBuffer VertexBuffer () const { return vb.buffer; }
		VkBuffer IndexBuffer () const { return ib.buffer; }
		struct Range { uint32_t first, count; };
		const Range &Indices (int shape) const { return range[shape]; }
		VkDescriptorSet Set () const;

		/// the styles of a planet's cities (indices as CITYBUILDING::style)
		void SetStyles (OBJHANDLE hPlanet);
		/// A building's instance (its frame and looks; its position, relative
		/// to an anchor, is the caller's) and its shape; far: a simpler one
		int Make (const CITYBUILDING &b, BInst &inst, bool far) const;
		/// the simpler shape for far buildings
		static int Far (int shape);
		/// Boxes for the ray tracing scene (the unit box: x, z -0.5 to 0.5, y 0
		/// to 1): rows of their 3x4 transforms (scaled) appended to rows; below
		/// the roofs (a flat roof is not inside its box), tiers of setbacks
		static void RTBoxes (int shape, const BInst &inst, std::vector<float> &rows);

	private:
		void MakeShapes ();
		void MakeTextures ();
		vkc::Context *ctx;
		TextureManager *tm;
		vkc::Buffer vb, ib;
		Range range[NSHAPE] = {};
		VkTexture *tex = nullptr;
		std::vector<int> styles; // per CITYBUILDING::style: region (Kit.cpp)
	};

} // namespace vkcity

#endif // !__CITYKIT_H
