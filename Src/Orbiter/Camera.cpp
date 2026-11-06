// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#define STRICT 1
#define OAPI_IMPLEMENTATION

#include <stdio.h>
#include <iomanip>

#include "resource.h"
#include "D3dmath.h"
#include "Camera.h"
#include "CamRig.h"
#ifndef _WIN32
#include "Platform.h"
#endif
#include "Config.h"
#include "Body.h"
#include "Planet.h"
#include "GroundScatter.h"
#include "Vessel.h"
#include "Vesselbase.h"
#include "Psys.h"
#include "Pane.h"
#include "Dialogs.h"
#include "DlgMgr.h"
#include "Orbiter.h"
#include "Orbitersdk.h"
#include "Util.h"
#include "Log.h"
#include "OrbiterAPI.h"
#include "Vobject.h"
#ifdef _WIN32
#include <zmouse.h>
#endif

using namespace std;
using camrig::Col;

extern Orbiter *g_pOrbiter;
extern TimeData td;
extern PlanetarySystem *g_psys;
extern Pane *g_pane;
extern Vessel *g_focusobj;
extern char DBG_MSG[256];

static Vector nullvec;
static bool brot = false;
const double dragT = 3.0;
static const double vmax = 2.0;  // max rotation velocity (rad/s)
static const double amax = 8.0;  // max rotation acceleration (rad/s^2)

// external tracking modes: easing [s] of turns and of the distance, the
// slower easing after a change of mode, a blend between views [s], the
// Chase camera's view (pitch [rad], above) and lag [s]
static const double TAU_TURN = 0.07, TAU_ZOOM = 0.12, TAU_SETTLE = 0.25, SETTLE_T = 1.5;
static const double BLEND_T = 0.5;
static const double CHASE_PITCH = -0.2, CHASE_LAG = 0.35;
// Flyby: the highest speed [m/s] at which the target passes the camera
// (faster in space: the camera drifts along with the target)
static const double FLYBY_VMAX = 300.0;
static const double HOME_FRAME = 3.0;       // Home: the camera this many framing radii out
static const double MIN_FRAME = 2.2;        // a new target: at least this many framing radii out
static const double WRECK_FRAME = 4.0, WRECK_MIN_DIST = 60.0, WRECK_PITCH = 0.35; // after a break-up: radii out, at least [m], looking down [rad]

Camera::Camera (double _nearplane, double _farplane)
{
	extmode = CAMERA_AUTO;
	intmode = CAMERA_GENERICCOCKPIT;
	action  = CAMERA_NORMAL;
	gdir.Set (1,0,0);
	rofs = &nullvec;
	target = 0;
	rdist = rdistT = 4.0;
	yawT = pitchT = 0.0;
	lockQ = lockQT = frame = IMatrix ();
	dirref = 0;
	ephi = etheta = 0.0;
	dphi = dtht = 0.0;
	eyeofs0. Set (0,0.1,0.08);
	SetDefaultCockpitDir (Vector (0,0,1));
	SetCockpitDir (0,0);
	SetCatchAngle (RAD*5.0);
	memset (&cockpitprm, 0, sizeof(cockpitprm));
	go.panspeed = g_pOrbiter->Cfg()->CfgCameraPrm.Panspeed;
	go.tgtlock = true;
	go.terrain_limit = g_pOrbiter->Cfg()->CfgCameraPrm.TerrainLimit;
	gos.planet[0] = gos.site[0] = gos.addr[0] = '\0';
	planet_proxy = 0;
	npreset = 0;
	external_view = true;//false;
	pv_mat_valid = false;
	has_tref = false;
	movehead = false;
	ExtCtrlMode = 0;
#ifdef _WIN32
	GetCursorPos (&pm);
#else
	pm.x = pm.y = 0;
#endif
	mmoveT = -1000.0;
	ap_int = ap_ext = RAD*25.0;
	ap = &ap_ext;
	SetAperture (*ap, true, true);
	nearplane = (float)_nearplane;
	farplane = (float)_farplane;
	aspect = 1.0; // arbitrary default - reset with ResizeViewport
	w05 = h05 = 100;
	mbdown[0] = mbdown[1] = false;
	VMAT_identity (view_mat);
	SetFrustumLimits (nearplane, farplane);
	ECC = 0;
	etile.resize(8);
}

Camera::~Camera ()
{
	ClearPresets();
}

bool Camera::ProcessMouse (UINT event, DWORD state, DWORD x, DWORD y, const char *kstate)
{
	if (event != WM_MOUSEWHEEL)
		mx = (int)x, my = (int)y;

	switch (event) {
	case WM_LBUTTONDOWN:
		mbdown[0] = true;
		return true;
	case WM_RBUTTONDOWN:
		mbdown[1] = true;
		g_pOrbiter->InitRotationMode();
		return true;
	case WM_LBUTTONUP:
		if (mbdown[0]) {
			mbdown[0] = false;
			return true;
		}
		break;
	case WM_RBUTTONUP:
		if (mbdown[1]) {
			mbdown[1] = false;
			g_pOrbiter->ExitRotationMode();
			return true;
		}
		break;
	case WM_MOUSEWHEEL:
		if (KEYMOD_CONTROL(kstate)) { // (external: the field of view; in the cockpit, the wheel does that already)
			if (external_view) g_pOrbiter->IncFOV ((short)HIWORD(state)*(-2.0 / 120.0*RAD));
		}
		else {
			short zDelta = (short)HIWORD(state);
			if (external_view)
				ShiftDist(-zDelta*0.001);
			else
				g_pOrbiter->IncFOV(zDelta*(-2.0 / 120.0*RAD));
		} return true;
		break;
	}
	return false;
}

void Camera::UpdateMouse ()
{
#ifdef _WIN32
	POINT pt;
	GetCursorPos (&pt);
	if (pt.x != pm.x || pt.y != pm.y) {
		pm.x = pt.x, pm.y = pt.y;
		mmoveT = td.SysT0;
	}

	if (mbdown[1]) {
		int dx, dy, x0, y0;
		x0 = pt.x, y0 = pt.y;
		if (!g_pOrbiter->IsFullscreen())
			ScreenToClient (g_pOrbiter->GetRenderWnd(), &pt);
		dx = pt.x - mx;
		dy = pt.y - my;
		SetCursorPos (x0-dx, y0-dy);
		if (!(dx || dy)) return;

		if (external_view) {
			if (extmode == CAMERA_GROUNDOBSERVER) {
				if (go.tgtlock) {
					GroundObserverShift (dx*go.panspeed * -0.005, dy*go.panspeed * -0.005, 0.0);
				} else
					GroundObserverTilt (dx * -0.005, dy * -0.005);
			} else {
				if (dx) ShiftPhi (dx * -0.005);
				if (dy) ShiftTheta (dy * -0.005);
			}
		} else {
			Rotate (dx * -0.001, dy * -0.001);
		}
	}
#else
	// The graphics client reports the cursor position and the relative
	// motion (the cursor is captured while the right button is held).
	int x, y, dx, dy;
	orbiter::platform::GetMousePos (x, y);
	orbiter::platform::TakeMouseMotion (dx, dy);
	if (x != pm.x || y != pm.y) {
		pm.x = x, pm.y = y;
		mmoveT = td.SysT0;
	}

	if (mbdown[1] && (dx || dy)) {
		if (external_view) {
			if (extmode == CAMERA_GROUNDOBSERVER) {
				if (go.tgtlock) {
					GroundObserverShift (dx*go.panspeed * -0.005, dy*go.panspeed * -0.005, 0.0);
				} else
					GroundObserverTilt (dx * -0.005, dy * -0.005);
			} else {
				if (dx) ShiftPhi (dx * -0.005);
				if (dy) ShiftTheta (dy * -0.005);
			}
		} else {
			Rotate (dx * -0.001, dy * -0.001);
		}
	}
#endif
}

void Camera::SetViewInternal ()
{
	external_view = false;
	if (ap != &ap_int) {
		ap = &ap_int;
		double a = ap_int;
		ap_int = 0.0; // force update
		g_pOrbiter->SetFOV(a);
	}
}

void Camera::SetViewExternal ()
{
	external_view = true;
	if (ap != &ap_ext) {
		ap = &ap_ext;
		double a = ap_ext;
		ap_ext = 0.0; // force update
		g_pOrbiter->SetFOV(a);
	}
}

void Camera::SetIntMode (IntCamMode mode)
{
	if (mode == intmode) return; // nothing to do
	if (!external_view && intmode == CAMERA_VIRTUALCOCKPIT)
		StoreVCParams();
	intmode = mode;
	if (!external_view) {
		if (intmode != CAMERA_VIRTUALCOCKPIT || !RecallVCParams()) {
			rpos.Set (nullvec);
			SetCockpitDir (0,0);
		}
	}
}

bool Camera::IsCockpitForward () const
{
	return (!external_view && cphi == 0.0 && ctheta == 0.0);
}

void Camera::StoreVCParams()
{
	cockpitprm.target = target;
	cockpitprm.rpos   = rpos;
	cockpitprm.cphi   = cphi;
	cockpitprm.ctheta = ctheta;
}

bool Camera::RecallVCParams()
{
	if (target != cockpitprm.target) return false; // don't restore across different vessels
	rpos.Set (cockpitprm.rpos);
	SetCockpitDir (cockpitprm.cphi, cockpitprm.ctheta);
	return true;
}

