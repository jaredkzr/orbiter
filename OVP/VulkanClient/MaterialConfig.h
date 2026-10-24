// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Physically based material parameters, derived from Orbiter mesh
// materials and refined by per-mesh material configuration files
// (Config/GC/*.cfg: MESH, MATERIAL, SPECULAR, SMOOTHNESS, METALNESS, REFLECT).
// Files in Config/VK may also replace textures (Vulkan client only):
//   TEXTURE <file>                  texture of the groups using the material
//   SIDE_TEXTURE <dx dy dz> <file>  texture of the triangles facing the
//                                   direction (mesh frame), e.g. the cell
//                                   side of a two-sided solar panel
//   SIDE_SMOOTHNESS, SIDE_METALNESS their surface parameters
// =======================================================================

#ifndef __MATERIALCONFIG_H
#define __MATERIALCONFIG_H

#include "OrbiterAPI.h"
#include <map>
#include <string>

/// Shading parameters of a mesh material
struct PBRParams {
	float metalness = 0.0f;
	float roughness = 1.0f;
	float reflectivity = 0.0f; ///< scale of environment reflections
	float f0 = 1.0f;           ///< scale of the dielectric Fresnel reflectance (0.04)

	/// Defaults for a legacy material (specular colour and power)
	static PBRParams FromMaterial (const MATERIAL &m);
};

/// Texture replacements of a material (paths relative to Textures)
struct MaterialTextures {
	std::string texture;     ///< replaces the groups' texture (empty: none)
	std::string sideTexture; ///< for triangles facing sideDir (empty: none)
	float sideDir[3] = { 0, 0, 0 };
	PBRParams sidePBR;       ///< parameters of those triangles
};

class MaterialConfig {
public:
	/// Read all configuration files in a directory
	void Load (const char *dir);
	/// Apply the configuration for a mesh file (if any) to a material
	bool Apply (const std::string &meshName, int matIdx, PBRParams &p) const;
	/// Texture replacements for a material (false: none). p: the material's
	/// parameters, the base of the side's
	bool Textures (const std::string &meshName, int matIdx, const PBRParams &p, MaterialTextures &t) const;
	size_t MeshCount () const { return meshes.size(); }

private:
	struct Override {
		bool hasSmooth = false, hasMetal = false, hasReflect = false, hasSpec = false;
		float smooth = 0.0f, metal = 0.0f, reflect = 0.0f, spec = 0.0f, power = 0.0f;
		std::string texture, sideTexture;
		float sideDir[3] = { 0, 0, 0 };
		bool hasSideSmooth = false, hasSideMetal = false;
		float sideSmooth = 0.0f, sideMetal = 0.0f;
	};
	static std::string Key (const std::string &meshName);
	std::map<std::string, std::map<int, Override>> meshes;
};

#endif // !__MATERIALCONFIG_H
