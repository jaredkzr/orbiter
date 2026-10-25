// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Ground scatter assets, generated when the client starts (no files):
// rocks (ellipsoids cut by fracture planes, with noise), grass tufts
// (curved tapered blades), shrubs (clusters of leafy lumps), conifers
// (trunk and jagged tiers) and broadleaf trees (trunk, branches, lumpy
// crown), each in several variants, with procedural textures. All
// generation is deterministic (seeded).
//
// Meshes are in the object frame of the ground: y up, x east, z north,
// origin on the ground (rocks are partly buried).
//
// ScatterGallery (ORBITER_VK_SCATTER_GALLERY=1) lays the assets of a
// planet out in rows next to the focus vessel, for previews.
// =======================================================================

#ifndef __SCATTERASSETS_H
#define __SCATTERASSETS_H

#include "VkMesh.h"
#include "VkMath.h"
#include <memory>
#include <string>
#include <vector>

class Scene;
class ShadowMap;
enum class RenderPass;

namespace vkscatter {

	enum class Kind { ROCK, GRASS, SHRUB, CONIFER, BROADLEAF };

	struct Asset {
		Kind kind;
		std::string name;
		std::unique_ptr<VkMesh> mesh;            // full detail
		std::vector<std::unique_ptr<VkMesh>> lods; // coarser levels of detail (may be empty)
		double radius;  // horizontal extent [m]
		double height;  // [m]
		/// level of detail for a size on screen [pixels]
		const VkMesh *ForPixels (double px) const
		{
			if (lods.empty() || px > 150.0) return mesh.get();
			return (px > 40.0 || lods.size() < 2) ? lods[0].get() : lods[1].get();
		}
	};

	/// Rock colouring of a body
	enum class RockStyle { GRANITE, REGOLITH, MARS };

	class Library {
	public:
		Library (vkc::Context *ctx, TextureManager *texmgr);
		~Library ();
		/// The assets of a body: rocks made with Blender (Utils/ScatterAssets,
		/// listed in Config/Scatter/<body>_rocks.cfg) if there are, else
		/// generated in the given style; vegetation if asked
		void Generate (const char *body, RockStyle style, bool vegetation);
		const std::vector<Asset> &Assets () const { return assets; }

	private:
		vkc::Context *ctx;
		TextureManager *texmgr;
		std::vector<Asset> assets;
		std::vector<VkTexture*> textures;
		VkTexture *Texture (uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba);
		bool LoadRocks (const char *body);
	};

	/// Preview of the assets in rows on the ground next to the focus vessel
	class Gallery {
	public:
		Gallery (Scene *scene, OBJHANDLE hPlanet);
		~Gallery ();
		static bool Enabled ();
		void Render (RenderPass pass);
		void RenderShadow (ShadowMap &sm);

	private:
		struct Item { const Asset *asset; VECTOR3 pos; MATRIX3 rot; };
		void Place ();
		Scene *scene;
		OBJHANDLE hPlanet;
		std::unique_ptr<Library> lib;
		std::vector<Item> items;  // positions and rotations in the planet frame
		bool placed = false;
	};

} // namespace vkscatter

#endif // !__SCATTERASSETS_H
