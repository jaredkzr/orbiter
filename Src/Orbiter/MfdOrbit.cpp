// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MfdOrbit.h"
#include "Pane.h"
#include "Celbody.h"
#include "Psys.h"
#include "Select.h"
#include "Log.h"
#include "Maneuver.h"
#include "MFDUI.h"
#include <algorithm>
#include <cmath>

using namespace std;

extern PlanetarySystem *g_psys;
extern TimeData td;
extern Pane *g_pane;
extern InputBox *g_input;
extern Select *g_select;
extern char DBG_MSG[256];

// =======================================================================
// class Instrument_Orbit

struct Instrument_Orbit::SavePrm Instrument_Orbit::saveprm = {
	0, 0, 0,
	Instrument_Orbit::PRJ_FRM,
	Instrument_Orbit::FRM_ECL,
	Instrument_Orbit::DIST_RAD,
	Instrument_Orbit::DISP_BOTH
};

Instrument_Orbit::Instrument_Orbit (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel)
: Instrument (_pane, _id, spec, _vessel)
{
	projmode = saveprm.projmode;
	frmmode  = saveprm.frmmode;
	dstmode  = saveprm.dstmode;
	dispmode = saveprm.dispmode;
	if (_vessel == saveprm.usr) {
		elref = (saveprm.elref ? saveprm.elref : _vessel->ElRef());
		tgt = saveprm.tgt;
	} else {
		elref = _vessel->ElRef();
		tgt = 0;
	}
	shpel = new Elements(); TRACENEW
	tgtel = new Elements(); TRACENEW
	if (elref) {
		shpel->Setup (_vessel->Mass(), elref->Mass(), _vessel->Els()->MJDepoch());
		if (tgt)
			tgtel->Setup (tgt->Mass(), elref->Mass(), _vessel->Els()->MJDepoch());
	}

	SetSize (spec);
	strcpy (title_str, "Orbit: ");
	strcpy (proj_str, "Proj: ");

	// init drawing resources
	if (gc) {
		brush[0]    = gc->clbkCreateBrush (col_green2);
		brush[1]    = gc->clbkCreateBrush (col_yellow2);
	} else {
		int i;
		for (i = 0; i < 2; i++) brush[i] = 0;
	}
}

Instrument_Orbit::~Instrument_Orbit ()
{
	int i;
	if (gc) {
		for (i = 0; i < 2; i++)
			if (brush[i]) gc->clbkReleaseBrush (brush[i]);
	}
	delete shpel;
	delete tgtel;

	// save status
	saveprm.usr      = vessel;
	saveprm.elref    = elref;
	saveprm.tgt      = tgt;
	saveprm.projmode = projmode;
	saveprm.frmmode  = frmmode;
	saveprm.dstmode  = dstmode;
	saveprm.dispmode = dispmode;
}

HELPCONTEXT *Instrument_Orbit::HelpTopic () const
{
	extern HELPCONTEXT DefHelpContext;
	DefHelpContext.topic = (char*)"/mfd_orbit.htm";
	return &DefHelpContext;
}

bool Instrument_Orbit::KeyBuffered (DWORD key)
{
	switch (key) {
	case OAPI_KEY_A:  // auto reference
		SelectAutoRef ();
		return true;
	case OAPI_KEY_D:  // toggle distance display mode
		dstmode = (dstmode == DIST_RAD ? DIST_ALT : DIST_RAD);
		Refresh();
		return true;
	case OAPI_KEY_F:  // reference frame
		frmmode = (frmmode == FRM_ECL ? FRM_EQU : FRM_ECL);
		Refresh();
		return true;
	case OAPI_KEY_H:  // copy reference to HUD
		CopyToHUD ();
		return true;
	case OAPI_KEY_M:  // display mode
		dispmode = (DisplayMode)((((int)dispmode)+1) % 3);
		Refresh();
		return true;
	case OAPI_KEY_N:  // deselect target
		UnselectTarget ();
		return true;
	case OAPI_KEY_P:  // projection plane
		projmode = (ProjectionMode)((((int)projmode)+1) % 3);
		if (projmode == PRJ_TGT && !tgt) projmode = PRJ_FRM;
		Refresh();
		return true;
	case OAPI_KEY_R:  // select reference
		OpenSelect_CelBody ("Orbit MFD: Reference", ClbkEnter_Ref);
		return true;
	case OAPI_KEY_T:  // select target
		OpenSelect_Tgt ("Orbit MFD: Target", ClbkEnter_Tgt, elref, 0);
		return true;
	}
	return false;
}

bool Instrument_Orbit::ProcessButton (int bt, int event)
{
	static const DWORD btkey[9] = { OAPI_KEY_R, OAPI_KEY_A, OAPI_KEY_T, OAPI_KEY_N, OAPI_KEY_M, OAPI_KEY_F, OAPI_KEY_P, OAPI_KEY_D, OAPI_KEY_H };
	if (event & PANEL_MOUSE_LBDOWN) {
		if (bt < 9) return KeyBuffered (btkey[bt]);
	}
	return false;
}

