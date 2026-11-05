// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Camera configuration dialog
// ======================================================================

#ifndef __DLGCAMERA_H
#define __DLGCAMERA_H

#include "DlgMgr.h"
#include "Camera.h"
#include "UiTheme.h"
#include <string>

class Body;
class CelestialBody;
class DlgCamera : public ImGuiDialog {
public:
	DlgCamera();
	void OnDraw() override;
private:
	void DrawStatus();
	void DrawView();
	void DrawTarget();
	void DrawGround();
	void DrawPreset();

	ExtCamMode extmode;
	IntCamMode intmode;
	bool extcam;
	bool ground_lock;
	bool rot_is_tilt;

	std::string m_SelectedTarget;
	std::string m_SelectedSite;
	std::string m_SitePlanet;
	int m_SelectedPreset = -1;
	char m_Filter[64] = "";
	ui::Fill m_FillTarget, m_FillGround, m_FillPreset;

	char longitude[64] = "";
	char latitude[64] = "";
	char altitude[64] = "";
	char followterrain[64] = "";
	bool m_TargetLock = false;

	Body *SelectedBody() const;
	bool TargetRow(const char *name, const char *icon, int depth);
	void AddCbodyNode(const CelestialBody *body, int depth);
	void ApplyObserver();
	void SetCurrentGroundpos();

	struct CameraTab {
		const char *name;
		const char *helptopic;
		void (DlgCamera::* func)();
	};

	static inline CameraTab tabs[] = {
		{"View",    "/cam_control.htm", &DlgCamera::DrawView},
		{"Target",  "/cam_target.htm",  &DlgCamera::DrawTarget},
		{"Ground",  "/cam_ground.htm",  &DlgCamera::DrawGround},
		{"Presets", "/cam_preset.htm",  &DlgCamera::DrawPreset},
	};

};
#endif // !__DLGCAMERA_H