void Camera::SetCMode (const CameraMode *cm)
{
	bool ext = (cm->GetMode() != CameraMode::CM_COCKPIT);
	Body *tgt = (Body*)cm->GetTarget();

	if ((tgt && tgt != target) || ext != external_view) {
		if (!tgt) tgt = target;
		Attach (tgt, ext ? 1:0);
	}
	if (tgt && !ext && tgt != g_focusobj)
		g_pOrbiter->SetFocusObject ((Vessel*)tgt);

	switch (cm->GetMode()) {
	case CameraMode::CM_COCKPIT: {
		const CameraMode_Cockpit *cmc = (const CameraMode_Cockpit*)cm;
		int pos = cmc->GetPosition();
		switch (cmc->GetCockpitMode()) {
		case CameraMode_Cockpit::CM_GENERIC:
			g_pane->SetPanelMode(1);
			break;
		case CameraMode_Cockpit::CM_PANEL2D:
			if (g_pane->SetPanelMode(2))
				if (pos >= 0) g_pane->SelectPanel(pos);
			break;
		case CameraMode_Cockpit::CM_VC:
			if (g_pane->SetPanelMode(3)) {
				if (pos >= 0) g_pane->SelectVC(pos);
				int lean = cmc->GetLeaning();
				if (lean >= 0 && lean < 4) {
					g_focusobj->LeanCamera (lean, cmc->GetLeaningSmooth());
				}
			}
			break;
		}
		} break;
	case CameraMode::CM_TRACK: {
		const CameraMode_Track *cmt = (const CameraMode_Track*)cm;
		double rd, ph, th;
		cmt->GetPosition (&rd, &ph, &th);
		Legacy l;
		switch (cmt->GetTrackMode()) {
		case CameraMode_Track::TM_CURRENT:       break;
		// (the classic modes: the closest new one)
		case CameraMode_Track::TM_RELATIVE:      SetTrackMode (CAMERA_LOCKED); break;
		case CameraMode_Track::TM_ABSDIR:        l.mode = CAM_ABSDIRECTION;     break;
		case CameraMode_Track::TM_GLOBAL:        l.mode = CAM_GLOBALFRAME;      break;
		case CameraMode_Track::TM_TARGETTOREF:   l.mode = CAM_TARGETTOOBJECT;   break;
		case CameraMode_Track::TM_TARGETFROMREF: l.mode = CAM_TARGETFROMOBJECT; break;
		case CameraMode_Track::TM_AUTO:          SetTrackMode (CAMERA_AUTO);    break;
		case CameraMode_Track::TM_FREE:          SetTrackMode (CAMERA_FREE);    break;
		case CameraMode_Track::TM_ORBITAL:       SetTrackMode (CAMERA_ORBITAL); break;
		case CameraMode_Track::TM_CHASE:         SetTrackMode (CAMERA_CHASE);   break;
		case CameraMode_Track::TM_LOCKED:        SetTrackMode (CAMERA_LOCKED);  break;
		case CameraMode_Track::TM_FLYBY:         SetTrackMode (CAMERA_FLYBY);   break;
		}
		if (l.mode) {
			if (!external_view || extmode == CAMERA_GROUNDOBSERVER) SetTrackMode (CAMERA_FREE);
			l.rd = (rd ? rd : rdist), l.ph = ph, l.th = th;
			ApplyLegacy (l, (const Body*)cmt->GetRef());
		} else if (rd) SetExtView (rd, ph, th);
		} break;
	case CameraMode::CM_GROUND: {
		const CameraMode_Ground *cmg = (const CameraMode_Ground*)cm;
		const Body *ref;
		double lng, lat, alt;
		cmg->GetPosition (&lng, &lat, &alt, (OBJHANDLE*)&ref);
		if (!ref) ref = dirref;
		SetGroundMode (CAMERA_GROUNDOBSERVER, ref, lng, lat, alt, 0, cmg->GetAltMode());
		SetGroundObserver_TargetLock (cmg->GetTgtLock());
		if (!go.tgtlock) {
			double ph, th;
			cmg->GetOrientation (&ph, &th);
			go.phi = ph, go.tht = th;
		}
		} break;
	}

	double newap = cm->GetFOV()*0.5*RAD;
	if (newap && fabs(newap - *ap) > 1e-6) g_pOrbiter->SetFOV (newap);
}

CameraMode *Camera::GetCMode () const
{
	CameraMode *cm;

	if (external_view) {
		switch (extmode) {
		case CAMERA_AUTO:
		case CAMERA_FREE:
		case CAMERA_ORBITAL:
		case CAMERA_CHASE:
		case CAMERA_LOCKED:
		case CAMERA_FLYBY:
		case CAMERA_INERTIAL: {
			cm = new CameraMode_Track; TRACENEW
			CameraMode_Track *cmt = (CameraMode_Track*)cm;
			static const CameraMode_Track::TrackMode tm[] = {
				CameraMode_Track::TM_AUTO, CameraMode_Track::TM_FREE, CameraMode_Track::TM_ORBITAL, CameraMode_Track::TM_CHASE,
				CameraMode_Track::TM_LOCKED, CameraMode_Track::TM_FLYBY, CameraMode_Track::TM_GLOBAL };
			cmt->SetTrackMode (tm[extmode]);
			cmt->SetPosition (rdist, Phi(), Theta());
			} break;
		case CAMERA_GROUNDOBSERVER: {
			cm = new CameraMode_Ground; TRACENEW
			CameraMode_Ground *cmg = (CameraMode_Ground*)cm;
			cmg->SetPosition (go.lng, go.lat, go.alt, (OBJHANDLE)dirref);
			if (!go.tgtlock) cmg->SetOrientation (go.phi, go.tht);
			} break;
		default: cm = new CameraMode_Cockpit; TRACENEW
		}
	} else {
		cm = new CameraMode_Cockpit; TRACENEW
	}
	cm->SetTarget (target);
	cm->SetFOV (2*DEG * *ap);
	return cm;
}

int Camera::GetMode () const
{
	if (external_view) {
		switch (extmode) {
		case CAMERA_AUTO:           return CAM_AUTO;
		case CAMERA_FREE:           return CAM_FREE;
		case CAMERA_ORBITAL:        return CAM_ORBITAL;
		case CAMERA_CHASE:          return CAM_CHASE;
		case CAMERA_LOCKED:         return CAM_LOCKED;
		case CAMERA_FLYBY:          return CAM_FLYBY;
		case CAMERA_INERTIAL:       return CAM_GLOBALFRAME;
		case CAMERA_GROUNDOBSERVER: return CAM_GROUNDOBSERVER;
		}
	}
	return CAM_COCKPIT;
}

void Camera::Attach (Body *_target, int mode)
{
	// sanity check
	if (_target->Type() != OBJTP_VESSEL) mode = 1;

	// save current cockpit parameters for later restore
	if (!external_view && intmode == CAMERA_VIRTUALCOCKPIT)
		StoreVCParams();

	Body *from = target; // (a view to blend from)
	target = _target;
	if (mode != 2) external_view = (mode != 0); // otherwise don't change

	if (external_view) {
		SetViewExternal();
		if (extmode != CAMERA_GROUNDOBSERVER) {
			if (!TrackModeAvailable (extmode)) extmode = CAMERA_FREE; // (e.g. Chase, and the target is a planet)
			frameKind = -1;
			chaseValid = false;
			if (from) StartBlend (from);
			if (from != target) { // (a new target: at least far enough to frame it, a launch stack whole)
				double rf = MIN_FRAME * FrameRadius () / target->Size();
				if (rdistT < rf) rdist = rdistT = rf;
			}
			if (extmode == CAMERA_FLYBY) PlaceFlyby (), UpdateFlyby (0.0);
			else UpdateTrack (0.0);
		}
	} else {
		SetViewInternal();
		Vessel* vessel = (Vessel*)target;
		rofs = vessel->CamPos();
		SetDefaultCockpitDir (*vessel->CamDir0(), vessel->CamTilt0());
		if (intmode != CAMERA_VIRTUALCOCKPIT || !RecallVCParams()) {
			rpos.Set (nullvec);
			SetCockpitDir (0,0);
		}
		SetCatchAngle (vessel->CamCatchRange());
		grot.Set (target->GRot() * rrot);
		gspos.Set (mul (target->GRot(), *rofs));
		gpos.Set (gspos + target->GPos());
		GPOS = MakeVECTOR3 (gpos);
		blend.on = false;
	}
	tref.Set(0,0,0);
	vphi = vtht = dphi = dtht = 0.0;
	has_tref = false;
}

bool Camera::Direction2Viewport(const Vector &dir, int &x, int &y)
{
	D3DVECTOR homog;
	D3DMath_VectorMatrixMultiply (homog, D3DMath_Vector(dir.x, dir.y, dir.z), *D3D_ProjViewMatrix());
	if (homog.x >= -1.0f && homog.y <= 1.0f && homog.z <= 1.0f) {
		if (std::hypot(homog.x, homog.y) < 1e-6) {
			x = (int)w05, y = (int)h05;
		} else {
			x = (int)(w05*(1.0f+homog.x));
			y = (int)(h05*(1.0f-homog.y));
		}
		return true;
	} else
		return false;
}

double Camera::Distance () const
{
	if (external_view && target) {
		if (extmode == CAMERA_GROUNDOBSERVER)
			return gspos.length();
			//return gpos.dist (target->GPos());
		else return rdist*target->Size();
	} else return 0.0;
}

double Camera::Phi () const
{
	if (!target) return 0.0;
	if (!external_view) return cphi;
	if (extmode == CAMERA_LOCKED) {
		double yaw, pitch;
		camrig::YawPitchOf (Col (lockQ, 2), yaw, pitch);
		return yaw;
	}
	return ephi;
}

double Camera::Theta () const
{
	if (!target) return 0.0;
	if (!external_view) return ctheta;
	if (extmode == CAMERA_LOCKED) {
		double yaw, pitch;
		camrig::YawPitchOf (Col (lockQ, 2), yaw, pitch);
		return pitch;
	}
	return etheta;
}

void Camera::AddPhi (double dphi)
{
	// (half the angle, as it always was: callers pass twice the turn)
	if (external_view) {
		if (extmode == CAMERA_GROUNDOBSERVER) {
			GroundObserverShift (dphi*go.panspeed, 0, 0);
		} else if (extmode == CAMERA_LOCKED) {
			lockQT = camrig::Orthonormalise (lockQT * camrig::RotY (dphi*0.5)); // (about the camera's own up)
		} else if (extmode != CAMERA_FLYBY) {
			yawT = camrig::Wrap (yawT + dphi*0.5);
		}
	}
}

void Camera::AddTheta (double dtheta)
{
	if (external_view) {
		if (extmode == CAMERA_GROUNDOBSERVER) {
			GroundObserverShift (0, -dtheta*go.panspeed, 0);
		} else if (extmode == CAMERA_LOCKED) {
			lockQT = camrig::Orthonormalise (lockQT * camrig::RotX (dtheta*0.5)); // (about the camera's own x axis)
		} else if (extmode != CAMERA_FLYBY) {
			pitchT = camrig::ClampPitch (pitchT + dtheta*0.5);
		}
	}
}

void Camera::Rotate (double _dphi, double _dtht, bool smooth)
{
	// change the camera direction
	if (external_view) {
		if (extmode == CAMERA_GROUNDOBSERVER && !go.tgtlock)
			GroundObserverTilt (_dphi*0.5, _dtht*0.5);
	} else { // cockpit view
		dphi = _dphi*vmax, dtht = _dtht*vmax;
	}
	rot_smooth = smooth;
}

void Camera::MoveTo (const Vector &p)
{
	if (ExtCtrlMode & CAMDATA_POS) return; // inhibit transition moves if camera is externally controlled
	if (external_view) {
	} else {
		movehead = true;
		tgtp = p;
	}
}

void Camera::MoveToDirect (const Vector &p)
{
	if (external_view) {
	} else {
		rpos = p;
	}
}

void Camera::ShiftPhi (double shift)
{
	if (external_view && extmode != CAMERA_GROUNDOBSERVER && target->Type() == OBJTP_PLANET) {
		shift *= max (1e-6, (rdist-1.0)/rdist);
	}
	AddPhi (shift);
}

void Camera::ShiftTheta (double shift)
{
	if (external_view && extmode != CAMERA_GROUNDOBSERVER && target->Type() == OBJTP_PLANET) {
		shift *= max (1e-6, (rdist-1.0)/rdist);
	}
	AddTheta (shift);
}

void Camera::ShiftDist (double shift)
{
	double fact;
	if (external_view && extmode != CAMERA_GROUNDOBSERVER) {
		double scale = max (1e-4, (rdistT-1.0)/rdistT); // slow down when approaching target radius
		scale = max(scale, 1.0/target->Size());       // move at least 1m/s
		shift *= scale;
	}
	if (shift > 0) fact = 1.0 + shift;
	else fact = max(1.0/rdistT, 1.0/(1.0 - shift));
	ChangeDist (fact);
}