const char *Instrument_Orbit::BtnLabel (int bt) const
{
	static const char *label[9] = { "REF", "AR", "TGT", "NT", "MOD", "FRM", "PRJ", "DST", "HUD" };
	return (bt < 9 ? label[bt] : 0);
}

int Instrument_Orbit::BtnMenu (const MFDBUTTONMENU **menu) const
{
	static const MFDBUTTONMENU mnu[9] = {
		{"Orbit reference", 0, 'R'},
		{"Auto reference", 0, 'A'},
		{"Select target", 0, 'T'},
		{"Unselect target", 0, 'N'},
		{"Display mode", 0, 'M'},
		{"Reference frame", "ECL/EQU", 'F'},
		{"Projection plane", 0, 'P'},
		{"Distance display", "Radius/Altitude", 'D'},
		{"Copy data to HUD", 0, 'H'}
	};
	if (menu) *menu = mnu;
	return 9;
}

bool Instrument_Orbit::ClbkEnter_Tgt (Select *menu, int item, char *str, void *data)
{
	Instrument_Orbit *instr = (Instrument_Orbit*)data;
	return instr->SelectTarget (str);
}

bool Instrument_Orbit::ClbkEnter_Ref (Select *menu, int item, char *str, void *data)
{
	Instrument_Orbit* instr = (Instrument_Orbit*)data;
	return instr->SelectRef (str);
}

void Instrument_Orbit::SetRef (const CelestialBody *ref)
{
	if (ref && ref != elref) {
		elref = ref;
		shpel->SetMasses (0.0, elref->Mass());
		tgtel->SetMasses (0.0, elref->Mass());
		if (tgt == (const Body*)ref) UnselectTarget();
		Refresh();
	}
}

bool Instrument_Orbit::SelectRef (char *str)
{
	CelestialBody *obj = g_psys->GetGravObj (str, true);
	if (!obj) return false;
	SetRef (obj);
	return true;
}

void Instrument_Orbit::SelectAutoRef ()
{
	SetRef (vessel->ElRef());
}

bool Instrument_Orbit::SelectTarget (char *str)
{
	Body *body = g_psys->GetObj (str, true);
	if (body == (Body*)elref) return false;
	if (body) {
		tgt = body;
		if (elref)
			tgtel->Setup (tgt->Mass(), elref->Mass(), shpel->MJDepoch());
		Refresh();
		return true;
	} else return false;
}

void Instrument_Orbit::UnselectTarget ()
{
	tgt = 0;
	if (projmode == PRJ_TGT) projmode = PRJ_FRM;
	Refresh();
}

void Instrument_Orbit::CopyToHUD () const
{
	g_pane->SetHUDMode (HUD_ORBIT);
	HUD *hud = g_pane->GetHUD();
	if (hud && hud->Mode() == HUD_ORBIT) {
		((HUD_Orbit*)hud)->SetReference (elref);
	}
}

void Instrument_Orbit::DisplayOrbit (oapi::Sketchpad *skp, int which, oapi::IVECTOR2 *p)
{
	skp->SetPen (GetDefaultPen (which));
	if (p[ELN+2].x != -1) { // closed orbit
		// ellipse
		skp->Polygon (p, ELN);
		// apoapsis
		oapi::IVECTOR2 &pa = p[ELN+2];
		skp->Ellipse (pa.x-3, pa.y-3, pa.x+4, pa.y+4);
	} else {                // open orbit
		// hyperbola
		skp->Polyline (p, ELN-1);
	}
	// radius vector
	skp->Line (ICNTX, ICNTY, p[ELN].x, p[ELN].y);
	// periapsis
	oapi::IVECTOR2 &pe = p[ELN+1];
	skp->SetBrush (brush[which]);
	skp->Ellipse (pe.x-3, pe.y-3, pe.x+4, pe.y+4);
	// ascending node
	oapi::IVECTOR2 &asc = p[ELN+3];
	skp->Rectangle (asc.x-3, asc.y-3, asc.x+4, asc.y+4);
	skp->SetBrush (0);
	// descending node
	oapi::IVECTOR2 &desc = p[ELN+4];
	skp->Rectangle (desc.x-3, desc.y-3, desc.x+4, desc.y+4);
	// line of nodes
	skp->SetPen (GetDefaultPen (which, 0, 2));
	skp->Line (asc.x, asc.y, desc.x, desc.y);
}

