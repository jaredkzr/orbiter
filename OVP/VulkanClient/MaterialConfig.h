// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Physically based material parameters, derived from Orbiter mesh
// materials and refined by per-mesh material configuration files
// (Config/GC/*.cfg: MESH, MATERIAL, SPECULAR, SMOOTHNESS, METALNESS, REFLECT)
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

class MaterialConfig {
public:
	/// Read all configuration files in a directory
	void Load (const char *dir);
	/// Apply the configuration for a mesh file (if any) to a material
	bool Apply (const std::string &meshName, int matIdx, PBRParams &p) const;
	size_t MeshCount () const { return meshes.size(); }

private:
	struct Override {
		bool hasSmooth = false, hasMetal = false, hasReflect = false, hasSpec = false;
		float smooth = 0.0f, metal = 0.0f, reflect = 0.0f, spec = 0.0f, power = 0.0f;
	};
	static std::string Key (const std::string &meshName);
	std::map<std::string, std::map<int, Override>> meshes;
};

#endif // !__MATERIALCONFIG_H
