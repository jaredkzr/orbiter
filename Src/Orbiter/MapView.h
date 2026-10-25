// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Map view: the camera looks at the focus vessel's reference body from
// afar, with the predicted trajectory (trajectory::Predictor), the orbits
// of nearby bodies, events (apsides, sphere of influence changes, closest
// approaches, impact) and manoeuvre nodes drawn over the scene. Nodes are
// added by clicking the trajectory and edited with handles (prograde,
// normal, radial) or in the node panel.
//
// Outside the map view the overlay shows the burn cue of the next node
// (time, velocity change, direction marker).
//
// Drawn with ImGui, so that it works with any graphics client.
// =======================================================================

#ifndef __MAPVIEW_H
#define __MAPVIEW_H

#include "OrbiterAPI.h"
#include "Vecmat.h"
#include "Camera.h"
#include "imgui.h"
#include <memory>
#include <string>
#include <vector>

class Body;
class CelestialBody;
class Vessel;
namespace trajectory { struct Prediction; struct Segment; struct Event; }

class MapView: public ImGuiDialog {
public:
	MapView ();
	void Display () override;
	void OnDraw () override {}

	bool IsOn () const { return on; }
	void Toggle ();
	void SetOn (bool on);

private:
	// projection of a global position to the screen; false if behind the camera
	bool Project (const Vector &gp, ImVec2 &s) const;
	// is the global position hidden by a planet (seen from the camera)?
	bool Occluded (const Vector &gp) const;
	// polyline of global positions (split where hidden or behind the camera)
	void DrawPath (ImDrawList *dl, const std::vector<Vector> &pts, ImU32 col, float width, bool dashed = false);
	void DrawMarker (ImDrawList *dl, const Vector &gp, ImU32 col, const char *label, int shape);
	// global position the points of a trajectory segment are relative to: the
	// map body, or for a moon of it, the moon where it is at the segment's start
	Vector Anchor (const trajectory::Segment &seg) const;
	Vector EventAnchor (const trajectory::Prediction *pred, const trajectory::Event &e) const;
	Vector NodeAnchor (const trajectory::Prediction *pred, size_t i) const;

	void DrawMap (const trajectory::Prediction *pred);
	void DrawBodyOrbits (ImDrawList *dl);
	void DrawNodes (ImDrawList *dl, const trajectory::Prediction *pred);
	void DrawPanels (const trajectory::Prediction *pred);
	void DrawBurnCue (const trajectory::Prediction *pred);
	void UpdateWarp ();

	bool on = false;
	struct SavedCamera {
		Body *target = nullptr;
		bool external = true;
		ExtCamMode extmode = CAMERA_TARGETRELATIVE;
		double dist = 4.0, phi = 0.0, theta = 0.0;
	} saved;
	const CelestialBody *mapRef = nullptr; // body the map camera is attached to

	// cached view state for the frame
	Vector camPos;
	Matrix camRot;
	double tanAp = 1.0;
	float W = 0, H = 0;
	std::vector<std::pair<Vector, double>> occluders; // spheres (global position, radius)
	std::vector<ImVec4> labels;                        // label rectangles placed in this frame

	// interaction
	int selNode = -1;
	enum class Drag { NONE, HANDLE, NODE } drag = Drag::NONE;
	int dragAxis = 0;     // 0 prograde, 1 normal, 2 radial
	double dragSign = 1.0;
	ImVec2 dragDir;       // screen direction of the dragged handle
	ImVec2 dragOrigin;
	bool hoverPath = false;
	double hoverT = 0.0;  // simulation time of the hovered trajectory point
	double warpTo = -1.0; // warping to this simulation time (-1: no)
};

#endif // !__MAPVIEW_H