void Instrument_Orbit::UpdateDraw (oapi::Sketchpad *skp)
{
	static const char *projstr[3] = {"Ecliptic", "Ship", "Target"};
	Matrix rot1, rot2, irot;

	bool bValidShpEl = (elref != 0);
	bool bValidTgtEl = (elref && tgt);

	if (bValidShpEl) {
		Vector pos = vessel->GPos()-elref->GPos();
		Vector vel = vessel->GVel()-elref->GVel();
		if (frmmode == FRM_EQU) { // convert to equatorial frame
			pos = tmul (elref->RotObliq(), pos);
			vel = tmul (elref->RotObliq(), vel);
		}
		shpel->Calculate (pos, vel, td.SimT1);
		strcpy (title_str+7, elref->Name());
		strcpy (proj_str+6, projstr[projmode]);
		instable = (shpel->PeDist() < elref->Size());
		scale = pixrad / (shpel->e < 1.0 ? shpel->ApDist() :
						  max (2.0*shpel->PeDist(), shpel->Radius()));
	}
	if (bValidTgtEl) {
		Vector pos = tgt->GPos()-elref->GPos();
		Vector vel = tgt->GVel()-elref->GVel();
		if (frmmode == FRM_EQU) { // convert to equatorial frame
			pos = tmul (elref->RotObliq(), pos);
			vel = tmul (elref->RotObliq(), vel);
		}
		tgtel->Calculate (pos, vel, td.SimT1);
		double scale2 = (tgtel->e < 1.0 ? pixrad / tgtel->ApDist() : scale);
		if (scale2 < scale) scale = scale2;
	} else {
		if (projmode == PRJ_TGT) projmode = PRJ_SHIP;
	}

	scale *= zoom;
	if (elref)
		elref_rad = (int)(elref->Size() * scale + 0.5);

	v4 = skp->GetVersion () >= 4;
	mfdui::Context &ui = UI ();
	ui.Begin (skp, (float)IW, (float)IH);

	if (dispmode != DISP_LIST) { // draw orbit graphs
		bool eclproj;
		skp->SetTextColor (draw[2][0].col);
		skp->Text (IW-(cw*23)/2, 1, frmmode == FRM_ECL ? "ECL":"EQU", 3);
		skp->Text (IW-(cw*7)/2,  1, projmode == PRJ_SHIP ? "SHP" : projmode == PRJ_TGT ? "TGT" : frmmode == FRM_ECL ? "ECL":"EQU", 3);
		skp->SetTextColor (draw[2][1].col);
		skp->Text (IW - cw*15, 1, "Frm     Prj", 11);
		if (bValidShpEl && projmode == PRJ_SHIP) {
			irot = IRotMatrix (shpel->cost, shpel->sint, shpel->cosi, shpel->sini);
			eclproj = false;
		} else if (bValidTgtEl && projmode == PRJ_TGT) {
			irot = IRotMatrix (tgtel->cost, tgtel->sint, tgtel->cosi, tgtel->sini);
			eclproj = false;
		} else {
			irot.Set (IMatrix());
			eclproj = true;
		}
		if (v4) {
			ui.Circle ((float)ICNTX, (float)ICNTY, (float)(elref->Size() * scale), draw[2][1].col, 1.0f);
			if (bValidTgtEl) DisplayOrbitF (1, tgtel, irot);
			if (bValidShpEl) {
				DisplayOrbitF (0, shpel, irot);
				DrawInteractive (irot);
			}
		} else {
		skp->SetPen (GetDefaultPen (2, 1));
		skp->Ellipse (ICNTX-elref_rad, ICNTY-elref_rad, ICNTX+elref_rad, ICNTY+elref_rad);
		if (bValidShpEl) {
			rot1 = RotMatrix (shpel->coso, shpel->sino, shpel->cost, shpel->sint, shpel->cosi, shpel->sini);
			if (!eclproj) rot1.premul (irot);
			UpdateOrbitGraph (ICNTX, ICNTY, IW, IH, scale, shpel, rot1, irot, o_pt1);
			DisplayOrbit (skp, 0, o_pt1);
		}
		if (bValidTgtEl) {
			rot2 = RotMatrix (tgtel->coso, tgtel->sino, tgtel->cost, tgtel->sint, tgtel->cosi, tgtel->sini);
			if (!eclproj) rot2.premul (irot);
			UpdateOrbitGraph (ICNTX, ICNTY, IW, IH, scale, tgtel, rot2, irot, o_pt2);
			DisplayOrbit (skp, 1, o_pt2);
		}
		}
	} else {
		skp->SetTextColor (draw[2][0].col);
		skp->Text (IW-(cw*23)/2, 1, frmmode == FRM_ECL ? "ECL":"EQU", 3);
		skp->SetTextColor (draw[2][1].col);
		skp->Text (IW - cw*15, 1, "Frm", 3);
	}

	DisplayTitle (skp, title_str);
	// output elref's contribution to gravity field
	bool dominant;
	double gfrac = g_psys->GetGravityContribution (elref, vessel->GPos(), &dominant);
	char cbuf[20];
	sprintf (cbuf, "G %0.2f", gfrac);
	skp->SetTextColor (draw[!dominant ? 3 : gfrac < 0.0 ? 1 : 0][0].col);
	skp->Text ((IW-cw*6)/2, IH-(3*ch)/2, cbuf, 6);

	if (dispmode != DISP_GRAPH) { // list ship and target orbit parameters
		if (bValidShpEl) {
			skp->SetTextColor (draw[0][0].col);
			skp->Text (cw/2, (ch*3)/2, "--OSC.EL.--", 11);
			DisplayElements (skp, shpel, cw/2, (ch*5)/2);
		}
		if (bValidTgtEl) {
			skp->SetTextColor (draw[1][0].col);
			int len = strlen (tgt->Name());
			if (len >= 11)
				strncpy (cbuf, tgt->Name(), 11);
			else {
				memset (cbuf, '-', 11);
				strncpy (cbuf+(11-len)/2, tgt->Name(), len);
			}
			skp->Text (IW-(cw*23)/2, (ch*3)/2, cbuf, 11);
			DisplayElements (skp, tgtel, IW-(cw*23)/2, (ch*5)/2);
		}
	}
	if (v4 && dispmode != DISP_LIST && bValidShpEl) DrawNodePanel ();
	ui.End ();
}

