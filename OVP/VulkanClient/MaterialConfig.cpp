// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MaterialConfig.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
	float Luminance (float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
}

PBRParams PBRParams::FromMaterial (const MATERIAL &m)
{
	PBRParams p;
	// Blinn-Phong exponent -> microfacet roughness
	float power = std::max (m.power, 0.0f);
	p.roughness = std::clamp (std::sqrt (2.0f / (power + 2.0f)), 0.08f, 1.0f);
	if (power <= 0.0f) p.roughness = 1.0f;
	float spec = Luminance (m.specular.r, m.specular.g, m.specular.b);
	// no specular colour: matte, but still with the Fresnel effect of dielectrics
	p.f0 = std::clamp (0.25f + spec, 0.25f, 2.0f);
	p.reflectivity = std::clamp (spec * 0.5f, 0.0f, 1.0f) * (1.0f - p.roughness);
	return p;
}

std::string MaterialConfig::Key (const std::string &meshName)
{
	std::string k = meshName;
	for (auto &c : k) c = (c == '/') ? '\\' : (char)tolower ((unsigned char)c);
	if (k.size() > 4 && k.compare (k.size() - 4, 4, ".msh") == 0) k.resize (k.size() - 4);
	return k;
}

void MaterialConfig::Load (const char *dir)
{
	std::error_code ec;
	for (auto &e : std::filesystem::directory_iterator (dir, ec)) {
		std::string fn = e.path().filename().string();
		std::string lfn = fn;
		for (auto &c : lfn) c = (char)tolower ((unsigned char)c);
		if (lfn.size() < 4 || lfn.compare (lfn.size() - 4, 4, ".cfg") || lfn.find (".atm.") != std::string::npos ||
			lfn.find ("_atm.") != std::string::npos)
			continue;
		std::ifstream f (e.path());
		std::string line, mesh;
		int mat = -1;
		while (std::getline (f, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::istringstream ss (line);
			std::string tag;
			if (!(ss >> tag) || tag[0] == ';') continue;
			if (tag == "MESH") { ss >> mesh; mesh = Key (mesh); mat = -1; continue; }
			if (tag == "MATERIAL") { ss >> mat; continue; }
			if (mesh.empty() || mat < 0) continue;
			Override &o = meshes[mesh][mat];
			float a = 0, b = 0, c = 0, d = 0;
			if (tag == "SMOOTHNESS" && (ss >> a)) o.hasSmooth = true, o.smooth = a;
			else if (tag == "METALNESS" && (ss >> a)) o.hasMetal = true, o.metal = a;
			else if (tag == "REFLECT" && (ss >> a >> b >> c)) o.hasReflect = true, o.reflect = Luminance (a, b, c);
			else if (tag == "SPECULAR" && (ss >> a >> b >> c >> d)) o.hasSpec = true, o.spec = Luminance (a, b, c), o.power = d;
			else if (tag == "TEXTURE") ss >> o.texture;
			else if (tag == "SIDE_TEXTURE" && (ss >> a >> b >> c >> o.sideTexture)) o.sideDir[0] = a, o.sideDir[1] = b, o.sideDir[2] = c;
			else if (tag == "SIDE_SMOOTHNESS" && (ss >> a)) o.hasSideSmooth = true, o.sideSmooth = a;
			else if (tag == "SIDE_METALNESS" && (ss >> a)) o.hasSideMetal = true, o.sideMetal = a;
		}
	}
}

bool MaterialConfig::Apply (const std::string &meshName, int matIdx, PBRParams &p) const
{
	auto it = meshes.find (Key (meshName));
	if (it == meshes.end()) return false;
	auto jt = it->second.find (matIdx);
	if (jt == it->second.end()) return false;
	const Override &o = jt->second;
	if (o.hasSpec) {
		MATERIAL m{};
		m.specular = { o.spec, o.spec, o.spec, 1.0f };
		m.power = o.power;
		float refl = p.reflectivity;
		p = PBRParams::FromMaterial (m);
		p.reflectivity = refl;
	}
	if (o.hasSmooth) p.roughness = std::clamp (1.0f - o.smooth, 0.04f, 1.0f);
	if (o.hasMetal) p.metalness = std::clamp (o.metal, 0.0f, 1.0f);
	if (o.hasReflect) p.reflectivity = std::clamp (o.reflect * 4.0f, 0.0f, 1.0f);
	else if (o.hasSmooth || o.hasMetal) p.reflectivity = std::max (p.reflectivity, 1.0f - p.roughness);
	return true;
}

bool MaterialConfig::Textures (const std::string &meshName, int matIdx, const PBRParams &p, MaterialTextures &t) const
{
	auto it = meshes.find (Key (meshName));
	if (it == meshes.end()) return false;
	auto jt = it->second.find (matIdx);
	if (jt == it->second.end()) return false;
	const Override &o = jt->second;
	if (o.texture.empty() && o.sideTexture.empty()) return false;
	t.texture = o.texture;
	t.sideTexture = o.sideTexture;
	for (int i = 0; i < 3; i++) t.sideDir[i] = o.sideDir[i];
	t.sidePBR = p;
	if (o.hasSideSmooth) t.sidePBR.roughness = std::clamp (1.0f - o.sideSmooth, 0.04f, 1.0f);
	if (o.hasSideMetal) t.sidePBR.metalness = std::clamp (o.sideMetal, 0.0f, 1.0f);
	if (o.hasSideSmooth || o.hasSideMetal) t.sidePBR.reflectivity = std::max (t.sidePBR.reflectivity, 1.0f - t.sidePBR.roughness);
	return true;
}