void Camera::ChangeDist (double fact)
{
	if (external_view) {
		if (extmode == CAMERA_GROUNDOBSERVER) {
			GroundObserverShift (0, 0, (fact > 1 ? fact-1 : -1/fact+1)*go.panspeed);
		} else if (extmode != CAMERA_FLYBY) {
			rdistT = max (1.0, rdistT*fact);
			rshift = false;
		}
	}
}

void Camera::Drag (const Vector &gshift)
{
	if (external_view) {
		tref.Set (has_tref ? tref+gshift: gshift);
		tref_t0  = td.SysT1;
		tref_d0  = tref.length();
		tref_r0  = gspos.length();
		tref_r1  = (has_tref && rshift ? tref_r1/tref_r0*rdist : rdist)*target->Size();
		rdist = rdistT = tref_r0/target->Size();
		has_tref = true;
		rshift   = (fabs ((tref_r0-tref_r1)/tref_r1) > 0.01);
	}
}

double Camera::SetAperture (double _ap, bool limit_range, bool force)
{
	const double ap_limit_lo = Rad(5.0);
	const double ap_limit_hi = Rad(45.0);

	if (force || (fabs (*ap-_ap) > 1e-5)) {
		*ap = _ap;
		if (limit_range) {
			if      (*ap < ap_limit_lo) *ap = ap_limit_lo;
			else if (*ap > ap_limit_hi) *ap = ap_limit_hi;
		}
		tan_ap = tan (*ap);
		UpdateProjectionMatrix ();
	}
	return *ap;
}

void Camera::SetFrustumLimits (double _nearplane, double _farplane)
{
	nearplane = (float)_nearplane;
	farplane  = (float)_farplane;
	homog_zlimit = farplane / (farplane-nearplane);
	UpdateProjectionMatrix ();
}

void Camera::ViewportToGlobalDir (double sx, double sy, Vector &gdir) const
{
	Vector s((sx-w05+0.5)*tan_ap/h05, (h05-sy-0.5)*tan_ap/h05, 1.0);
	gdir.Set (mul (grot, s.unit()));
}

bool Camera::GlobalToViewport (const Vector &gpos, double &sx, double &sy) const
{
	Vector s (tmul (grot, gpos - *GPosPtr()));
	if (s.z <= 0.0) return false;
	sx = s.x / s.z * h05 / tan_ap + w05 - 0.5;
	sy = h05 - s.y / s.z * h05 / tan_ap - 0.5;
	return true;
}

void Camera::SetDefaultCockpitDir (const Vector &dir, double tilt)
{
	isStdDir = (dir.x == 0 && dir.y == 0 && dir.z > 0 && tilt == 0.0);
	Matrix rrot0_old(rrot0);
	if (isStdDir) {
		cphi0 = ctheta0 = 0.0;
		rrot0.Set (1,0,0,  0,1,0,  0,0,1);
	} else {
		cphi0   = atan2 (-dir.x, dir.z);
		ctheta0 = asin (dir.y);
		double sinph = sin(cphi0),   cosph = cos(cphi0);
		double sinth = sin(ctheta0), costh = cos(ctheta0);
		rrot0.Set (cosph,  sinph*sinth, -sinph*costh,
				   0.0,    costh,        sinth,
		 		   sinph, -cosph*sinth,  cosph*costh);
		if (tilt) {
			double sint = sin(tilt), cost = cos(tilt);
			rrot0.postmul (Matrix (cost, -sint, 0,
				                   sint, cost, 0,
								   0, 0, 1));
		}
	}
	// reset relative camera direction so that absolute direction
	// (in vessel frame) remains the same
	if (!external_view) {
		rrot.premul(transp(rrot0)*rrot0_old);
		cphi = atan2(rrot.m31,rrot.m11);
		ctheta = asin(rrot.m23);
	}
}

void Camera::SetCockpitDir (double ph, double th)
{
	// ph and th are polar and azimuth angles relative to default direction
	if (!external_view) {
		cphi   = normangle (ph);
		ctheta = normangle (th);
		double sinph = sin(cphi),   cosph = cos(cphi);
		double sinth = sin(ctheta), costh = cos(ctheta);
		rrot.Set (cosph,  sinph*sinth, -sinph*costh,
			      0.0,    costh,        sinth,
				  sinph, -cosph*sinth,  cosph*costh);
	}
	eyeofs = mul (rrot, Vector (0,0.1,0.08)) - eyeofs0;
	if (!isStdDir) eyeofs = mul (rrot0, eyeofs);
}

void Camera::SetDistance(double rd)
{
	rdist = rdistT = max (1.0, rd);
}

void Camera::SetExtView (double rd, double ph, double th)
{
	if (rd > 0.0) rdist = rdistT = max (1.0, rd);
	if (extmode == CAMERA_LOCKED) {
		lockQ = lockQT = camrig::YawPitch (ph, th);
	} else {
		ephi = yawT = camrig::Wrap (ph);
		etheta = pitchT = camrig::ClampPitch (th);
	}
}

void Camera::ResetExtView ()
{
	if (!external_view || extmode == CAMERA_GROUNDOBSERVER) return;
	if (extmode == CAMERA_FLYBY) {
		StartBlend ();
		PlaceFlyby ();
		return;
	}
	// behind the target, a little above, at the distance that frames it
	settleT = camT + SETTLE_T;
	if (extmode == CAMERA_LOCKED) lockQT = camrig::YawPitch (0.0, CHASE_PITCH);
	else yawT = 0.0, pitchT = CHASE_PITCH;
	rdistT = max (1.0, HOME_FRAME * FrameRadius () / target->Size());
}

double Camera::FrameRadius () const
{
	if (!target) return 1.0;
	double r = target->Size();
	if (target->Type() != OBJTP_VESSEL) return r;
	// (the whole stack: from its root, every attached child)
	const Vessel *root = (const Vessel*)target;
	while (root->AttachParent ()) root = root->AttachParent ();
	std::vector<const Vessel*> todo (1, root);
	for (size_t i = 0; i < todo.size() && i < 64; i++) {
		const Vessel *v = todo[i];
		r = max (r, (v->GPos() - target->GPos()).length() + v->Size());
		for (DWORD k = 0; k < v->nAttachChild (); k++)
			if (const Vessel *c = v->AttachChild (k)) todo.push_back (c);
	}
	return r;
}

void Camera::FrameWreck (double radius)
{
	if (!external_view || !target) return;
	// (back from the debris field, looking down on it)
	double d = max (WRECK_FRAME * radius, WRECK_MIN_DIST);
	rdistT = max (1.0, d / target->Size());
	pitchT = WRECK_PITCH;
	settleT = camT + SETTLE_T;
}

void Camera::ResetCockpitDir (bool smooth)
{
	if (!external_view) {
		cntr_phi = 0;
		cntr_tht = 0;
		if (smooth) {
			action = CAMERA_RECENTER;
			rot_smooth = true;
		} else
			SetCockpitDir (0.0, 0.0);
	}
}

void Camera::ResetCockpitDir (double ph, double th, bool smooth)
{
	if (!external_view) {
		cntr_phi = ph;
		cntr_tht = th;
		if (smooth) {
			action = CAMERA_RECENTER;
			rot_smooth = true;
		} else
			SetCockpitDir (ph, th);
	}
}

void Camera::SetCatchAngle (double cangle)
{
	catchangle = cangle;
}

void Camera::ResetCockpitView()
{
	movehead = false;
	ResetCockpitPos();
	ResetCockpitDir(false);
}

void Camera::ResetCockpitPos ()
{
	if (!external_view) {
		rpos.Set (0,0,0);
	}
}


void Camera::SetTrackMode (ExtCamMode mode, const Body *ref)
{
	if (mode == CAMERA_GROUNDOBSERVER) return; // (SetGroundMode)
	// (from the cockpit or the ground the camera returns to its distance
	// and angles, blended; between tracking modes it stays where it is)
	bool adopt = (external_view && extmode != CAMERA_GROUNDOBSERVER);
	if (!adopt) Attach (target, 1);
	if (!TrackModeAvailable (mode)) mode = CAMERA_FREE;
	bool fromFlyby = (extmode == CAMERA_FLYBY && mode != CAMERA_FLYBY);
	if (mode == CAMERA_FLYBY && extmode != CAMERA_FLYBY) flybyReturn = rdistT; // (Flyby's distance is its own)
	extmode = mode;
	settleT = -1e10; // (Chase sets it again)
	frameKind = -1;
	chaseValid = false;
	if (mode == CAMERA_FLYBY) {
		StartBlend ();
		PlaceFlyby ();
		UpdateFlyby (0.0);
		return;
	}
	bool changed;
	Matrix F (TrackFrame (0.0, changed));
	if (adopt) {
		StartBlend (); // (the place stays, the orientation turns to the new frame's up)
		Adopt (F);
		if (fromFlyby && flybyReturn > 0.0) rdistT = flybyReturn; // (eased back to the distance before Flyby)
	}
	if (mode == CAMERA_CHASE) {
		yawT = 0.0, pitchT = CHASE_PITCH;
		settleT = camT + SETTLE_T;
	}
	if (!adopt) UpdateTrack (0.0);
}

void Camera::CycleTrackMode (int dir)
{
	static const ExtCamMode cycle[] = { CAMERA_AUTO, CAMERA_FREE, CAMERA_ORBITAL, CAMERA_CHASE, CAMERA_LOCKED, CAMERA_FLYBY };
	const int n = sizeof (cycle) / sizeof (cycle[0]);
	int i = 0;
	for (int k = 0; k < n; k++) if (cycle[k] == extmode) i = k;
	if (!external_view || extmode == CAMERA_GROUNDOBSERVER || extmode == CAMERA_INERTIAL)
		i = (dir > 0 ? n - 1 : 1); // (into the cycle at Auto)
	for (int k = 0; k < n; k++) {
		i = (i + (dir > 0 ? 1 : n - 1)) % n;
		if (TrackModeAvailable (cycle[i])) break;
	}
	SetTrackMode (cycle[i]);
	modeChangeT = camT; // (its label)
}

void Camera::FaceObject (const Body *obj, bool behindTarget)
{
	if (!obj || !target || obj == target) return;
	if (!external_view || extmode == CAMERA_GROUNDOBSERVER || extmode == CAMERA_FLYBY || extmode == CAMERA_CHASE)
		SetTrackMode (CAMERA_FREE);
	StartBlend ();
	Vector g ((obj->GPos() - target->GPos()).unit());
	double d = rdistT * target->Size();
	gspos = g * (behindTarget ? -d : d);
	gpos = target->GPos() + gspos;
	grot = camrig::LookLevel (-gspos, UpAtTarget (), grot);
	bool changed;
	Matrix F (TrackFrame (0.0, changed));
	Adopt (F);
	UpdateTrack (0.0);
}