void Instrument_Orbit::DisplayElements (oapi::Sketchpad *skp, const Elements *el, int x, int y)
{
	char cbuf[16];
	int dy = ch;
	if (el) {
		if (el->e < 1.0) { // closed orbit
			sprintf (cbuf, "SMa%s", el->a < 1e5*AU ? DistStr (el->a) : " N/A");
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "SMi%s", el->SMi() < 1e5*AU ? DistStr (el->SMi()) : " N/A");
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			if (dstmode == DIST_RAD) {
				sprintf (cbuf, "PeR%s", DistStr (el->PeDist()));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
				sprintf (cbuf, "ApR%s", el->ApDist() < 1e5*AU ? DistStr (el->ApDist()) : " N/A");
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
				sprintf (cbuf, "Rad%s", DistStr (el->Radius()));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			} else {
				double r = elref->Size();
				sprintf (cbuf, "PeA%s", DistStr (el->PeDist()-r));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
				sprintf (cbuf, "ApA%s", el->ApDist()-r < 1e5*AU ? DistStr (el->ApDist()-r) : " N/A");
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
				sprintf (cbuf, "Alt%s", DistStr (el->Radius()-r));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			}
			sprintf (cbuf, "Ecc% 0.4f", el->e);
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "T  %s", FloatStr (el->OrbitT()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "PeT%s", FloatStr (el->PeT()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "ApT%s", FloatStr (el->ApT()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "Vel%s", FloatStr (el->Vel()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "Inc% 7.2fº", Deg(el->i));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "LAN% 7.2fº", Deg(el->theta));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "LPe% 7.2fº", Deg(el->omegab));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "AgP% 7.2fº", Deg(el->ArgPer()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "TrA% 7.2fº", Deg(el->TrueAnm()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "TrL% 7.2fº", Deg(el->TrueLng()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "MnA% 7.2fº", Deg(posangle(el->MeanAnm())));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "MnL% 7.2fº", Deg(el->MeanLng()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
		} else { // open orbit
			sprintf (cbuf, "SMa%s", -el->a < 1e5*AU ? DistStr (el->a) : " N/A");
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "SMi%s", -el->SMi() < 1e5*AU ? DistStr (-el->SMi()) : " N/A");
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			if (dstmode == DIST_RAD) {
				sprintf (cbuf, "PeR%s", DistStr (el->PeDist()));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
				skp->Text (x, y, "ApR N/A", 7); y += dy;
				sprintf (cbuf, "Rad%s", DistStr (el->Radius()));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			} else {
				double r = elref->Size();
				sprintf (cbuf, "PeA%s", DistStr (el->PeDist()-r));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
				skp->Text (x, y, "ApA N/A", 7); y += dy;
				sprintf (cbuf, "Alt%s", DistStr (el->Radius()-r));
				skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			}
			if      (el->e < 1e2) sprintf (cbuf, "Ecc% 0.4f", el->e);
			else if (el->e < 1e3) sprintf (cbuf, "Ecc% 0.3f", el->e);
			else if (el->e < 1e4) sprintf (cbuf, "Ecc% 0.2f", el->e);
			else if (el->e < 1e5) sprintf (cbuf, "Ecc% 0.1f", el->e);
			else {
				double e = el->e;
				int eexp = 0;
				while (e >= 10.0) e *= 0.1, eexp += 1;
				sprintf (cbuf, "Ecc% 0.3fe%d", e, eexp);
			}
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			skp->Text (x, y, "T   N/A", 7); y += dy;
			sprintf (cbuf, "PeT%s", DistStr (el->PeT()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			skp->Text (x, y, "ApT N/A", 7); y += dy;
			sprintf (cbuf, "Vel%s", DistStr (el->Vel()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "Inc% 7.2fº", Deg(el->i));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "LAN% 7.2fº", Deg(el->theta));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "LPe% 7.2fº", Deg(el->omegab));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "AgP% 7.2fº", Deg(el->ArgPer()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "TrA% 7.2fº", Deg(el->TrueAnm()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "TrL% 7.2fº", Deg(el->TrueLng()));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			sprintf (cbuf, "MnA%sº", FloatStr (DEG*el->MeanAnm(),4));
			skp->Text (x, y, cbuf, strlen(cbuf)); y += dy;
			skp->Text (x, y, "MnL N/A", 7); y += dy;
		}
	} else {
		skp->Text (x, y, "Orbital", 7); y += dy;
		skp->Text (x, y, "Elements", 8); y += dy;
		skp->Text (x, y, "not", 3); y += dy;
		skp->Text (x, y, "available", 9);
	}
}

int Instrument_Orbit::ProcessMessage (int msg, void *data)
{
	switch (msg) {
	case MSG_KILLVESSEL:
		if ((Body*)data == tgt) {
			tgt = 0; // current target object was destroyed
			if (projmode == PRJ_TGT) projmode = PRJ_FRM;
		}
		return 1;
	}
	return 0;
}

void Instrument_Orbit::SetSize (const Spec &spec)
{
	pixrad = (spec.w*4)/9;
	ICNTX = spec.w/2;
	ICNTY = spec.h/2;
}

bool Instrument_Orbit::ReadParams (ifstream &ifs)
{
	char cbuf[256], *pc;
	char cref[128] = "", ctgt[128] = "", cprj[128] = "";
	dstmode = DIST_RAD;
	if (!FindScnHeader (ifs)) return false;
	for (;;) {
		if (!ifs.getline (cbuf, 256)) return false;
		pc = trim_string (cbuf);
		if (!_strnicmp (pc, "END_MFD", 7)) break;
		if (!_strnicmp (pc, "PROJ", 4)) {
			strcpy (cprj, trim_string (pc+4));
		} else if (!_strnicmp (pc, "ALT", 3)) {
			dstmode = DIST_ALT;
		} else if (!_strnicmp (pc, "REF", 3)) {
			strcpy (cref, trim_string (pc+3));
		} else if (!_strnicmp (pc, "TARGET", 6)) {
			strcpy (ctgt, trim_string (pc+6));
		} else if (!_strnicmp (pc, "FRAME", 5)) {
			pc = trim_string (pc+5);
			if (!_stricmp (pc, "Ecliptic"))
				frmmode = FRM_ECL;
			else if (!_stricmp (pc, "Equator"))
				frmmode = FRM_EQU;
		}
	}
	if (cref[0]) SelectRef (cref);
	if (ctgt[0]) SelectTarget (ctgt);
	if (cprj[0]) {
		if (!_strnicmp (cprj, "Ship", 4))
			projmode = PRJ_SHIP;
		else if (!_strnicmp (cprj, "Target", 6) && tgt)
			projmode = PRJ_TGT;
		else if (!_strnicmp (cprj, "Ecliptic", 8) || !_strnicmp (cprj, "Frame", 5))
			projmode = PRJ_FRM;
	}
	return true;
}

void Instrument_Orbit::WriteParams (ostream &ofs) const
{
	ofs << "  TYPE Orbit" << endl;
	ofs << "  PROJ " <<
		(projmode == PRJ_FRM ? "Frame" :
		 projmode == PRJ_SHIP ? "Ship" : "Target") << endl;
	ofs << "  FRAME " <<
		(frmmode == FRM_ECL ? "Ecliptic" : "Equator") << endl;
	if (dstmode == DIST_ALT)
		ofs << "  ALT" << endl;
	if (elref) ofs << "  REF " << elref->Name() << endl;
	if (tgt) ofs << "  TARGET " << tgt->Name() << endl;
}

// =======================================================================
// Anti-aliased graphs and pointer interaction

Vector Instrument_Orbit::ToElFrame (const Vector &v) const
{
	return (frmmode == FRM_EQU && elref) ? tmul (elref->RotObliq(), v) : v;
}

Vector Instrument_Orbit::FromElFrame (const Vector &v) const
{
	return (frmmode == FRM_EQU && elref) ? mul (elref->RotObliq(), v) : v;
}

oapi::FVECTOR2 Instrument_Orbit::ToScreen (const Vector &p, const Matrix &irot) const
{
	Vector q (mul (irot, p));
	return oapi::FVECTOR2 ((float)(ICNTX + 0.5 + q.x * scale), (float)(ICNTY + 0.5 - q.z * scale));
}

void Instrument_Orbit::SampleOrbit (const Elements *el, const Matrix &irot, std::vector<oapi::FVECTOR2> &pt, std::vector<double> *ta) const
{
	const int N = 360;
	pt.clear ();
	if (ta) ta->clear ();
	Vector pos, vel;
	double e = el->e;
	if (e < 1.0) {
		double f = sqrt ((1.0 + e) / (1.0 - e));
		for (int i = 0; i < N; i++) {
			double E = Pi2 * i / N;           // eccentric anomaly: dense near periapsis
			double t = 2.0 * atan (f * tan (0.5 * E));
			el->PosVel_TA (pos, vel, t);
			pt.push_back (ToScreen (pos, irot));
			if (ta) ta->push_back (t);
		}
	} else {
		double p = el->P();
		double radmax = 1.5 * std::max (IW, IH) / scale;
		double c = (p / radmax - 1.0) / e;
		double tmax = (c > -1.0 && c < 1.0) ? acos (c) : acos (-1.0 / e) * 0.98;
		for (int i = 0; i <= N; i++) {
			double t = -tmax + 2.0 * tmax * i / N;
			el->PosVel_TA (pos, vel, t);
			pt.push_back (ToScreen (pos, irot));
			if (ta) ta->push_back (t);
		}
	}
}

double Instrument_Orbit::TimeToTA (const Elements *el, double ta) const
{
	double M = el->MeanAnomaly_from_TrueAnomaly (ta), M0 = el->MeanAnm ();
	if (el->e < 1.0) {
		double T = el->OrbitT ();
		double dt = (M - M0) / Pi2 * T;
		dt = fmod (dt, T);
		if (dt < 0.0) dt += T;
		return dt;
	}
	double n = sqrt (el->Mu () / fabs (el->a * el->a * el->a));
	return (M - M0) / n;
}

void Instrument_Orbit::DisplayOrbitF (int which, const Elements *el, const Matrix &irot, bool dashed, DWORD col)
{
	mfdui::Context &ui = UI ();
	if (!col) col = draw[which][0].col;
	std::vector<oapi::FVECTOR2> pt;
	SampleOrbit (el, irot, pt, nullptr);
	bool closed = el->e < 1.0;
	if (dashed) {
		for (size_t i = 0; i + 1 < pt.size(); i += 6)
			ui.Polyline (&pt[i], (int)std::min ((size_t)4, pt.size() - i), col, 1.5f);
	} else {
		ui.Polyline (pt.data(), (int)pt.size(), col, 1.5f, closed);
	}
	Vector pos, vel;
	el->PosVel_TA (pos, vel, 0.0); // periapsis
	oapi::FVECTOR2 pe = ToScreen (pos, irot);
	float a = std::max (3.0f, ui.Unit () * 0.22f);
	ui.Circle (pe.x, pe.y, a, col, 1.0f, true);
	if (closed) {
		el->PosVel_TA (pos, vel, Pi);
		oapi::FVECTOR2 ap = ToScreen (pos, irot);
		ui.Circle (ap.x, ap.y, a, col, 1.2f);
	}
	if (dashed) return;
	oapi::FVECTOR2 c ((float)ICNTX + 0.5f, (float)ICNTY + 0.5f);
	oapi::FVECTOR2 r = ToScreen (el->RVec(), irot);
	ui.Line (c.x, c.y, r.x, r.y, col, 1.0f);
	Vector asc, desc;
	bool ha = el->AscendingNode (asc), hd = el->DescendingNode (desc);
	if (ha && hd) {
		oapi::FVECTOR2 pa = ToScreen (asc, irot), pd = ToScreen (desc, irot);
		ui.FillRect (mfdui::Rect (pa.x - a, pa.y - a, pa.x + a, pa.y + a), draw[which][1].col);
		ui.FrameRect (mfdui::Rect (pa.x - a, pa.y - a, pa.x + a, pa.y + a), col);
		ui.FrameRect (mfdui::Rect (pd.x - a, pd.y - a, pd.x + a, pd.y + a), col);
		// line of nodes (dashed)
		float dx = pd.x - pa.x, dy = pd.y - pa.y, len = sqrtf (dx*dx + dy*dy);
		for (float s = 0.0f; s < len; s += 9.0f) {
			float s1 = std::min (len, s + 5.0f);
			ui.Line (pa.x + dx * s / len, pa.y + dy * s / len, pa.x + dx * s1 / len, pa.y + dy * s1 / len, draw[which][1].col, 1.0f);
		}
	}
}

namespace {
	void FmtTime (char *buf, size_t n, double dt)
	{
		const char *sgn = dt < 0.0 ? "-" : "";
		dt = fabs (dt);
		int d = (int)(dt / 86400.0);
		dt -= d * 86400.0;
		int h = (int)(dt / 3600.0), m = (int)((dt - h * 3600.0) / 60.0), s = (int)(dt - h * 3600.0 - m * 60.0);
		if (d) snprintf (buf, n, "%s%dd %02d:%02d:%02d", sgn, d, h, m, s);
		else snprintf (buf, n, "%s%02d:%02d:%02d", sgn, h, m, s);
	}
}

void Instrument_Orbit::DrawInteractive (const Matrix &irot)
{
	mfdui::Context &ui = UI ();
	float u = ui.Unit ();
	DWORD accent = draw[1][0].col; // yellow
	const double mu = shpel->Mu ();

	// the graph: wheel zooms, a click on the orbit adds a node
	float wheel = 0.0f;
	int f = ui.Area ("graph", mfdui::Rect (0.0f, 0.0f, (float)IW, (float)IH), 0, 0, &wheel);
	if (f & mfdui::AREA_WHEEL) {
		zoom = std::clamp (zoom * pow (1.25, (double)wheel), 1.0 / 64.0, 64.0);
		RedrawLive (0.2);
	}

	std::vector<oapi::FVECTOR2> pt;
	std::vector<double> ta;
	SampleOrbit (shpel, irot, pt, &ta);
	// (tests) points of the orbit ahead: "orbit.<percent of the period>"
	if (shpel->e < 1.0) {
		double T = shpel->OrbitT ();
		for (int pc = 5; pc < 100; pc += 5) {
			int best = -1;
			double bd = 1e30;
			for (size_t i = 0; i < ta.size(); i++) {
				double d = fabs (TimeToTA (shpel, ta[i]) - T * pc / 100.0);
				if (d < bd) bd = d, best = (int)i;
			}
			char nm[32];
			snprintf (nm, sizeof (nm), "orbit.%d", pc);
			if (best >= 0) Anchor (nm, pt[best].x, pt[best].y);
		}
	}
	auto Nearest = [&](float x, float y, float rmax) {
		int best = -1;
		float bd = rmax * rmax;
		for (size_t i = 0; i < pt.size(); i++) {
			float dx = pt[i].x - x, dy = pt[i].y - y, d = dx*dx + dy*dy;
			if (d < bd) bd = d, best = (int)i;
		}
		return best;
	};

	// manoeuvre nodes along the trajectory (two-body, about elref)
	const std::vector<maneuver::Node> &nodes = vessel->ManeuverNodes ();
	if (selNode >= (int)nodes.size()) selNode = -1;
	Vector r = FromElFrame (shpel->RVec ()), v = FromElFrame (shpel->VVec ());
	double t = td.MJD1;
	bool nodeHover = false;
	for (size_t i = 0; i < nodes.size(); i++) {
		const maneuver::Node &nd = nodes[i];
		double dt = (nd.mjd - t) * 86400.0;
		if (nd.mjd < td.MJD0) continue; // past
		Vector rn, vn;
		if (!maneuver::Kepler (r, v, mu, dt, rn, vn)) break;
		Vector dvg = maneuver::ToGlobal (nd.dv, rn, vn);
		r = rn, v = vn + dvg, t = nd.mjd;
		// the orbit after the node
		Elements post (*shpel);
		post.Calculate (ToElFrame (r), ToElFrame (v), td.SimT1 + (nd.mjd - td.MJD1) * 86400.0);
		bool sel = ((int)i == selNode);
		DisplayOrbitF (1, &post, irot, true, sel ? draw[2][0].col : accent);
		// node marker
		oapi::FVECTOR2 m = ToScreen (ToElFrame (rn), irot);
		float a = u * 0.38f;
		char id[32];
		snprintf (id, sizeof (id), "node%d", (int)i);
		float dx, dy;
		int nf = ui.Area (id, mfdui::Rect (m.x - a * 1.4f, m.y - a * 1.4f, m.x + a * 1.4f, m.y + a * 1.4f), &dx, &dy);
		if (nf & mfdui::AREA_HOVER) nodeHover = true;
		if (nf & (mfdui::AREA_CLICKED | mfdui::AREA_PRESSED)) selNode = (int)i;
		if ((nf & mfdui::AREA_DRAG) && i == 0) {
			// move the first node along the current orbit
			float px, py;
			if (ui.PointerPos (&px, &py)) {
				int k = Nearest (px, py, u * 6.0f);
				if (k >= 0) {
					maneuver::Node n2 = nd;
					n2.mjd = td.MJD1 + TimeToTA (shpel, ta[k]) / 86400.0;
					vessel->SetManeuverNode ((DWORD)i, n2);
					selNode = 0;
				}
			}
		}
		oapi::FVECTOR2 dm[4] = { { m.x, m.y - a }, { m.x + a, m.y }, { m.x, m.y + a }, { m.x - a, m.y } };
		ui.FillPolygon (dm, 4, sel ? draw[2][0].col : accent);
		ui.Polyline (dm, 4, 0x000000, 1.0f, true);
		if (nf & mfdui::AREA_HOVER) {
			char tip[64], tbuf[32];
			FmtTime (tbuf, sizeof (tbuf), (nd.mjd - td.MJD0) * 86400.0);
			snprintf (tip, sizeof (tip), "Node %d  T-%s  dv %.1f m/s", (int)i + 1, tbuf, nd.dv.length ());
			ui.ShowTooltip (tip);
		}
	}

	// the pointer on the orbit: time and altitude there; click: new node
	float px, py;
	if (!nodeHover && (f & mfdui::AREA_HOVER) && ui.PointerPos (&px, &py)) {
		int k = Nearest (px, py, u * 0.7f);
		if (k >= 0) {
			double dt = TimeToTA (shpel, ta[k]);
			Vector pos, vel;
			shpel->PosVel_TA (pos, vel, ta[k]);
			ui.Circle (pt[k].x, pt[k].y, u * 0.25f, draw[2][0].col, 1.5f);
			char tip[96], tbuf[32];
			FmtTime (tbuf, sizeof (tbuf), dt);
			snprintf (tip, sizeof (tip), "T+%s  alt %.0f km", tbuf, (pos.length () - elref->Size ()) * 1e-3);
			ui.ShowTooltip (tip);
			if (f & mfdui::AREA_CLICKED) {
				float cx, cy;
				ui.ClickPos (&cx, &cy);
				int kc = Nearest (cx, cy, u * 0.9f);
				if (kc >= 0 && dt > 0.0) {
					maneuver::Node nd;
					nd.mjd = td.MJD1 + TimeToTA (shpel, ta[kc]) / 86400.0;
					nd.dv = Vector ();
					selNode = (int)vessel->AddManeuverNode (nd);
				}
			}
		} else if (f & mfdui::AREA_CLICKED) {
			selNode = -1;
		}
	}
}

void Instrument_Orbit::DrawNodePanel ()
{
	const std::vector<maneuver::Node> &nodes = vessel->ManeuverNodes ();
	if (selNode < 0 || selNode >= (int)nodes.size()) return;
	mfdui::Context &ui = UI ();
	float u = ui.Unit ();
	maneuver::Node nd = nodes[selNode];

	// three rows above the bottom line: node, controls, velocity change
	mfdui::Rect panel (u * 0.3f, IH - u * 5.0f, IW - u * 0.3f, IH - u * 1.5f);
	ui.Area ("nodepanel", panel); // (takes the clicks the widgets don't)
	ui.FillRect (panel, 0x061006);
	ui.FrameRect (panel, draw[1][0].col);
	mfdui::Rect in = panel.Inset (u * 0.2f);
	char buf[96], tbuf[32];
	FmtTime (tbuf, sizeof (tbuf), (nd.mjd - td.MJD0) * 86400.0);
	snprintf (buf, sizeof (buf), "Node %d   T-%s", selNode + 1, tbuf);
	mfdui::Rect r0 = in.Row (0, 3, u * 0.1f), r1 = in.Row (1, 3, u * 0.1f), r2 = in.Row (2, 3, u * 0.1f);
	ui.TextIn (r0, buf, draw[1][0].col, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE);
	snprintf (buf, sizeof (buf), "dv %.1f m/s", nd.dv.length ());
	ui.TextIn (r0, buf, draw[1][0].col, mfdui::ALIGN_RIGHT | mfdui::ALIGN_MIDDLE);

	bool changed = false;
	bool ab = nd.autoburn;
	if (ui.Toggle ("autoburn", r1.Left (r1.W() * 0.5f), "Autoburn", &ab)) nd.autoburn = ab, changed = true;
	ui.Tooltip ("Fly this burn with the burn autopilot");
	if (ui.Button ("delete", r1.Right (u * 4.0f), "Delete")) {
		vessel->DelManeuverNode ((DWORD)selNode);
		selNode = -1;
		return;
	}
	if (ui.Button ("close", r1.CutRight (u * 4.3f).Right (u * 3.0f), "Close")) {
		selNode = -1;
		return;
	}

	static const char *lbl[3] = { "Pro", "Nml", "Rad" };
	static const char *ids[3] = { "Prograde", "Normal", "Radial" };
	double comp[3] = { nd.dv.x, nd.dv.y, nd.dv.z };
	mfdui::NumberSpec dvs (1.0, -1e5, 1e5, "%.1f");
	for (int k = 0; k < 3; k++) {
		mfdui::Rect c = r2.Col (k, 3, u * 0.25f);
		float lw = ui.TextWidth ("Nml", mfdui::FONT_SMALL) + u * 0.7f;
		ui.TextIn (c.Left (lw), lbl[k], draw[0][1].col, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		if (ui.Number (ids[k], c.CutLeft (lw), &comp[k], dvs)) changed = true;
		ui.Tooltip ("m/s. Wheel or drag: 1 m/s steps; click: type a value");
	}
	if (changed) {
		nd.dv = Vector (comp[0], comp[1], comp[2]);
		vessel->SetManeuverNode ((DWORD)selNode, nd);
	}
}
