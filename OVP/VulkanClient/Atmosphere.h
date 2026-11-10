// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Planetary atmospheres: scattering parameters, sky rendering, and the
// uniform data used for aerial perspective on the planet surface
// =======================================================================

#ifndef __ATMOSPHERE_H
#define __ATMOSPHERE_H

#include "VkContext.h"
#include "OrbiterAPI.h"
#include <vector>
#include "OceanFft.h"
#include <cmath>
#include <cstdint>
#include <array>

class Scene;

/// Shared resources for atmosphere rendering (owned by the scene)
/// gas giants: depth of the scattering atmosphere's floor below the 1-bar level [m]
constexpr double GG_FLOOR_DEPTH = 200e3;

static constexpr int WAVE_COUNT = 14; // as WAVE_COUNT in atmosphere.glsl (keep in sync)
// short crests (pass 2, W2/W3): each wave's amplitude varies along its crest by m = 1 + D n(u), n a smooth
// random function (1-D value noise, WAVE_ENV_CELLS cells per period of its phase u = kp . x + psi, |kp| =
// WAVE_ENV_K k across the wave: cells of about 2.25 wavelengths, repeating after 18), normalised so that the
// sea's slope variance holds. (A single cosine, W2, made a regular lattice of patches with the wave: a
// crosshatch.) WaveEnvelope here and in tile_main.glsl must agree (the whitecaps' calibration uses it)
static constexpr double WAVE_ENV_K = 1.0 / 18.0, WAVE_ENV_D = 1.0;
static constexpr int WAVE_ENV_CELLS = 8;
/// the envelope m and dm/du of wave i at phase u [rad]
inline void WaveEnvelope (int i, double u, double &m, double &dmdu)
{
	auto hash = [] (uint32_t a, uint32_t c) {
		uint32_t h = a * 0x9E3779B1u ^ c * 0x85EBCA77u;
		h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
		return (double)(h & 0xffffu) / 65535.0 * 2.0 - 1.0;
	};
	const double twoPi = 6.283185307179586;
	double t = u * (WAVE_ENV_CELLS / twoPi);
	double fc = std::floor (t), f = t - fc;
	int c0 = ((int)fc % WAVE_ENV_CELLS + WAVE_ENV_CELLS) % WAVE_ENV_CELLS, c1 = (c0 + 1) % WAVE_ENV_CELLS;
	double v0 = hash ((uint32_t)i, (uint32_t)c0), v1 = hash ((uint32_t)i, (uint32_t)c1);
	double sm = f * f * (3.0 - 2.0 * f), n = v0 + (v1 - v0) * sm;
	const double norm = 1.0 / std::sqrt (1.0 + WAVE_ENV_D * WAVE_ENV_D * 0.2287); // (E[n^2] of the noise)
	m = (1.0 + WAVE_ENV_D * n) * norm;
	dmdu = WAVE_ENV_D * (v1 - v0) * 6.0 * f * (1.0 - f) * (WAVE_ENV_CELLS / twoPi) * norm;
}
// the waves' directions about the wind: amplitude WAVE_SPREAD0 + WAVE_SPREAD1 i / (WAVE_COUNT - 1) [rad]
static constexpr double WAVE_SPREAD0 = 0.5, WAVE_SPREAD1 = 0.4;

/// sea state: drives the wave spectrum (Atmosphere::SeaState: from the wind 10 m above the water)
struct WaveSea {
	float slope = 0.0f;       // slope amplitude of each wave
	float azimuth = 0.0f;     // main direction of travel [rad] from the anchor's t1 (downwind)
	float lambdaScale = 1.0f; // longest wavelength / 60 m
	float wind = 0.0f;        // wind speed 10 m above the water [m/s]
	float bearing = 0.0f;     // direction the wind blows toward [rad], from north clockwise
	float varUnres = 0.0f;    // mean-square slope (both axes) of the waves shorter than the shortest drawn
	float varTotal = 0.0f;    // mean-square slope (both axes) of the sea (Cox-Munk)
	const char *src = "none"; // the wind's source: model, cfg, env, none
};

/// whitecaps and shore foam (Atmosphere::FoamState): from the sea state
struct WaveFoam {
	float cover = 0.0f;    // whitecap coverage W(U) (Stramska & Petelski 2003); 0: none
	float measured = 0.0f; // coverage the calibration reached
	float thresh = 1e9f;   // crest index threshold (slope units)
	float sigma = 0.0f;    // std of the crest index
	float mean = 0.0f;     // far-field mean foam
	float shoreW = 0.0f, shoreL = 0.0f, shoreT = 2.0f, shoreOn = 0.0f, Hs = 0.0f; // surf band [m], bore spacing [m], period [s], strength, Hs [m]
};