bool Camera::TrackModeAvailable (ExtCamMode mode) const
{
	switch (mode) {
	case CAMERA_CHASE:
	case CAMERA_FLYBY:
		return target && (target->Type() == OBJTP_VESSEL || target->Type() == OBJTP_DEBRIS);
	case CAMERA_GROUNDOBSERVER:
		return false; // (not a tracking mode)
	case CAMERA_ORBITAL: {
		// (its up is the orbit's normal: in the air, near the ground, the
		// horizon would stand on end - not offered there)
		if (!target) return false;
		if (target->Type() == OBJTP_VESSEL || target->Type() == OBJTP_DEBRIS) {
			const SurfParam *sp = target->Type() == OBJTP_VESSEL ? ((const Vessel*)target)->GetSurfParam() : ((const VesselBase*)target)->SurfaceParams();
			if (sp && sp->ref && ((sp->is_in_atm && sp->atmrho > 1e-6) || sp->alt < 0.01 * sp->ref->Size())) return false; // (air that matters: below about 100 km on Earth)
		}
		return true;
	}
	default:
		return target != 0;
	}
}

const char *Camera::ExtModeName (ExtCamMode mode)
{
	switch (mode) {
	case CAMERA_AUTO:           return "Auto";
	case CAMERA_FREE:           return "Free";
	case CAMERA_ORBITAL:        return "Orbital";
	case CAMERA_CHASE:          return "Chase";
	case CAMERA_LOCKED:         return "Locked";
	case CAMERA_FLYBY:          return "Flyby";
	case CAMERA_INERTIAL:       return "Inertial";
	case CAMERA_GROUNDOBSERVER: return "Ground";
	}
	return "";
}

bool Camera::CelestialTarget () const
{
	int t = target ? target->Type() : OBJTP_INVALID;
	return t == OBJTP_PLANET || t == OBJTP_STAR || t == OBJTP_CBODY || t == OBJTP_BLACKHOLE;
}

const Body *Camera::RefBody () const
{
	if (!target) return 0;
	if (target->Type() == OBJTP_SURFBASE) return target->ElRef(); // (its planet)
	return target->ElRef();
}

Vector Camera::UpAtTarget () const
{
	const Body *ref = RefBody ();
	if (!ref || CelestialTarget ()) return Vector (0, 1, 0);
	Vector r (target->GPos() - ref->GPos());
	double l = r.length();
	return l > 0.0 ? r / l : Vector (0, 1, 0);
}

Vector Camera::TravelVelocity (bool &surface) const
{
	surface = false;
	const Body *ref = RefBody ();
	if (target->Type() == OBJTP_VESSEL || target->Type() == OBJTP_DEBRIS) {
		const VesselBase *vb = (const VesselBase*)target;
		const SurfParam *sp = (target->Type() == OBJTP_VESSEL ? ((const Vessel*)target)->GetSurfParam() : vb->SurfaceParams());
		if (sp && sp->ref && (sp->is_in_atm || sp->alt < 0.05 * sp->ref->Size())) {
			surface = true;
			return sp->groundvel_glob;
		}
	}
	return ref ? target->GVel() - ref->GVel() : target->GVel();
}

void Camera::StartBlend (const Body *from)
{
	if (!target) return;
	blend.on = true;
	blend.t0 = camT;
	// (relative positions: the bodies may have moved since the camera was
	// placed, by up to a frame - hundreds of metres in orbit)
	blend.off = gspos;
	if (from && from != target) blend.off += from->GPos() - target->GPos();
	blend.rot = grot;
}

void Camera::Adopt (const Matrix &F)
{
	double d = gspos.length();
	Vector dir (d > 0.0 ? -gspos / d : Col (grot, 2)); // (towards the target)
	Vector fdir (tmul (F, dir));
	rdist = rdistT = max (1.0, d / target->Size());
	if (extmode == CAMERA_LOCKED) {
		lockQ = lockQT = camrig::LookLevel (fdir, tmul (F, Col (grot, 1)), IMatrix ());
	} else {
		double yaw, pitch;
		camrig::YawPitchOf (fdir, yaw, pitch);
		ephi = yawT = yaw;
		etheta = pitchT = camrig::ClampPitch (pitch);
	}
}

void Camera::ApplyLegacy (const Legacy &l, const Body *ref)
{
	// where the classic mode put the camera: the new mode (Free; Locked
	// for target-relative) takes it from there
	if (l.mode == CAM_TARGETRELATIVE) {
		extmode = CAMERA_LOCKED;
		frameKind = -1;
		SetExtView (l.rd, l.ph, l.th);
		UpdateTrack (0.0);
		return;
	}
	double d = max (1.0, l.rd) * target->Size();
	Vector off (Col (camrig::YawPitch (l.ph, l.th), 2) * -d);
	switch (l.mode) {
	case CAM_ABSDIRECTION:
		off = mul (target->GRot(), off); // (relative to the target when it was stored)
		break;
	case CAM_TARGETTOOBJECT:
	case CAM_TARGETFROMOBJECT:
		if (ref && ref != target) {
			Vector g ((ref->GPos() - target->GPos()).unit());
			off = g * (l.mode == CAM_TARGETTOOBJECT ? -d : d);
		}
		break;
	}
	extmode = CAMERA_FREE;
	frameKind = -1;
	gspos = off;
	gpos = target->GPos() + off;
	grot = camrig::LookLevel (-off, UpAtTarget (), grot);
	bool changed;
	Matrix F (TrackFrame (0.0, changed));
	Adopt (F);
	UpdateTrack (0.0);
}

double Camera::GroundElevation (const Planet *ref, double lng, double lat, double alt) const
{
	double elev = 0.0;
	ElevationManager *emgr = ref->ElevMgr();
	if (emgr) {
		int reslvl = (int)(32.0-log(max(go.alt,100.0))*LOG2);
		elev = emgr->Elevation (lat, lng, reslvl, &etile);
	}
	return elev;
}

void Camera::SetGroundMode (ExtCamMode mode, const Body *ref, double lng, double lat, double alt, const GroundObserverSite *_gos, bool alt_above_ground)
{
	if (!ref) return;
	SetViewExternal();
	if (ref != dirref) {
		dirref = ref;
		for (int i = 0; i < etile.size(); i++)
			etile[i].Clear();
	}
	go.lng = lng;
	go.lat = lat;
	double elev = GroundElevation ((const Planet*)ref, lng, lat, alt);
	if (alt_above_ground) { // measure altitude from elevated ground
		go.alt = alt;
		go.alt0 = alt+elev;
	} else {				// measure altitude from mean radius
		go.alt0 = alt;
		go.alt = alt-elev;
	}
	double clng = cos(go.lng), slng = sin(go.lng);
	double clat = cos(go.lat), slat = sin(go.lat);
	go.R.Set ( clng*slat, clng*clat, -slng,   // rotate from local
	          -clat,      slat,       0,      // observer to local
		       slng*slat, slng*clat,  clng);  // planet coords
	extmode = mode;

	if (!_gos) gos.planet[0] = gos.site[0] = gos.addr[0] = '\0';
	else {
		strcpy (gos.planet, _gos->planet);
		strcpy (gos.site, _gos->site);
		strcpy (gos.addr, _gos->addr);
	}
}

//bool Camera::GetGroundMode (const Body **ref, double *lng, double *lat, double *alt)
//{
//	if (!external_view || extmode != CAMERA_GROUNDOBSERVER)
//		return false;
//
//	*ref = dirref;
//	*lng = go_lng;
//	*lat = go_lat;
//	*alt = go_alt;
//}

void Camera::SetGroundObserver_PanSpeed (double speed)
{
	go.panspeed = max (0.1, min (1e5, speed));
	g_pOrbiter->Cfg()->CfgCameraPrm.Panspeed = go.panspeed;
}

void Camera::SetGroundObserver_TargetLock (bool lock)
{
	if (lock != go.tgtlock) {
		go.tgtlock = lock;

		if (go.tgtlock && external_view && extmode == CAMERA_GROUNDOBSERVER) {
			gdir.Set ((target->GPos()-gpos).unit());
			Vector hdir = tmul (go.R, tmul (dirref->GRot(), gdir));
			go.tht = asin (hdir.y);
			go.phi = atan2 (-hdir.x, hdir.z);
		}
	}
}

void Camera::SetGroundObserver_TerrainLimit (double altlimit)
{
	if (altlimit != go.terrain_limit) {
		if (go.alt < go.terrain_limit && go.alt >= altlimit) { // switching from terrain mode to flat mode
			double elev = GroundElevation (planet_proxy, go.lng, go.lat, go.alt);
			go.alt0 = elev + go.alt; // recalibrate target alt0 to current pos
		}
		go.terrain_limit = altlimit;
		g_pOrbiter->Cfg()->CfgCameraPrm.TerrainLimit = go.terrain_limit;
	}
}

void Camera::GroundObserverShift (double dx, double dz, double dh)
{
	if (!external_view || extmode != CAMERA_GROUNDOBSERVER) return;
	double r;
	
	Vector dsz (grot.m13, grot.m23, grot.m33); // dz: go forward/backward w.r.t. camera view direction
	Vector dsx (grot.m11, grot.m21, grot.m31); // dx: go sideways w.r.t. camera view direction
	dirref->GlobalToEquatorial (gpos + dsz*dz + dsx*dx, go.lng, go.lat, r);
	double new_alt = max (1.0, go.alt+dh);
	go.alt0 += new_alt-go.alt;
	go.alt = new_alt;
	double clng = cos(go.lng), slng = sin(go.lng);
	double clat = cos(go.lat), slat = sin(go.lat);
	go.R.Set ( clng*slat, clng*clat, -slng,   // rotate from local
	          -clat,      slat,       0,      // observer to local
		       slng*slat, slng*clat,  clng);  // planet coords
}

void Camera::GroundObserverTilt (double dphi, double dtht)
{
	go.phi += dphi;
	go.tht += dtht;
}

void Camera::ResizeViewport (int w, int h)
{
	w05 = 0.5*w;
	h05 = 0.5*h;
	aspect = h05/w05;
	UpdateProjectionMatrix ();
}

