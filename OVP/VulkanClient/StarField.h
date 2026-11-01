// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The stars of the simulated sky: the 3D catalogue (Config/CSphere/Sky/
// stars.bin, galactic::StarCatalogue) seen from the observer. A compute
// pass selects the stars brighter than the limiting magnitude through the
// dust (again, spread over frames, when the observer has moved); each
// frame draws them as point-spread sprites at their directions from the
// observer, with their light in the four bands of the Milky Way's
// (GalaxySky), which adds the fainter stars as diffuse light.
// =======================================================================

#ifndef __STARFIELD_H
#define __STARFIELD_H

#include "VkContext.h"
#include "GalacticFrame.h"
#include "GalaxySky.h"
#include <memory>
#include <vector>

namespace galactic { class StarCatalogue; }

class StarField {
public:
	/// The catalogue (null if unsupported)
	static std::unique_ptr<galactic::StarCatalogue> Load ();

	/// Take the catalogue's stars (at the observer's epoch) and select the
	/// stars seen from obs
	bool Init (vkc::Context *ctx, const GalaxySky &sky, const galactic::StarCatalogue &cat, const galactic::Observer &obs, bool hideSun);
	void Shutdown ();
	bool Ready () const { return ready; }

	/// Advance the selection (outside rendering scopes): starts a new one
	/// when the observer has moved far enough from the current one's (or
	/// the limit has changed)
	/// (relativistic view: beta, the observer's velocity, Galactic, units
	/// of c: brightnesses seen moving; a change selects them again)
	void Update (const galactic::Observer &obs, bool hideSun, const double beta[3] = nullptr);
	/// Stars (indices) not drawn: drawn as bodies elsewhere (at most MAX_EXCLUDED)
	void SetExcluded (const std::vector<int> &idx);
	static constexpr int MAX_EXCLUDED = 4;

	/// Draw the stars (in the scene pass). galViewProj: Galactic direction
	/// -> clip (column-major); fx, fy: the projection's scale; gain: scene
	/// radiance of 1 S10; maxLimit: the faintest drawn at most (the sky's)
	void Render (VkCommandBuffer cmd, const float galViewProj[16], float fx, float fy, float gain, float maxLimit, const float relBetaGal[3] = nullptr);

	/// for the stars seen through a black hole's lens (BlackHoleLens): the
	/// draw set (catalogue stars, list, colours, generated stars; usable in
	/// compute too), its layout, the observer, the limit, the years since
	/// the catalogue's epoch
	VkDescriptorSetLayout DrawLayout () const { return drawLayout; }
	VkDescriptorSet DrawSet () const { return drawSet[front]; }
	const galactic::Observer &CurObserver () const { return curObs; }
	float Limit () const { return limit; }
	double YearsNow () const { return Years (curObs); }

	/// limiting magnitude (V, from the observer) of the stars drawn: the
	/// Milky Way's (GalaxySky::StarLimit); a change selects them again
	void SetLimit (float m);

private:
	void CreatePipelines (const GalaxySky &sky);
	/// a part of a selection (of the catalogue stars and the generator's cells)
	void Select (VkCommandBuffer cmd, int list, int part, int nParts);
	bool GenRows (uint32_t &nRows, uint32_t &nWork); // the generator's work, from the observer's place

	vkc::Context *ctx = nullptr;
	bool ready = false;
	float limit = 8.0f;     // of the stars drawn
	float selLimit = 8.0f;  // of the selection under way
	float wantLimit = 8.0f; // of the next
	bool selectAgain = false;
	uint32_t nStar = 0, capacity = 0;
	vkc::Buffer stars, colours, lists[2];
	vkc::Buffer gens[2], genRows;           // generated stars (per list), the generator's rows
	std::vector<GalaxySky::LFRow> lfRows;   // luminosity functions
	std::vector<SkyObjects::Cluster> clusters; // globular clusters
	float catLimit = 0.0f, catNear = 0.0f;  // the catalogue's completeness
	int front = 0;
	uint32_t excluded[MAX_EXCLUDED] = { ~0u, ~0u, ~0u, ~0u };
	galactic::Observer listObs, selObs, curObs;
	double catEpoch = 2016.0; // [Julian years] the catalogue's
	double Years (const galactic::Observer &o) const { return (o.mjd - 51544.5) / 365.25 + 2000.0 - catEpoch; } // since the catalogue's epoch
	double listBeta[3] = { 0, 0, 0 }, selBeta[3] = { 0, 0, 0 }; // relativistic view: the velocities selected for
	float ext[4] = { 0, 0, 0, 0 };          // extinction per band per A_V
	bool selecting = false, selHideSun = true, listHideSun = true;
	int selPart = 0;
	uint32_t genRowCount = 0, genWork = 0; // of the selection under way

	VkDescriptorSetLayout selectLayout = VK_NULL_HANDLE, drawLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet selectSet[2] = {}, drawSet[2] = {}; // per list
	VkPipelineLayout selectPL = VK_NULL_HANDLE, drawPL = VK_NULL_HANDLE;
	VkPipeline selectPipe = VK_NULL_HANDLE, genPipe = VK_NULL_HANDLE, drawPipe = VK_NULL_HANDLE;
};

#endif // !__STARFIELD_H