class AtmosphereRenderer {
public:
	bool Init (vkc::Context *ctx, class TextureManager *texmgr);
	/// Write the ground textures of a set
	void WriteGround (VkDescriptorSet set, class VkTexture *west, class VkTexture *east, int binding = 1) const;
	void Shutdown ();
	VkDescriptorSetLayout SetLayout () const { return setLayout; }
	VkDescriptorPool Pool () const { return pool; }
	/// bind the sea field's images (bindings 9, 10; null: white)
	void WriteOcean (VkDescriptorSet set, VkImageView longV, VkImageView rippleV) const;
	VkPipeline SkyPipeline () const { return skyPipeline; }
	VkPipelineLayout SkyLayout () const { return skyLayout; }
	/// set with uniform data for "no atmosphere"
	VkDescriptorSet NoAtmosphereSet () const { return noneSet; }
	/// the volumetric clouds' coverage map and shape noise (for their
	/// shadows), written into every set; call before atmospheres are made
	void SetCloudVolume (VkImageView coverage, VkSampler covSampler, VkImageView shape, VkSampler shapeSampler);
	void WriteCloudVolume (VkDescriptorSet set) const;

	/// Compute the transmittance and multiple-scattering tables of an
	/// atmosphere (images created by the caller, storage usage)
	struct LutParams { float betaR[4], betaM[4], ozone[4], radii[4]; };
	void ComputeLuts (const LutParams &p, const vkc::Image &transmittance, const vkc::Image &multiscat);
	/// sampler of the tables (linear, clamped)
	VkSampler LutSampler () const { return lutSampler; }

private:
	vkc::Context *ctx = nullptr;
	class TextureManager *texmgr = nullptr;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkPipelineLayout skyLayout = VK_NULL_HANDLE;
	VkPipeline skyPipeline = VK_NULL_HANDLE;
	vkc::Buffer noneUbo;
	VkSampler oceanSampler = VK_NULL_HANDLE; // (the sea field: repeat, mip mapped)
	VkDescriptorSet noneSet = VK_NULL_HANDLE;
	VkSampler lutSampler = VK_NULL_HANDLE;
	VkDescriptorImageInfo cloudVol[2] = {};
	VkDescriptorSetLayout lutSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout lutLayout = VK_NULL_HANDLE;
	VkPipeline transmittancePipe = VK_NULL_HANDLE, multiscatPipe = VK_NULL_HANDLE;
};

class Atmosphere {
public:
	Atmosphere (OBJHANDLE hPlanet, Scene *scene);
	~Atmosphere ();