DWORD Camera::UpdateExternalControl (ExternalCameraControl *ecc)
{
	DWORD dmode = 0;
	DWORD cmode = ECC->GetCameraMode();

	// camera control via external module
	if (external_view) {
		if (extmode == CAMERA_GROUNDOBSERVER) { // ground mode
			if (cmode & CAMMODE_GROUND) {
				ExternalCameraControl::CamData data;
				if (ECC->clbkPoll (&data)) {
					go.phi = data.yaw;
					go.tht = data.pitch;
				}	
			}
		} else { // track mode
			if (cmode & CAMMODE_TRACK) {
				ExternalCameraControl::CamData data;
				if (ECC->clbkPoll (&data)) {
					const ExternalCameraControl::TrackMode *trkmode = ECC->GetTrackMode();
					double vel, datax, datay;
					double dt = td.SysDT*trkmode->speed;
					double dz = trkmode->deadzone;
					if (trkmode->trackrotation) {
						if (trkmode->rotationdata == ExternalCameraControl::TrackMode::BYPOSITION) {
							datax = data.x, datay = data.y;
						} else {
							datax = data.yaw/PI, datay = data.pitch/PI;
						}
						vel = 2.0*(fabs(datax)-dz);
						if (vel > 0) AddPhi ((datax > 0 ? vel:-vel) * dt);
						vel = 2.0*(fabs(datay)-0.6*dz);
						if (vel > 0) AddTheta ((datay > 0 ? -vel:vel)*dt);
					}
					if (trkmode->trackzoom) {
						vel = 2.0*(fabs(data.z)-dz);
						if (vel > 0) ChangeDist (data.z > 0 ? 1.0/(1.0+vel*dt) : 1.0+vel*dt);
					}
				}
			}
		}
	} else { // cockpit camera
		DWORD extctrl = 0;
		const ExternalCameraControl::VCMode *vcmode = 0;
		bool dorot = true, dopos = false;
		double rotscale = 1.0;
		double posrange = 0.5;
		static bool frozen = false;
		static bool dragrot = false, dragpos = false;
		switch (intmode) {
		case CAMERA_VIRTUALCOCKPIT:
			extctrl = (cmode & CAMMODE_VC);
			if (extctrl) {
				vcmode = ECC->GetVCMode();
				dorot = vcmode->trackrotation;
				dopos = vcmode->trackposition;
				rotscale = vcmode->rotationrange/PI;
				posrange = vcmode->positionrange;
				if (frozen) {
					if (td.SysT0-mmoveT > vcmode->freeze_t)
						frozen = false, dragrot = dorot, dragpos = dopos, action = CAMERA_RECENTER;
				} else {
					if (vcmode->freezeonmouse && td.SysT0-mmoveT < vcmode->freeze_t)
						frozen = true;
					else if (dragrot && action == CAMERA_NORMAL)
						dragrot = false;
				}
				extctrl = !frozen;
			}
			break;
		case CAMERA_2DPANELCOCKPIT:
			extctrl = (cmode & CAMMODE_PANELCOCKPIT);
			break;
		case CAMERA_GENERICCOCKPIT:
			extctrl = (cmode & CAMMODE_GENERICCOCKPIT);
			break;
		}
		if (extctrl) {
			ExternalCameraControl::CamData data;
			if (ECC->clbkPoll (&data)) {
				if (dorot) {
					double yaw = data.yaw*rotscale;
					double pitch = data.pitch*rotscale;
					if (dragrot) {
						cntr_phi = yaw;
						cntr_tht = pitch;
					} else {
						SetCockpitDir (yaw, pitch);
						action = CAMERA_MANUAL;
						dphi = dtht = 0;
						vphi = vtht = 0;
					}
					dmode |= CAMDATA_DIR;
				}
				if (dopos) {
					double x = min (posrange, max (-posrange, data.x));
					double y = min (posrange, max (-posrange, data.y));
					double z = min (posrange, max (-posrange, data.z));
					Vector p(x,y,z);
					Vector pr(mul (isStdDir ? rrot : rrot0*rrot, p));
					if (dragpos) {
						Vector D0(pr-rpos);
						double d0 = D0.length();
						double d = td.SysDT*1.0;
						if (d > d0) dragpos = false;
						else pr = rpos + D0*(d/d0);
					}
					MoveToDirect (pr);
					dmode |= CAMDATA_POS;
				}
			}
		}
	}
	return dmode;
}

void Camera::Update ()
{
	double dt = td.UiDT ();
	double t1 = td.SysT1;
	camT += dt;

	// NOTE: Camera is updated after world state has been updated, but before state S1
	// is copied back to state S0

	ExtCtrlMode = 0;
	if (ECC) ExtCtrlMode = UpdateExternalControl (ECC);

	// get a list of current visuals
	double dist, calt, dist_proxy = 1e100;
	VObject **vobj, *vo;
	const Body *bd;
	int i, nobj;
	nobj = 0; vobj = 0;
	for (i = 0; i < nobj; i++) {
		bd = (vo = vobj[i])->GetBody();
		if (!external_view && bd == (Body*)g_focusobj &&
			!g_focusobj->HasExtpassMeshes()) continue;
		dist = vo->CDist() - bd->ClipRadius();
		if (bd->Type() == OBJTP_PLANET) {
			if (dist < 50e3) {
				dist = 0; // suppress dynamic nearplane if in proximity of the surface
			} else if (((Planet*)bd)->CloudParam (calt)) {
				double lowc;
				if (dist > calt) dist -= calt;
				else if (dist < (lowc = 0.99985*(calt+bd->Size())-bd->Size()))
					dist = lowc-dist;
				else dist = 4.0;
			}
		}
		if (dist < dist_proxy) dist_proxy = dist;
	}

	// find the largest apparent planet
	double ralt_proxy = 1e100;
	for (i = 0; i < g_psys->nPlanet(); i++) {
		Planet *planet = g_psys->GetPlanet(i);
		dist = gpos.dist (planet->GPos());
		double alt = dist - planet->Size();
		double ralt = alt/planet->Size();
		if (ralt < ralt_proxy) {
			ralt_proxy = ralt;
			alt_proxy  = alt;
			planet_proxy = planet;
		}
	}

	if (external_view) {
		if (extmode == CAMERA_GROUNDOBSERVER) {
			double rad, elev = 0.0;
			if (dirref->Type() == OBJTP_PLANET)
				elev = GroundElevation((Planet*)dirref, go.lng, go.lat, go.alt);
			if (go.alt0-elev >= go.terrain_limit) { // flat mode
				rad = dirref->Size() + go.alt0;
				go.alt = go.alt0 - elev;
			} else { // terrain-hugging mode
				rad = dirref->Size() + elev + go.alt;
			}
			dirref->EquatorialToGlobal (go.lng, go.lat, rad, gpos);
			GPOS = MakeVECTOR3 (gpos);
			gspos.Set (gpos - target->GPos());
			if (go.tgtlock) {
				gdir.Set ((target->GPos()-gpos).unit());
				Vector hdir = tmul (go.R, tmul (dirref->GRot(), gdir));
				if (fabs (hdir.y) < 0.999999) {
					go.tht = asin (hdir.y);
					go.phi = atan2 (-hdir.x, hdir.z);
				} else {
					go.tht = (hdir.y > 0 ? Pi05 : -Pi05);
					go.phi = 0.0;
				}
			}
			double sinph = sin(go.phi), cosph = cos(go.phi);
			double sinth = sin(go.tht), costh = cos(go.tht);
			rrot.Set (cosph,  sinph*sinth, -sinph*costh,
				      0.0,    costh,        sinth,
					  sinph, -cosph*sinth,  cosph*costh);
			grot.Set (dirref->GRot() * go.R * rrot);
		} else if (extmode == CAMERA_FLYBY) {
			UpdateFlyby (td.SimDT); // (the target moves in simulation time)
		} else {
			if (extmode == CAMERA_ORBITAL && !TrackModeAvailable (CAMERA_ORBITAL)) { // (into the air: Auto)
				SetTrackMode (CAMERA_AUTO);
				modeChangeT = camT;
			}
			UpdateTrack (dt);
		}
		if (has_tref) {
			double t = t1 - tref_t0;
			if (t >= dragT) {
				tref.Set(0,0,0);
				has_tref = false;
				rshift = false;
			} else {
				double scale = (1.0 - t*t/(dragT*dragT)*(3.0-2.0*t/dragT));
				double tlen = tref.length();
				if (tlen) {
					tref *= (tref_d0*scale)/tref.length();
					gpos += tref; gspos += tref;
					GPOS = MakeVECTOR3 (gpos);
				}
				if (rshift)
					rdist = rdistT = ((tref_r0-tref_r1)*scale+tref_r1)/target->Size();
			}
		}
		// a change of target or view: blended from where the camera was
		// (relative to the target: its direction and log distance, and its orientation)
		if (blend.on && extmode != CAMERA_GROUNDOBSERVER) {
			double u = (camT - blend.t0) / BLEND_T;
			if (u >= 1.0 || u < 0.0) blend.on = false;
			else {
				double k = camrig::SmoothStep (u);
				double la = blend.off.length(), lb = gspos.length();
				if (la > 1e-6 && lb > 1e-6) {
					Vector a (blend.off / la), b (gspos / lb);
					Vector ax (crossp (a, b));
					double sn = ax.length(), cs = dotp (a, b);
					Vector d (b);
					if (sn > 1e-9) d = mul (camrig::AxisAngle (ax / sn, atan2 (sn, cs) * k), a);
					else if (cs > 0.0) d = a;
					double l = exp (log (la) * (1.0 - k) + log (lb) * k);
					gspos = d * l;
					gpos = target->GPos() + gspos;
					GPOS = MakeVECTOR3 (gpos);
				}
				grot = camrig::Slerp (blend.rot, grot, k);
			}
		}
		if (extmode != CAMERA_GROUNDOBSERVER) KeepAboveTerrain ();
	} else { // cockpit mode

		double vp, vt;
		static const double dfac = 2.0*amax/vmax; // max rotation deceleration factor
		static double pf = 1e10;
		static double idleT = 0;

		switch (action) {
		case CAMERA_NORMAL:
			if (!mbdown[1] && !brot && !IsCockpitDefaultDir() && (fabs (normangle (cphi)) < catchangle) && (fabs (normangle (ctheta)) < catchangle))
				ResetCockpitDir();
			break;
		case CAMERA_RECENTER:
			pf = 1e10; // init stopping test
			action = CAMERA_RECENTER_CONT;
			// fall through
		case CAMERA_RECENTER_CONT: {
			double a = normangle (cntr_phi-cphi);
			double b = normangle (cntr_tht-ctheta);
			double fa = fabs(a), fb = fabs(b), f = fa+fb;
			if (f < 0.001 || (f < 0.01 && f > pf)) {
				SetCockpitDir (cntr_phi, cntr_tht);
				dphi = dtht = vphi = vtht = 0.0;
			  	action = CAMERA_NORMAL;
			} else {
				if (fa > fb) {
					if (a >= 0.0) dphi = min (min (min (vmax, vphi+amax*dt), a*dfac) * dt, a);
					else          dphi = max (max (max (-vmax, vphi-amax*dt), a*dfac) * dt, a);
					dtht = dphi*b/a;
				} else {
					if (b >= 0.0) dtht = min (min (min (vmax, vtht+amax*dt), b*dfac) * dt, b);
					else          dtht = max (max (max (-vmax, vtht-amax*dt), b*dfac) * dt, b);
					dphi = dtht*a/b;
				}
				vphi = dphi*td.iSysDT;
				vtht = dtht*td.iSysDT;
				pf = fa+fb;
			}
			} break;
		case CAMERA_MANUAL:
			break;
		}

		if (dphi || dtht) {
			double dp_left, dp_right, dt_up, dt_down;
			g_focusobj->CamRange (dp_left, dp_right, dt_up, dt_down);
			const double phirange = 0.8*Pi;   // make panel-dependent!
			const double thtrange = 0.8*Pi05; // make panel-dependent!

			vp = dphi*td.iSysDT;
			vt = dtht*td.iSysDT;

			double p = normangle (cphi);
			if (vp > 0) vp = min (vp,  (dp_left-p)*dfac);
			else        vp = max (vp, (-dp_right-p)*dfac);
			double t = normangle (ctheta);
			if (vt > 0) vt = min (vt,  (dt_up-t)*dfac);
			else        vt = max (vt, (-dt_down-t)*dfac);
			dphi = dtht = 0.0;
			idleT = t1;
		} else {
			vp = vt = 0.0;
		}
		if (rot_smooth) {
			if (fabs (vp-vphi)*td.iSysDT > amax) vp = (vp > vphi ? amax:-amax)*dt + vphi;
			if (fabs (vt-vtht)*td.iSysDT > amax) vt = (vt > vtht ? amax:-amax)*dt + vtht;
		}
		if (vp || vt) {
			SetCockpitDir (cphi + vp*dt, ctheta + vt*dt);
			vphi = vp, vtht = vt;
			brot = true;
		} else {
			vphi = vtht = 0.0;
			brot = (t1-idleT < 0.1);
			//brot = false;
		}

		// 'lean'
		if (movehead) {
			static double v = 0;
			const double a = 1.5, vmax = 2.0;
			Vector dr = tgtp - rpos;
			double d, dst = dr.length();
			if (dst > 1e-4) {
				if (v < vmax) v = min (vmax, v + dt*a);
				v = min (v, sqrt (0.5*a*dst));
				d = min (dst, dt * v);
				rpos += dr*(d/dst);
			} else {
				rpos = tgtp;
				movehead = false;
				v = 0;
			}
		}

		grot.Set (target->GRot() * (isStdDir ? rrot : rrot0*rrot));
		gspos.Set (mul (target->GRot(), *rofs + rpos + eyeofs));
		gpos.Set (gspos + target->GPos());
		GPOS = MakeVECTOR3 (gpos);
	}

	// dynamic nearplane calculation
	const double np_min = (external_view ? 2.5 : 1.0);
	double np_max = 1e-4*farplane;
	double np = dist_proxy;  // near-plane no further than closest object
	if (planet_proxy && alt_proxy < 0.1*planet_proxy->Size()) {
		// make sure the near-plane doesn't cut into the planet surface
		Vector gd(grot.m13,grot.m23,grot.m33);
		Vector cp(planet_proxy->GPos()-gpos);
		double alt = cp.length()-planet_proxy->Size();
		double az = acos (dotp (gd, cp.unit()));
		double a = atan (tan_ap*std::hypot(w05,h05)/h05);
		double tht = az-a;
		if (tht < Pi05)
			np = min (np, alt*cos(a)/cos(tht));
	}
	if (np < np_min) {
		if (nearplane > np_min) SetFrustumLimits (np_min, farplane);
	} else if (np > np_max) {
		if (nearplane < np_max) SetFrustumLimits (np_max, farplane);
	} else {
		if (nearplane > np) SetFrustumLimits (max (np_min, 0.5*np), farplane);
		else if (nearplane < 0.25*np) SetFrustumLimits (min (np_max, 0.5*np), farplane);
	}
#ifdef OLD_NEARPLANE
	const double minp_min = 5.0; // 2*smallest near plane
	double minp_max = 1e-3*farplane;
	if (dist_proxy < minp_min) dist_proxy = minp_min;
	else if (dist_proxy > minp_max) dist_proxy = minp_max;
	if (nearplane < 0.5*dist_proxy || nearplane > 0.9*dist_proxy)
		SetFrustumLimits (0.75*dist_proxy, farplane);
#endif

	// Update D3D view transform matrix. Note this is only a rotation,
	// no translation, since our camera is always at the origin in
	// visual world coordinates
	// Note that we need inv(inv(grot)) = grot since
	// 1. grot is rotation of camera in world coordinates, whereas D3D wants
	//    rotation from world to view coordinates
	// 2. rotations in logical interface are counterclockwise, but clockwise
	//    in D3D
	SetD3DRotation (view_mat, grot);
	pv_mat_valid = false;

	//UpdateMouse ();

}