	bool Present () const { return present; }
	/// colour saturation of the surface textures (tile_main.glsl surfPrm.x)
	float SurfSaturation () const { return surfSaturation; }
	/// the sea field's slopes for this frame into its images (the camera's planet; before the frame's
	/// passes, outside rendering: cmd records transfers)
	void UploadOcean (VkCommandBuffer cmd, const VECTOR3 &camGlobal);
	/// share of a salt flat at a point [rad] (Config/VK/Playa.cfg: the playa ground there is a salt crust, not clay)
	float SaltShare (double lat, double lng) const;
	/// Write this frame's uniform data (camera and sun relative to the planet)
	void Update ();
	/// Draw the sky (if the camera can see the atmosphere)
	void RenderSky ();
	/// Descriptor set with this frame's uniform data
	VkDescriptorSet Set () const;
	/// Planet surface (hemisphere textures, level 4 tiles) and mean colour, used
	/// for reflections of the planet. Call before the first use of the sets.
	void SetGround (class VkTexture *west, class VkTexture *east, const float colour[3]);
	/// Cloud layer (hemisphere textures) at the given altitude, for cloud shadows
	void SetClouds (class VkTexture *west, class VkTexture *east, double alt);

private:
	OBJHANDLE hPlanet;
	Scene *scene;
	vkc::Context *ctx;
	bool present = false;
	double radius = 0.0, atmRadius = 0.0;
	float betaR[4] = {}, betaM[4] = {};
	float overcastSky[3] = { 0, 0, 0 }; // diffuse radiance under an overcast (Venus, Titan)
	float nearHaze[4] = {};                // C4 (T4): haze near the ground (the UBO's field; zero: none)
	float overcastSun[4] = {}, overcastPrm[4] = {}; // the light from about the sun under an overcast (C3: the UBO's fields; zero: none)
	float mieLobe[4] = {}; // aerosols' forward lobe per colour, its share (atmosphere.glsl's AtmMiePhase)
	float mieG = 0.76f, sunIntensity = 6.0f; // (sky radiance scale: physically about pi for a sunlit white surface of 1)
	float airglow = 0.0f; // strength of the airglow layer (Earth)
	float detail[4] = { 0, 0, 0, 0 }; // procedural surface detail (atmosphere.glsl)
	float surfSaturation = 1.0f;     // colour saturation of the surface textures
	float aurora[4] = { 0, 0, 0, 0 }; // geomagnetic pole, aurora strength
	float ozone[4] = {};                  // ozone absorption [1/m] (Earth)
	float groundAlbedo = 0.3f;            // for light reflected into the sky
	bool gasGiant = false;                // no ground at the planet's radius (GG_FLOOR_DEPTH below it)
	vkc::Image transmittanceLut, multiscatLut; // precomputed tables
	bool visible = false;
	// per frame slot, for the main view and the environment probe
	static constexpr uint32_t NSETS = 2 * vkc::FRAMES_IN_FLIGHT;
	vkc::Buffer ubo[NSETS];
	vkc::Image oceanImg[vkc::FRAMES_IN_FLIGHT][2]; // the sea field (W3, OceanFft): the long layer with its levels, the ripples
	vkc::Buffer oceanStage[vkc::FRAMES_IN_FLIGHT];
	std::vector<float> oceanScratch;
	uint64_t oceanFrame = 0;              // (the frame UploadOcean last wrote)
	static size_t OceanStageBytes ();
	OceanFft ocean;
	bool oceanOn = false;                 // (ORBITER_VK_WAVE_FFT=0: the plane waves)
	VECTOR3 oceanT1{}, oceanT2{};
	VkDescriptorSet sets[NSETS] = {};
	uint32_t SetIndex () const;
	float groundColour[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
	float cloudParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	// waves on water (a sum of WAVE_COUNT waves, set on the CPU)
	bool waves = false;
	const char *wavesWhy = "";
	bool procOcean = false;               // (a procedural ocean world: its sea ice, I1)
	bool procSea = false;                 // (a procedural world's sea: its colour is made, the shallows tinted)
	bool seaSet = false;                  // (the sea state was taken over water: ice worlds keep it between their open water)
	char wavesName[64] = "";
	std::vector<std::array<double, 4>> saltFlats; // planet-frame centre direction, angular radius [rad] (Config/VK/Playa.cfg)
	double waveG = 9.81;                  // surface gravity [m/s^2]
	WaveSea sea;
	double seaCfgWind = -1.0;             // Sea.cfg Wind of this body (< 0: none)
	double seaEnvWind = -1.0;             // ORBITER_VK_SEA_WIND (< 0: none)
	VECTOR3 seaWindDir{};                 // planet frame, unit, at the anchor
	WaveSea SeaState (double lng, double lat, double elev) const;
	bool foam = true;                     // whitecaps and shore foam (ORBITER_VK_FOAM)
	WaveFoam wfoam;
	float laceMean = 0.0f;                // mean of the foam lace at full resolution
	float foamKey[4] = { -1, -1, -1, -1 }; // (wind, slope, lambdaScale, g) of the last calibration
	std::vector<float> capE, capLace, capLA, capA; // cached noise samples of the calibration
	WaveFoam FoamState (const WaveSea &s, bool *ran = nullptr);
	void LogFoam (const char *prefix) const;
	bool waveAnchored = false;
	VECTOR3 waveAnchor{}, waveT1{}, waveT2{};
	double waveLogT = -1e9;
	void UpdateWaves (const VECTOR3 &cam, double camR, float prm[4], float data[][4], float env[][4]);
};

/// skylight of an overcast atmosphere (Venus, Titan): the ground's
/// irradiance from the sky relative to the sun's above it; false for others
bool OvercastSkylight (const char *planet, float rgb[3]);

/// the sunlight under an overcast (C3): share s0 of the ground's light from about the sun with the sun at the
/// zenith, its falloff k towards the horizon, the angular radius [rad] of its apparent source and the cloud deck's
/// altitude [m]; false for other planets, and when ORBITER_VK_OVERCAST_SUN=0 (shares scaled by its value otherwise)
struct OvercastSun { double share, falloff, radius, deck; };
bool OvercastSunlight (const char *planet, OvercastSun &s);

#endif // !__ATMOSPHERE_H