MATRIX4 Camera::ProjectionMatrix() const
{
	MATRIX4 mat = {aspect/tan_ap,0,0,0,
		           0,1.0/tan_ap,0,0,
				   0,0,farplane/(farplane-nearplane),1.0,
				   0,0,-nearplane*farplane/(farplane-nearplane),0};
	return mat;
}

MATRIX4 Camera::ViewMatrix() const
{
	MATRIX4 mat = {grot.m11,grot.m12,grot.m13,0,
		           grot.m21,grot.m22,grot.m23,0,
				   grot.m31,grot.m32,grot.m33,0,
				   0,0,0,1};
	return mat;
}

D3DMATRIX *Camera::D3D_ProjViewMatrix ()
{
	if (!pv_mat_valid) {
		D3DMath_MatrixMultiply (pv_mat, proj_mat, view_mat);
		pv_mat_valid = true;
	}
	return &pv_mat;
}

Matrix Camera::TrackFrame (double dt, bool &changed)
{
	changed = false;
	const Body *ref = RefBody ();
	Vector up (UpAtTarget ());
	ExtCamMode m = extmode;
	if (m == CAMERA_AUTO) {
		bool orb = false;
		if (ref && !CelestialTarget ()) {
			double R = ref->Size(), alt = (target->GPos() - ref->GPos()).length() - R;
			double atm = 0.0;
			if (ref->Type() == OBJTP_PLANET && ((const Planet*)ref)->HasAtmosphere())
				atm = ((const Planet*)ref)->AtmAltLimit();
			orb = camrig::AutoOrbital (autoOrbital, alt, R, atm);
		}
		autoOrbital = orb;
		m = orb ? CAMERA_ORBITAL : CAMERA_FREE;
	}
	// Free's frame, carried along from the last one (level, not turned about up)
	auto freeFrame = [&]() {
		Matrix F;
		if (frameKind == CAMERA_FREE || frameKind == CAMERA_CHASE) return camrig::Transport (frame, up);
		if (camrig::FrameUpFwd (up, Col (target->GRot(), 2), F, 0.05)) return F; // (forward: the target's heading)
		if (camrig::FrameUpFwd (up, Col (target->GRot(), 1), F, 0.05)) return F;
		return camrig::Transport (IMatrix (), up);
	};
	int kind = m;
	Matrix F;
	switch (m) {
	case CAMERA_LOCKED:
		F = target->GRot();
		break;
	case CAMERA_INERTIAL:
		F = IMatrix ();
		break;
	case CAMERA_ORBITAL: {
		// up: the orbit's normal; forward: along the orbit (prograde)
		bool ok = false;
		if (ref) {
			Vector r (target->GPos() - ref->GPos()), v (target->GVel() - ref->GVel());
			Vector n (crossp (v, r));
			if (n.length() > 1e-6 * r.length() * v.length() && n.length() > 0.0)
				ok = camrig::FrameUpFwd (n.unit(), v, F);
		}
		if (ok) break;
		kind = CAMERA_FREE; // (no orbit to speak of: Free)
		F = freeFrame ();
		} break;
	case CAMERA_CHASE: {
		// forward: the direction of travel (the target's heading when slow), smoothed
		bool surface;
		Vector v (TravelVelocity (surface));
		double spd = v.length();
		Vector head (Col (target->GRot(), 2));
		if (fabs (dotp (head, up)) > 0.95) head = Col (target->GRot(), 1) * -1.0; // (a rocket on its pad: its belly)
		double w = camrig::SmoothStep ((spd - 1.0) / 9.0);
		Vector fwd (head * (1.0 - w));
		if (spd > 0.0) fwd += v * (w / spd);
		if (fwd.length() < 1e-6) fwd = head;
		fwd = fwd.unit();
		if (!chaseValid || dt <= 0.0) chaseFwd = fwd, chaseValid = true;
		else chaseFwd = (chaseFwd + (fwd - chaseFwd) * camrig::Follow (dt, CHASE_LAG)).unit();
		if (!camrig::FrameUpFwd (up, chaseFwd, F, 0.05))
			F = (frameKind == CAMERA_CHASE || frameKind == CAMERA_FREE) ? camrig::Transport (frame, up) : freeFrame ();
		} break;
	default:
		kind = CAMERA_FREE;
		F = freeFrame ();
		break;
	}
	if (frameKind >= 0 && kind != frameKind) changed = true;
	frame = F;
	frameKind = kind;
	return F;
}

void Camera::UpdateTrack (double dt)
{
	// ease towards the requested view
	double kr = camrig::Follow (dt, camT < settleT ? TAU_SETTLE : TAU_TURN);
	double kd = camrig::Follow (dt, TAU_ZOOM);
	if (extmode == CAMERA_LOCKED) {
		lockQ = camrig::Slerp (lockQ, lockQT, kr);
	} else {
		ephi = camrig::Wrap (ephi + camrig::Wrap (yawT - ephi) * kr);
		etheta = etheta + (pitchT - etheta) * kr;
	}
	rdist = exp (log (rdist) + (log (rdistT) - log (rdist)) * kd);

	bool changed;
	Matrix F (TrackFrame (dt, changed));
	if (changed) { // (Auto switched frames: the camera stays where it is, turning to the new up)
		StartBlend ();
		Adopt (F);
	}
	Matrix rel (extmode == CAMERA_LOCKED ? lockQ : camrig::YawPitch (ephi, etheta));
	grot.Set (F * rel);
	gspos.Set (Col (grot, 2) * (-rdist * target->Size()));
	gpos.Set (gspos + target->GPos());
	GPOS = MakeVECTOR3 (gpos);
}

void Camera::PlaceFlyby ()
{
	bool surface;
	Vector v (TravelVelocity (surface));
	double spd = v.length();
	Vector vdir (spd > 0.5 ? v / spd : Col (target->GRot(), 2));
	if (spd <= 0.5) spd = 0.0;
	Vector up (UpAtTarget ());
	double ahead;
	double pass = min (spd, FLYBY_VMAX);
	Vector off (camrig::FlybyOffset (vdir, up, pass, target->Size(), fly.side, ahead));
	fly.side = -fly.side;
	fly.ref = RefBody ();
	fly.vdir = vdir;
	fly.ahead = ahead;
	// fixed on the ground if the target is slow enough to pass it, else
	// drifting with the target (in space: an orbit next to its own)
	fly.ground = (surface && fly.ref && spd <= FLYBY_VMAX);
	if (fly.ground) {
		fly.local = tmul (fly.ref->GRot(), target->GPos() + off - fly.ref->GPos());
	} else {
		fly.off = off;
		fly.drift = vdir * pass;
	}
	fly.placed = true;
}

void Camera::UpdateFlyby (double dt)
{
	if (!fly.placed) PlaceFlyby ();
	Vector off;
	if (fly.ground && fly.ref) {
		off = fly.ref->GPos() + mul (fly.ref->GRot(), fly.local) - target->GPos();
	} else {
		fly.off -= fly.drift * dt;
		off = fly.off;
	}
	// passed (or left behind): ahead again, a cut
	bool surface;
	Vector v (TravelVelocity (surface));
	Vector vdir (fly.ground ? (v.length() > 0.5 ? v.unit() : fly.vdir) : fly.vdir);
	if (dt > 0.0 && camrig::FlybyPassed (off, vdir, fly.ahead)) {
		PlaceFlyby ();
		blend.on = false;
		off = fly.ground && fly.ref ? fly.ref->GPos() + mul (fly.ref->GRot(), fly.local) - target->GPos() : fly.off;
	}
	gspos = off;
	gpos = target->GPos() + off;
	GPOS = MakeVECTOR3 (gpos);
	// looking at the target, horizon level
	Vector up (UpAtTarget ());
	const Body *ref = RefBody ();
	if (ref && !CelestialTarget ()) up = (gpos - ref->GPos()).unit();
	if (off.length() > 1e-6) grot = camrig::LookLevel (-off, up, grot);
	rdist = rdistT = max (1.0, off.length() / target->Size());
}

void Camera::KeepAboveTerrain ()
{
	// (the camera's distance to its target is unchanged in the scene's state:
	// lifted each frame, it slides over the terrain and returns to its line
	// where that is clear)
	const double CLEARANCE = 2.0; // [m] above the elevation (the terrain's mesh lies between its samples)
	const Planet *p = planet_proxy;
	if (!p || alt_proxy > 0.05 * p->Size()) return;
	ElevationManager *emgr = p->ElevMgr();
	if (!emgr) return;
	if (p != clampPlanet) {
		clampTile.clear ();
		clampTile.resize (8);
		clampPlanet = p;
	}
	double lng, lat, rad;
	p->GlobalToEquatorial (gpos, lng, lat, rad);
	double h = rad - p->Size();
	int lvl = (int)(32.0 - log (max (h, 100.0)) * LOG2);
	double elev = emgr->Elevation (lat, lng, lvl, &clampTile);
	double rmin = p->Size() + elev + CLEARANCE;
	// (and above trees, rocks: not inside a crown after a crash)
	if (h < 100.0) if (scatter::GroundScatter *sc = p->Scatter ()) if (sc->Active ()) {
		static std::vector<const scatter::Obstacle*> obs;
		obs.clear ();
		Vector pl (tmul (p->GRot(), gpos - p->GPos()));
		sc->Obstacles (pl, 3.0, obs);
		Vector upl (pl.unit());
		for (const scatter::Obstacle *o : obs) {
			const Vector &c = o->hull.centre;
			Vector dc (pl - c);
			double across = (dc - upl * dotp (dc, upl)).length();
			if (across < o->hull.radius) rmin = max (rmin, c.length() + o->hull.radius + 1.0);
		}
	}
	if (rad >= rmin || rad <= 0.0) return;
	Vector up ((gpos - p->GPos()) / rad);
	Vector np (p->GPos() + up * rmin);
	// turned by the angle between the old and the new direction to the target
	Vector d0 (target->GPos() - gpos), d1 (target->GPos() - np);
	double l0 = d0.length(), l1 = d1.length();
	if (l0 > 1e-6 && l1 > 1e-6) {
		d0 /= l0, d1 /= l1;
		Vector ax (crossp (d0, d1));
		double s = ax.length(), c = dotp (d0, d1);
		if (s > 1e-9) {
			ax /= s;
			double t = 1.0 - c;
			Matrix R (
				c + ax.x*ax.x*t,      ax.x*ax.y*t - ax.z*s, ax.x*ax.z*t + ax.y*s,
				ax.y*ax.x*t + ax.z*s, c + ax.y*ax.y*t,      ax.y*ax.z*t - ax.x*s,
				ax.z*ax.x*t - ax.y*s, ax.z*ax.y*t + ax.x*s, c + ax.z*ax.z*t);
			grot.premul (R);
		}
	}
	gpos.Set (np);
	gspos.Set (gpos - target->GPos());
	GPOS = MakeVECTOR3 (gpos);
}

void Camera::UpdateProjectionMatrix ()
{
	ZeroMemory (&proj_mat, sizeof (D3DMATRIX));
	proj_mat._11 = (FLOAT)(aspect / tan_ap);
	proj_mat._22 = (FLOAT)(1.0    / tan_ap);
	if (farplane >= 1e20) {
		proj_mat._33 = 1.0f;
		proj_mat._43 = -nearplane;
	} else {
		proj_mat._43 = (proj_mat._33 = farplane / (farplane-nearplane)) * (-nearplane);
	}
	proj_mat._34 = 1.0f;

	// register new projection matrix with device
    //pDev->SetTransform (D3DTRANSFORMSTATE_PROJECTION, &proj_mat);
}

void Camera::InitState (const char *scn, Body *default_target)
{
	// set defaults
	target = default_target;
	external_view = (target->Type() != OBJTP_VESSEL);
	ap_int = ap_ext = RAD*25.0;
	ap = (external_view ? &ap_ext : &ap_int);

	// read state from scenario file
	if (scn) {
		ifstream ifs (g_pOrbiter->ScnPath(scn));
		if (ifs) Read (ifs);
	}
	Body *newtgt = target; target = NULL;
	int mode = (external_view ? 1:0); external_view = true;
	Attach (newtgt, mode);
	if (extmode == CAMERA_GROUNDOBSERVER)
		SetGroundMode (CAMERA_GROUNDOBSERVER, dirref, go.lng, go.lat, go.alt);
	else if (legacy.mode && external_view)
		ApplyLegacy (legacy, dirref); // (a classic track mode: the closest new one, the camera where it was)
	legacy.mode = 0;
	SetAperture (*ap, true, true);
}

void Camera::ClearPresets ()
{
	if (npreset) {
		for (DWORD i = 0; i < npreset; i++)
			delete preset[i];
		delete []preset;
		preset = NULL;
		npreset = 0;
	}
}

DWORD Camera::AddPreset (CameraMode *mode)
{
	if (!mode) mode = GetCMode();
	CameraMode **tmp = new CameraMode*[npreset+1]; TRACENEW
	if (npreset) {
		memcpy (tmp, preset, npreset*sizeof(CameraMode*));
		delete []preset;
	}
	preset = tmp;
	preset[npreset] = mode;
	return npreset++;
}

bool Camera::DelPreset (DWORD idx)
{
	DWORD i, j;
	if (idx >= npreset) return false;
	CameraMode **tmp = 0;
	if (npreset > 1) {
		tmp = new CameraMode*[npreset-1]; TRACENEW
		for (i = j = 0; i < npreset; i++)
			if (i != idx) tmp[j++] = preset[i];
	}
	delete []preset;
	preset = tmp;
	if (npreset > 0) npreset--;
	return true;
}

void Camera::RecallPreset (DWORD idx)
{
	if (idx >= npreset) return; // out of range
	SetCMode (preset[idx]);
}

CameraMode *Camera::GetPreset (DWORD idx)
{
	if (idx >= npreset) return 0;
	return preset[idx];
}

bool Camera::Read (ifstream &ifs)
{
	char cbuf[256] = "", ctrackmode[64] = "", cdirref[64] = "", * pc = NULL;
	Body *tg = 0;
	double rd = 4.0, ph = 0.0, th = 0.0;
	int n = 0;
	go.tgtlock = true;

	if (!FindLine (ifs, "BEGIN_CAMERA")) return false;
	for (;;) {
		if (!ifs.getline (cbuf, 256)) break;
		pc = trim_string (cbuf);
		if (!_strnicmp (pc, "END_CAMERA", 10)) break;
		if (!_strnicmp (pc, "TARGET", 6)) {
			pc = trim_string (pc+6);
			if (!(tg = g_psys->GetObj (pc, true)))
				tg = g_psys->GetBase (pc, true);
		} else if (!_strnicmp (pc, "MODE", 4)) {
			pc = trim_string (pc+4);
			if (!_strnicmp (pc, "Extern", 6)) external_view = true;
			else                             external_view = false;
		} else if (!_strnicmp (pc, "POS", 3)) {
			n = sscanf (pc+3, "%lf%lf%lf", &rd, &ph, &th);
			ph *= RAD, th *= RAD;
		} else if (!_strnicmp (pc, "FOV", 3)) {
			double a;
			n = sscanf (pc+3, "%lf", &a);
			if (a < 10.0) a = 10.0;
			else if (a > 160.0) a = 160.0;
			a *= RAD*0.5;
			ap_int = ap_ext = a;
		} else if (!_strnicmp (pc, "TRACKMODE", 9)) {
			n = sscanf (pc+9, "%s%s", ctrackmode, cdirref);
		} else if (!_strnicmp (pc, "GROUNDLOCATION", 14)) {
			n = sscanf (pc+14, "%lf%lf%lf", &go.lng, &go.lat, &go.alt);
			go.lng *= RAD, go.lat *= RAD;
		} else if (!_strnicmp (pc, "GROUNDDIRECTION", 15)) {
			n = sscanf (pc+15, "%lf%lf", &go.phi, &go.tht);
			go.phi *= RAD, go.tht *= RAD;
			go.tgtlock = false;
		} else if (!_strnicmp (pc, "BEGIN_PRESET", 12)) {
			for (;;) {
				if (!ifs.getline (cbuf, 256)) break;
				pc = trim_string (cbuf);
				if (!_strnicmp (pc, "END_PRESET", 10)) break;
				AddPreset (CameraMode::Create (pc));
			}
		}
	}
	if (tg && external_view) target = tg;
	legacy.mode = 0;
	if (external_view) {
		rdist = rdistT = max (1.0, rd);
		static const struct { const char *name; ExtCamMode mode; } modes[] = {
			{"Auto", CAMERA_AUTO}, {"Free", CAMERA_FREE}, {"Orbital", CAMERA_ORBITAL}, {"Chase", CAMERA_CHASE},
			{"Locked", CAMERA_LOCKED}, {"Flyby", CAMERA_FLYBY}, {"Inertial", CAMERA_INERTIAL}
		};
		int found = -1;
		for (auto &m : modes) if (!_stricmp (ctrackmode, m.name)) found = m.mode;
		if (found >= 0) {
			extmode = (ExtCamMode)found;
		} else if (!_stricmp (ctrackmode, "Ground") && (dirref = g_psys->GetObj (cdirref, true))) {
			extmode = CAMERA_GROUNDOBSERVER;
		} else {
			// the classic modes (applied in InitState, where the target is known)
			extmode = CAMERA_FREE;
			legacy.rd = rd, legacy.ph = ph, legacy.th = th;
			if (!_stricmp (ctrackmode, "AbsoluteDirection"))
				legacy.mode = CAM_ABSDIRECTION;
			else if (!_stricmp (ctrackmode, "GlobalFrame") || !_stricmp (ctrackmode, "Global"))
				legacy.mode = CAM_GLOBALFRAME;
			else if (!_stricmp (ctrackmode, "TargetTo") && (dirref = g_psys->GetObj (cdirref, true)))
				legacy.mode = CAM_TARGETTOOBJECT;
			else if (!_stricmp (ctrackmode, "TargetFrom") && (dirref = g_psys->GetObj (cdirref, true)))
				legacy.mode = CAM_TARGETFROMOBJECT;
			else // TargetRelative (and none given)
				extmode = CAMERA_LOCKED;
		}
		if (extmode == CAMERA_LOCKED) lockQ = lockQT = camrig::YawPitch (ph, th);
		else ephi = yawT = camrig::Wrap (ph), etheta = pitchT = camrig::ClampPitch (th);
	}
	return true;
}

void Camera::Write (ostream &ofs) const
{
	ofs << "BEGIN_CAMERA" << endl;
	ofs << setprecision(2);
	ofs << "  TARGET " << target->Name() << endl;
	ofs << "  MODE " << (external_view ? "Extern":"Cockpit") << endl;
	if (external_view) {
		ofs << "  POS " << setprecision(6) << rdist << ' ' << DEG*Phi() << ' ' << DEG*Theta() << setprecision(2) << endl;
		ofs << "  TRACKMODE ";
		switch (extmode) {
		case CAMERA_GROUNDOBSERVER:
			ofs << "Ground " << dirref->Name() << endl;
			ofs << setprecision(5) << "  GROUNDLOCATION "
				<< go.lng*DEG << ' ' << go.lat*DEG << ' '
				<< setprecision(2) << go.alt << endl;
			if (!go.tgtlock)
				ofs << "  GROUNDDIRECTION " << go.phi*DEG << ' ' << go.tht*DEG << endl;
			break;
		default:
			ofs << ExtModeName (extmode) << endl;
			break;
		}
	}
	ofs << "  FOV " << 2.0*DEG* *ap << endl;
	if (npreset) {
		char cbuf[256] = "    ";
		ofs << "  BEGIN_PRESET" << endl;
		for (DWORD i = 0; i < npreset; i++) {
			preset[i]->Store (cbuf+4);
			ofs << cbuf << endl;
		}
		ofs << "  END_PRESET" << endl;
	}
	ofs << "END_CAMERA" << endl << endl;
}


// ============================================================
// Implementation of external camera control interface

bool Camera::RegisterExternalControl (ExternalCameraControl *ecc)
{
	if (ECC) {
		return false; // external control already registered
	} else {
		ECC = ecc;
		return true;
	}
}

bool Camera::UnregisterExternalControl ()
{
	if (!ECC) {
		return false; // no external control registered
	} else {
		ECC = 0;
		return true;
	}
}


// ============================================================
// ============================================================
// Implementation of CameraMode_XXX API interface classes
// ============================================================
// ============================================================

// ============================================================

CameraMode::CameraMode ()
{
	target = 0;  // use current target by default
	fov = 0;     // use current field of view by default
}

void CameraMode::SetTarget (OBJHANDLE hTgt)
{
	target = hTgt;
}

void CameraMode::SetFOV (double FOV)
{
	fov = FOV;
}

CameraMode *CameraMode::Create (char *str)
{
	CameraMode *cm;
	char *pc, *tc;

	if (!(pc = strtok (str, ":"))) return 0;
	tc = trim_string (pc);
	if (!_stricmp (tc, "Cockpit")) {
		cm = new CameraMode_Cockpit; TRACENEW
	} else if (!_stricmp (tc, "Track")) {
		cm = new CameraMode_Track; TRACENEW
	} else if (!_stricmp (tc, "Ground")) {
		cm = new CameraMode_Ground; TRACENEW
	} else {
		cm = new CameraMode_Cockpit; TRACENEW
	}
	
	if (!(pc = strtok (NULL, ":")) || !(cm->target = (OBJHANDLE)g_psys->GetObj (trim_string (pc), true))) {
		delete cm;
		return 0;
	}

	pc = strtok (NULL, ":");
	if (pc) sscanf (pc, "%lf", &cm->fov);

	pc = strtok (NULL, "");
	if (pc) cm->Init (trim_string (pc));

	return cm;
}

// ============================================================

CameraMode_Cockpit::CameraMode_Cockpit (): CameraMode ()
{
	cmode = CM_CURRENT;
	pos = -1;
	lean = -1;
	lean_smooth = false;
}

void CameraMode_Cockpit::Init (char *str)
{
	if (!str || str[0] == '\0') return;

	if (!strnicmp(str, "generic", 7)) {
		cmode = CM_GENERIC;
		str += 7;
	} else if (!strnicmp(str, "panel2d", 7)) {
		cmode = CM_PANEL2D;
		str += 7;
		if (str[0] == ':' && sscanf(++str, "%d", &pos))
			while (*str != ' ' && *str != '\0') str++;
	} else if (!strnicmp(str, "vc", 2)) {
		cmode = CM_VC;
		str += 2;
		if (str[0] == ':' && sscanf(++str, "%d", &pos)) {
			while (*str != ' ' && *str != '\0' && *str != ':') str++;
			if (*str == ':' && sscanf(++str, "%d", &lean)) {
				lean_smooth = false;
				while (*str != ' ' && *str != '\0' && toupper(*str) != 'S') str++;
				if (toupper(*str) == 'S') {
					lean_smooth = true;
					str++;
				}
			}
		}
	} else if (!strnicmp(str, "current", 7)) {
		cmode = CM_CURRENT;
		str += 7;
	}
	
}

void CameraMode_Cockpit::GetDescr (char *str, int len)
{
	char cbuf[256] = "";
	if (target) strcat (cbuf, ((Body*)target)->Name()), strcat (cbuf, " ");
	strcat (cbuf, "Cockpit ");
	strncpy (str, cbuf, len-1);
}

void CameraMode_Cockpit::Store (char *str)
{
	sprintf (str, "Cockpit:%s:%0.2f", target ? ((Body*)target)->Name() : "-", fov);
}

// ============================================================

CameraMode_Track::CameraMode_Track (): CameraMode ()
{
	reldist = 0;     // use current distance and position
	tmode = TM_CURRENT; // use current track mode
}

void CameraMode_Track::Init (char *str)
{
	char tm[64], rf[256];
	sscanf (str, "%s%lf%lf%lf%s", tm, &reldist, &phi, &theta, rf);
	if (!_stricmp (tm, "RELATIVE"))
		tmode = TM_RELATIVE;
	else if (!_stricmp (tm, "ABSDIR"))
		tmode = TM_ABSDIR;
	else if (!_stricmp (tm, "GLOBAL"))
		tmode = TM_GLOBAL;
	else if (!_stricmp (tm, "AUTO"))
		tmode = TM_AUTO;
	else if (!_stricmp (tm, "FREE"))
		tmode = TM_FREE;
	else if (!_stricmp (tm, "ORBITAL"))
		tmode = TM_ORBITAL;
	else if (!_stricmp (tm, "CHASE"))
		tmode = TM_CHASE;
	else if (!_stricmp (tm, "LOCKED"))
		tmode = TM_LOCKED;
	else if (!_stricmp (tm, "FLYBY"))
		tmode = TM_FLYBY;
	else if (!_stricmp (tm, "TARGETTOREF")) {
		tmode = TM_TARGETTOREF;
		Body *r = g_psys->GetObj (rf, true);
		if (r) ref = (OBJHANDLE)r;
		else tmode = TM_CURRENT;
	} else if (!_stricmp (tm, "TARGETFROMREF")) {
		tmode = TM_TARGETFROMREF;
		Body *r = g_psys->GetObj (rf, true);
		if (r) ref = (OBJHANDLE)r;
		else tmode = TM_CURRENT;
	}
}

void CameraMode_Track::Store (char *str)
{
	static const char *tmstr[12] = {"CURRENT","RELATIVE", "ABSDIR", "GLOBAL", "TARGETTOREF", "TARGETFROMREF",
		"AUTO", "FREE", "ORBITAL", "CHASE", "LOCKED", "FLYBY"};
	sprintf (str, "Track:%s%:%0.2f:%s %0.3f %0.3f %0.3f", 
		target ? ((Body*)target)->Name() : "-", fov,
		tmstr[tmode], reldist, phi, theta);
	if (tmode == TM_TARGETTOREF || tmode == TM_TARGETFROMREF) {
		strcat (str, " ");
		strcat (str, ((Body*)ref)->Name());
	}
}

void CameraMode_Track::GetDescr (char *str, int len)
{
	char cbuf[256] = "";
	if (target) strcat (cbuf, ((Body*)target)->Name()), strcat (cbuf, " ");
	strcat (cbuf, "Track ");
	switch (tmode) {
	case TM_RELATIVE: strcat (cbuf, "relative "); break;
	case TM_ABSDIR:   strcat (cbuf, "fixed "); break;
	case TM_GLOBAL:   strcat (cbuf, "global "); break;
	case TM_AUTO:     strcat (cbuf, "auto "); break;
	case TM_FREE:     strcat (cbuf, "free "); break;
	case TM_ORBITAL:  strcat (cbuf, "orbital "); break;
	case TM_CHASE:    strcat (cbuf, "chase "); break;
	case TM_LOCKED:   strcat (cbuf, "locked "); break;
	case TM_FLYBY:    strcat (cbuf, "flyby "); break;
	default: break;
	}
	strncpy (str, cbuf, len-1);
}

void CameraMode_Track::SetTrackMode (TrackMode trackmode, OBJHANDLE refobj)
{
	tmode = trackmode;
	ref   = refobj;
}

void CameraMode_Track::SetPosition (double rd, double ph, double th)
{
	reldist = rd;
	phi     = ph;
	theta   = th;
}

void CameraMode_Track::GetPosition (double *rd, double *ph, double *th) const
{
	*rd = reldist;
	*ph = phi;
	*th = theta;
}

// ============================================================

CameraMode_Ground::CameraMode_Ground (): CameraMode ()
{
	ref = 0;    // use current planet
	alt = 0;    // use current position
	alt_above_ground = true;
	tgtlock = true;
}

void CameraMode_Ground::Init (char *str)
{
	char rf[256], alt_mode;
	int i = sscanf (str, "%s%lf%lf%lf%c%lf%lf", rf, &lng, &lat, &alt, &alt_mode, &phi, &theta);
	lng *= RAD, lat *= RAD;
	alt_above_ground = (toupper(alt_mode) != 'M');
	tgtlock = (i < 7);
	ref = (OBJHANDLE)g_psys->GetObj (rf, true);
}

void CameraMode_Ground::Store (char *str)
{
	sprintf (str, "Ground:%s:%0.2f:%s %0.5f %0.5f %0.2f%s",
		target ? ((Body*)target)->Name() : "-", fov,
		((Body*)ref)->Name(), lng*DEG, lat*DEG, alt, alt_above_ground ? "" : "M");
	if (!tgtlock) sprintf (str+strlen(str), " %0.2f %0.2f",
		phi, theta);
}

void CameraMode_Ground::GetDescr (char *str, int len)
{
	char cbuf[256] = "";
	if (target) strcat (cbuf, ((Body*)target)->Name()), strcat (cbuf, " ");
	strcat (cbuf, "Ground ");
	strncpy (str, cbuf, len-1);
}

void CameraMode_Ground::SetPosition (double longitude, double latitude, double altitude, OBJHANDLE hRef)
{
	lng = longitude;
	lat = latitude;
	alt = altitude;
	if (hRef) ref = hRef;
}

void CameraMode_Ground::GetPosition (double *longitude, double *latitude, double *altitude, OBJHANDLE *hRef) const
{
	*longitude = lng;
	*latitude  = lat;
	*altitude  = alt;
	*hRef      = ref;
}

void CameraMode_Ground::SetOrientation (double ph, double th)
{
	phi = ph;
	theta = th;
	tgtlock = false;
}

void CameraMode_Ground::GetOrientation (double *ph, double *th) const
{
	*ph = phi;
	*th = theta;
}
