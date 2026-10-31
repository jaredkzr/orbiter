// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MfdPlanner.h"
#include "Transfer.h"
#include "MFDUI.h"
#include "Orbiter.h"
#include "Pane.h"
#include "Celbody.h"
#include "Psys.h"
#include "Astro.h"
#include "Util.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace std;
using mfdui::Rect;

extern Orbiter *g_pOrbiter;
extern PlanetarySystem *g_psys;
extern TimeData td;

struct Layout {
	Rect area;   // the page
	float u;     // layout unit
};

Instrument_Planner::SavePrm Instrument_Planner::saveprm;

namespace {
	void FmtDate (char *buf, size_t n, double mjd)
	{
		struct tm t = *mjddate (mjd);
		snprintf (buf, n, "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, std::clamp (t.tm_mon, 1, 12), t.tm_mday, t.tm_hour, t.tm_min);
	}

	void FmtSpan (char *buf, size_t n, double sec)
	{
		const char *sg = sec < 0.0 ? "-" : "";
		sec = fabs (sec);
		int d = (int)(sec / 86400.0);
		sec -= d * 86400.0;
		int h = (int)(sec / 3600.0), m = (int)((sec - h * 3600.0) / 60.0), s = (int)(sec - h * 3600.0 - m * 60.0);
		if (d) snprintf (buf, n, "%s%dd %02d:%02d", sg, d, h, m);
		else snprintf (buf, n, "%s%02d:%02d:%02d", sg, h, m, s);
	}

	const char *RoleName (int role)
	{
		switch (role) {
		case transfer::ROLE_EJECTION: return "Ejection";
		case transfer::ROLE_MCC: return "Correction";
		case transfer::ROLE_CAPTURE: return "Capture";
		case transfer::ROLE_FLYBY: return "Flyby burn";
		case transfer::ROLE_DSM: return "Manoeuvre";
		}
		return "-";
	}

	// two text columns: label (dim) and value
	void Row (mfdui::Context &ui, const Rect &r, const char *label, const char *value, DWORD col, float split = 0.38f)
	{
		ui.TextIn (r.Left (r.W() * split), label, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		ui.TextIn (r.CutLeft (r.W() * split), value, col, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
}

// =======================================================================

Instrument_Planner::Instrument_Planner (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel)
: Instrument (_pane, _id, spec, _vessel)
{
	page = (saveprm.usr == _vessel) ? saveprm.page : PG_SETUP;
	if (transfer::Planner *pl = Planner ()) {
		if (!pl->settings.target) { // (a default target: the first moon, or the first other planet)
			auto t = pl->Targets ();
			for (const CelestialBody *c : t) if (pl->KindOf (c) == transfer::KIND_MOON) { pl->settings.target = c; break; }
			if (!pl->settings.target && !t.empty()) pl->settings.target = t.front();
		}
		lastSerial = pl->GetStatus ().serial;
	}
}

Instrument_Planner::~Instrument_Planner ()
{
	saveprm.usr = vessel;
	saveprm.page = page;
}

transfer::Planner *Instrument_Planner::Planner () const
{
	return g_pOrbiter->TransferPlanner (vessel);
}

bool Instrument_Planner::Update (double upDTscale)
{
	// redraw when the planner has news, and while it computes
	if (transfer::Planner *pl = Planner ()) {
		transfer::Status st = pl->GetStatus ();
		if (st.serial != lastSerial) {
			// switch to the page of the result
			bool routes = pl->GetRoute ().valid;
			if (lastJob == transfer::Status::SEARCH && page == PG_SETUP) page = routes ? PG_ROUTE : PG_CHART;
			if (lastJob == transfer::Status::SEARCH && routes && page == PG_CHART) page = PG_ROUTE;
			if (lastJob == transfer::Status::SUGGEST) page = PG_ROUTE, routeView = 1;
			if (lastJob == transfer::Status::REFINE && (page == PG_CHART || page == PG_BURN || page == PG_ROUTE) && pl->GetPlan ().valid) page = PG_CRUISE;
			lastSerial = st.serial;
			Refresh ();
		}
		if (st.job != transfer::Status::IDLE) RedrawLive (0.25);
		lastJob = st.job;
	}
	return Instrument::Update (upDTscale);
}

void Instrument_Planner::UpdateDraw (oapi::Sketchpad *skp)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	ui.Begin (skp, (float)IW, (float)IH);
	float u = ui.Unit ();

	// title and the planner's state
	const CelestialBody *ref = vessel->ElRef ();
	const CelestialBody *tgt = pl ? pl->settings.target : nullptr;
	char buf[160];
	snprintf (buf, sizeof (buf), "Transfer: %s > %s", ref ? ref->Name() : "-", tgt ? tgt->Name() : "?");
	ui.TextIn (Rect (0.0f, 0.0f, IW * 0.68f, u), buf, col_grey1, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE);
	transfer::Status st = pl ? pl->GetStatus () : transfer::Status ();
	if (st.job != transfer::Status::IDLE) {
		static const char *jn[5] = { "", "Searching", "Targeting", "Correcting", "Suggesting" };
		snprintf (buf, sizeof (buf), "%s %d%%", jn[st.job], (int)(st.progress * 100.0f + 0.5f));
		Rect pr (IW * 0.68f, u * 0.2f, IW - u * 0.3f, u * 0.8f);
		ui.FillRect (pr, ui.Colour (mfdui::FILL));
		ui.FillRect (Rect (pr.x0, pr.y0, pr.x0 + pr.W() * st.progress, pr.y1), ui.Colour (mfdui::ACTIVE));
		ui.TextIn (pr, buf, ui.Colour (mfdui::ACCENT), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}

	// the tabs (gravity assists: Route in place of the porkchop chart)
	static const char *names[NPAGE] = { "Setup", "Chart", "Dates", "Launch", "Burn", "Cruise", "Budget", "Route" };
	bool adv = pl && pl->settings.assists;
	int vis[NPAGE], nv = 0;
	for (int p : { (int)PG_SETUP, adv ? (int)PG_ROUTE : (int)PG_CHART, (int)PG_WINDOWS, (int)PG_LAUNCH, (int)PG_BURN, (int)PG_CRUISE, (int)PG_BUDGET }) vis[nv++] = p;
	if (adv && page == PG_CHART) page = PG_ROUTE;
	if (!adv && page == PG_ROUTE) page = PG_CHART;
	const char *tabs[NPAGE];
	int tsel = 0;
	for (int k = 0; k < nv; k++) { tabs[k] = names[vis[k]]; if (vis[k] == page) tsel = k; }
	if (ui.Tabs ("page", Rect (u * 0.15f, u * 1.05f, IW - u * 0.15f, u * 2.05f), tabs, nv, &tsel)) page = vis[tsel];

	Layout L{ Rect (u * 0.3f, u * 2.3f, IW - u * 0.3f, IH - u * 1.05f), u };
	if (pl) {
		switch (page) {
		case PG_SETUP:   DrawSetup (L); break;
		case PG_CHART:   DrawChart (L); break;
		case PG_WINDOWS: DrawWindows (L); break;
		case PG_BURN:    DrawBurn (L); break;
		case PG_CRUISE:  DrawCruise (L); break;
		case PG_BUDGET:  DrawBudget (L); break;
		case PG_LAUNCH:  DrawLaunch (L); break;
		case PG_ROUTE:   DrawRoute (L); break;
		}
		ui.TextIn (Rect (u * 0.2f, IH - u * 1.0f, IW - u * 0.2f, IH - u * 0.05f), st.message.c_str(),
			ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
	ui.End ();
}

// -----------------------------------------------------------------------
// Setup: target, objective, arrival orbit, corrections

void Instrument_Planner::DrawSetup (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u, rh = u * 1.12f, gap = u * 0.12f;
	transfer::Settings &s = pl->settings;
	Rect a = L.area;
	auto R = [&](int i) { return Rect (a.x0, a.y0 + i * rh, a.x1, a.y0 + i * rh + rh - gap); };
	float lw = a.W() * 0.36f;
	auto Label = [&](const Rect &r, const char *t) {
		ui.TextIn (r.Left (lw), t, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	};

	// target
	auto targets = pl->Targets ();
	std::vector<const char*> names;
	int sel = -1;
	for (size_t i = 0; i < targets.size(); i++) {
		names.push_back (targets[i]->Name());
		if (targets[i] == s.target) sel = (int)i;
	}
	Label (R(0), "Target");
	if (!targets.empty() && ui.Choice ("target", R(0).CutLeft (lw), names.data(), (int)names.size(), &sel)) {
		s.target = targets[sel];
		s.capAlt = 0.0;
	}
	ui.Tooltip ("Planets with the same parent, or moons of the body the vessel orbits");

	static const char *obj[2] = { "Least total dv", "Least ejection dv" };
	Label (R(1), "Objective");
	ui.Choice ("objective", R(1).CutLeft (lw), obj, 2, &s.objective);
	ui.Tooltip ("Total: ejection + capture. Ejection: for flybys and aerocapture");

	// arrival orbit
	double alt = (s.capAlt > 0.0 ? s.capAlt : transfer::Planner::DefaultCaptureAlt (s.target));
	double apo = std::max (s.capApo, alt);
	Label (R(2), "Periapsis");
	Rect rv = R(2).CutLeft (lw);
	if (ui.Number ("Periapsis", rv.Col (0, 2, gap), &alt, mfdui::NumberSpec (10e3, 0.0, 1e9, "%.0f", "km", 1e-3))) s.capAlt = alt;
	ui.Tooltip ("Periapsis altitude at the target");
	if (ui.Number ("Apoapsis", rv.Col (1, 2, gap), &apo, mfdui::NumberSpec (10e3, 0.0, 1e10, "%.0f", "km", 1e-3))) s.capApo = apo, s.capAlt = alt;
	ui.Tooltip ("Apoapsis altitude of the capture orbit (= periapsis: circular)");

	static const char *arr[2] = { "Equatorial", "As it arrives" };
	Label (R(3), "Arrival plane");
	ui.Choice ("arrival", R(3).CutLeft (lw), arr, 2, &s.arrival);

	// corrections
	for (int m = 0; m < 2; m++) {
		Rect r = R(4 + m);
		char lb[32], id[16];
		snprintf (lb, sizeof (lb), "Correction %d", m + 1);
		snprintf (id, sizeof (id), "mcc%d", m + 1);
		ui.Toggle (id, r.Left (lw + u * 0.8f), lb, &s.useMcc[m]);
		double pc = s.mcc[m] * 100.0;
		snprintf (id, sizeof (id), "mccat%d", m + 1);
		if (ui.Number (id, r.CutLeft (lw + u).Left (u * 4.5f), &pc, mfdui::NumberSpec (5.0, 1.0, 99.0, "%.0f", "%"))) s.mcc[m] = pc / 100.0;
		ui.Tooltip ("Time of the correction, as a part of the flight");
	}
	ui.Toggle ("autocorrect", R(6), "Compute corrections when due", &s.autoCorrect);

	// gravity assists (the advanced mode: the Route page)
	ui.Toggle ("assists", R(7).Left (a.W() * 0.6f), "Advanced: gravity assists", &s.assists);
	ui.Tooltip ("Routes by flybys of other bodies: the Route page takes the place of the chart");
	transfer::Kind k = pl->KindOf (s.target);
	if (s.assists) {
		std::string seq;
		const CelestialBody *ref = vessel->ElRef ();
		seq = ref ? ref->Name () : "-";
		for (const CelestialBody *f : s.flybys) { seq += " > "; seq += f->Name (); }
		seq += " > ";
		seq += s.target ? s.target->Name () : "?";
		if (s.flybys.empty ()) seq += "  (direct: add flybys on Route)";
		ui.TextIn (R(8), seq.c_str (), ui.Colour (mfdui::INFO), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}

	// search
	int rs = s.assists ? 9 : 8;
	Rect b = R(rs).Left (a.W() * 0.5f);
	if (ui.Button ("search", b, "Search", false, k != transfer::KIND_NONE)) Action (2);
	ui.Tooltip (s.UsesRoute () ? "The best route by these flybys, and the best by departure date" : "Launch windows and the porkchop chart of the first good one");
	const char *kind = k == transfer::KIND_SIBLING ? "Ejection, then a conic about the parent" :
		k == transfer::KIND_MOON ? "From the parking orbit to the moon" : "No transfer to this target";
	if (s.UsesRoute ()) kind = "Route with flybys";
	ui.TextIn (R(rs).CutLeft (a.W() * 0.52f), kind, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	if (const CelestialBody *ref = vessel->ElRef ()) {
		const Elements *el = vessel->Els ();
		char buf[128];
		if (vessel->GetStatus () == FLIGHTSTATUS_LANDED || vessel->GroundContact ())
			snprintf (buf, sizeof (buf), "On the ground on %s: see Launch after the search", ref->Name());
		else if (el && el->e < 1.0)
			snprintf (buf, sizeof (buf), "Parking orbit: %.0f x %.0f km about %s", (el->PeDist() - ref->Size()) * 1e-3,
				(el->ApDist() - ref->Size()) * 1e-3, ref->Name());
		else
			snprintf (buf, sizeof (buf), "Not in a closed orbit about %s", ref->Name());
		ui.TextIn (R(rs + 1), buf, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
}

// -----------------------------------------------------------------------
// Chart: the porkchop plot

void Instrument_Planner::DrawChart (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u;
	auto g = pl->Porkchop ();
	if (!g) {
		ui.TextIn (L.area, "No chart yet: Setup > Search", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE);
		return;
	}
	transfer::Solution best = pl->Best (), sel = pl->Selected ();
	Rect plot = L.area.CutBottom (u * 2.4f);
	mfdui::PlotSpec spec (g->dep0, g->dep1, g->tof0, g->tof1);
	spec.xfmt = mfdui::AXIS_DATE, spec.yfmt = mfdui::AXIS_DAYS;
	spec.xlabel = "Departure", spec.ylabel = "Flight time";
	float zlo = g->zmin, zhi = g->zmin * 1.6f;
	hover = false;
	if (ui.BeginPlot ("porkchop", plot, spec)) {
		ui.PlotHeatmap (g->total.data(), g->nx, g->ny, g->dep0, g->dep1, g->tof0, g->tof1, zlo, zhi);
		float lev[6] = { zlo * 1.02f, zlo * 1.05f, zlo * 1.1f, zlo * 1.2f, zlo * 1.35f, zlo * 1.5f };
		ui.PlotContours (g->total.data(), g->nx, g->ny, g->dep0, g->dep1, g->tof0, g->tof1, lev, 6, 0x90FFFFFF, 1.0f);
		if (best.valid) ui.PlotMarker (best.dep, best.tof, ui.Colour (mfdui::BRIGHT), mfdui::MARK_CROSS, "Optimum");
		if (sel.valid && (sel.dep != best.dep || sel.tof != best.tof)) ui.PlotMarker (sel.dep, sel.tof, ui.Colour (mfdui::ACCENT), mfdui::MARK_CIRCLE);
		double x, y;
		if (ui.PlotHover (&x, &y)) {
			hover = true, hoverX = x, hoverY = y;
			float sx, sy;
			ui.PlotToScreen (x, y, &sx, &sy);
			double x0, x1, y0, y1;
			ui.PlotView (&x0, &x1, &y0, &y1);
			float ax, ay, bx, by;
			ui.PlotToScreen (x0, y0, &ax, &ay);
			ui.PlotToScreen (x1, y1, &bx, &by);
			ui.Line (sx, ay, sx, by, 0x60FFFFFF, 1.0f);
			ui.Line (ax, sy, bx, sy, 0x60FFFFFF, 1.0f);
		}
		if (ui.PlotClicked (&x, &y)) pl->Select (x, y);
		ui.EndPlot ();
	}

	// readout: the hovered point (from the grid) or the selected transfer
	Rect rrow = L.area.Bottom (u * 2.3f).Top (u * 1.05f);
	Rect ro = rrow.CutRight (u * 6.2f);
	char buf[160], d0[32];
	if (hover) {
		double fx = (hoverX - g->dep0) / (g->dep1 - g->dep0) * (g->nx - 1), fy = (hoverY - g->tof0) / (g->tof1 - g->tof0) * (g->ny - 1);
		int i = std::clamp ((int)floor (fx), 0, g->nx - 2), j = std::clamp ((int)floor (fy), 0, g->ny - 2);
		double tx = fx - i, ty = fy - j;
		auto Z = [&](const std::vector<float> &z) {
			return (1-tx)*(1-ty)*z[j*g->nx+i] + tx*(1-ty)*z[j*g->nx+i+1] + (1-tx)*ty*z[(j+1)*g->nx+i] + tx*ty*z[(j+1)*g->nx+i+1];
		};
		FmtDate (d0, sizeof (d0), hoverX);
		snprintf (buf, sizeof (buf), "%.10s %.0f d %.0f m/s", d0, hoverY, Z (g->total));
		ui.TextIn (ro, buf, ui.Colour (mfdui::BRIGHT), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	} else if (sel.valid) {
		FmtDate (d0, sizeof (d0), sel.dep);
		snprintf (buf, sizeof (buf), "%.10s %.0f d %.0f m/s", d0, sel.tof, sel.dvTotal);
		ui.TextIn (ro, buf, ui.Colour (mfdui::ACCENT), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
	// colour scale
	Rect leg = rrow.Right (u * 4.2f).Inset (0.0f, u * 0.25f);
	DWORD ramp[32];
	for (int i = 0; i < 32; i++) ramp[i] = mfdui::Context::MapColour (i / 31.0f);
	ui.Image (ramp, 32, 1, leg, true);
	snprintf (buf, sizeof (buf), "%.1f", zlo * 1e-3);
	ui.Text (leg.x0 - u * 0.15f, leg.CY(), buf, ui.Colour (mfdui::DIM), mfdui::ALIGN_RIGHT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);

	Rect br = L.area.Bottom (u * 1.15f);
	if (ui.Button ("optimum", br.Col (0, 3, u * 0.2f), "Optimum")) pl->Select (-1.0, 0.0);
	if (ui.Button ("nodes", br.Col (1, 3, u * 0.2f), "Make nodes", false, sel.valid)) Action (3);
	ui.Tooltip ("Manoeuvre nodes of the selected transfer, targeted with the numerical prediction");
	if (ui.Button ("nodesarm", br.Col (2, 3, u * 0.2f), "Make & arm", false, sel.valid)) Action (6);
	ui.Tooltip ("... and fly them with the burn autopilot");
}

// -----------------------------------------------------------------------
// Windows: least velocity change by departure date

void Instrument_Planner::DrawWindows (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u;
	auto c = pl->Windows ();
	if (!c || c->dv.empty()) {
		ui.TextIn (L.area, "No windows yet: Setup > Search", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE);
		return;
	}
	size_t n = c->dv.size();
	std::vector<double> x (n), y (n);
	double ymin = 1e30, ymax = 0.0;
	for (size_t i = 0; i < n; i++) {
		x[i] = c->dep0 + (c->dep1 - c->dep0) * i / (n - 1);
		y[i] = std::isfinite (c->dv[i]) ? c->dv[i] * 1e-3 : std::numeric_limits<double>::quiet_NaN();
		if (std::isfinite (y[i])) ymin = std::min (ymin, y[i]), ymax = std::max (ymax, y[i]);
	}
	if (ymin > ymax) return;
	ymax = std::min (ymax, ymin * 3.0);
	mfdui::PlotSpec spec (c->dep0, c->dep1, std::max (0.0, ymin - 0.1 * (ymax - ymin)), ymax);
	spec.xfmt = mfdui::AXIS_DATE;
	spec.xlabel = "Departure", spec.ylabel = pl->GetRoute ().valid ? "Route dv [km/s]" : "Least dv [km/s]";
	Rect plot = L.area.CutBottom (u * 1.2f);
	hover = false;
	if (ui.BeginPlot ("windows", plot, spec)) {
		ui.PlotLine (x.data(), y.data(), (int)n, ui.Colour (mfdui::TEXT), 1.5f);
		for (size_t k = 0; k < c->windows.size() && k < 6; k++) {
			double w = c->windows[k];
			size_t i = (size_t)std::clamp ((w - c->dep0) / (c->dep1 - c->dep0) * (n - 1) + 0.5, 0.0, (double)(n - 1));
			if (!std::isfinite (y[i])) continue;
			char lb[16];
			struct tm t = *mjddate (w);
			snprintf (lb, sizeof (lb), "%02d/%02d", std::clamp (t.tm_mon, 1, 12), (t.tm_year + 1900) % 100);
			ui.PlotMarker (w, y[i], k == 0 ? ui.Colour (mfdui::BRIGHT) : ui.Colour (mfdui::ACCENT), mfdui::MARK_TRIANGLE, lb);
		}
		double hx, hy;
		if (ui.PlotHover (&hx, &hy)) hover = true, hoverX = hx;
		double cx, cy;
		if (ui.PlotClicked (&cx, &cy)) {
			if (pl->GetRoute ().valid) pl->Select (cx, 0.0), page = PG_ROUTE; // (the route departing then)
			else pl->SearchAround (cx), page = PG_CHART;
		}
		ui.EndPlot ();
	}
	Rect ro = L.area.Bottom (u * 1.1f);
	char buf[160], d0[32];
	if (hover) {
		size_t i = (size_t)std::clamp ((hoverX - c->dep0) / (c->dep1 - c->dep0) * (n - 1) + 0.5, 0.0, (double)(n - 1));
		FmtDate (d0, sizeof (d0), hoverX);
		snprintf (buf, sizeof (buf), "%.10s  %.0f m/s  %.0f d  (click: %s)", d0, c->dv[i], c->tof[i], pl->GetRoute ().valid ? "choose" : "chart");
		ui.TextIn (ro, buf, ui.Colour (mfdui::BRIGHT), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	} else if (!c->windows.empty()) {
		FmtDate (d0, sizeof (d0), c->windows.front());
		snprintf (buf, sizeof (buf), "Best window: %.10s", d0);
		ui.TextIn (ro, buf, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
}

// -----------------------------------------------------------------------
// Burn: the selected transfer and its ejection

void Instrument_Planner::DrawBurn (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u;
	transfer::Solution s = pl->Selected ();
	if (!s.valid) {
		ui.TextIn (L.area, "No transfer selected", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE);
		return;
	}
	bool moon = pl->CurrentKind () == transfer::KIND_MOON;
	// on the ground: the ejection from the parking orbit of the chosen launch
	transfer::LaunchPlan lp = pl->Launch ();
	bool landed = lp.landed;
	const CelestialBody *ref = vessel->ElRef ();
	Vector orbR, orbV;
	double orbT = td.MJD1;
	if (ref) orbR = vessel->GPos() - ref->GPos(), orbV = vessel->GVel() - ref->GVel();
	if (landed && lp.sel >= 0) {
		const transfer::LaunchOpportunity &o = lp.opp[lp.sel];
		s.burnValid = true;
		s.burnMjd = o.burnMjd, s.burnDv = o.burnDv, s.burnDvMag = o.dvEject, s.dvCapture = o.dvCapture;
		s.tof = o.tof, s.dep = o.burnMjd;
		orbR = o.rIns, orbV = o.vIns, orbT = o.insMjd;
	} else if (landed) {
		s.burnValid = false;
	}
	Rect a = L.area;
	Rect text = a.Left (a.W() * 0.56f), diag = a.CutLeft (a.W() * 0.58f).CutBottom (u * 1.25f);
	float rh = u * 0.82f;
	auto R = [&](int i) { return Rect (text.x0, text.y0 + i * rh, text.x1, text.y0 + (i + 1) * rh); };
	DWORD tc = ui.Colour (mfdui::TEXT);
	char buf[96], d0[32], d1[32];
	int i = 0;
	FmtDate (d0, sizeof (d0), s.burnValid ? s.burnMjd : s.dep);
	FmtSpan (d1, sizeof (d1), ((s.burnValid ? s.burnMjd : s.dep) - td.MJD0) * 86400.0);
	Row (ui, R(i++), "Burn", d0, ui.Colour (mfdui::ACCENT));
	snprintf (buf, sizeof (buf), "T-%s", d1);
	Row (ui, R(i++), "", buf, ui.Colour (mfdui::ACCENT));
	FmtDate (d0, sizeof (d0), s.dep + s.tof);
	Row (ui, R(i++), "Arrival", d0, tc);
	snprintf (buf, sizeof (buf), "%.1f d", s.tof);
	Row (ui, R(i++), "Flight", buf, tc);
	if (!moon) {
		snprintf (buf, sizeof (buf), "%.2f km2/s2", s.c3 * 1e-6);
		Row (ui, R(i++), "C3", buf, tc);
		snprintf (buf, sizeof (buf), "%.0f m/s, %.1f deg", s.vinfDep, s.decl * DEG);
		Row (ui, R(i++), "v-inf dep", buf, tc);
	}
	double ej = s.burnValid ? s.burnDvMag : s.dvEject;
	double bt = vessel->BurnDuration (ej);
	if (bt >= 0.0) { char t[32]; FmtSpan (t, sizeof (t), bt); snprintf (buf, sizeof (buf), "%.0f m/s, %s", ej, t + 3); }
	else snprintf (buf, sizeof (buf), "%.0f m/s", ej);
	Row (ui, R(i++), moon ? "Injection" : "Ejection", buf, ui.Colour (mfdui::BRIGHT));
	if (s.burnValid) {
		snprintf (buf, sizeof (buf), "%+.0f %+.0f %+.0f", s.burnDv.x, s.burnDv.y, s.burnDv.z);
		Row (ui, R(i++), "P / N / R", buf, tc);
	}
	snprintf (buf, sizeof (buf), "%.0f m/s", s.dvCapture);
	Row (ui, R(i++), "Capture", buf, tc);
	snprintf (buf, sizeof (buf), "%.0f m/s", s.vinfArr);
	Row (ui, R(i++), "v-inf arr", buf, tc);
	snprintf (buf, sizeof (buf), "%.0f m/s", ej + (pl->settings.objective == transfer::OBJ_TOTAL ? s.dvCapture : 0.0));
	Row (ui, R(i++), "Total", buf, ui.Colour (mfdui::BRIGHT));
	if (!moon && s.burnValid && s.burnDvMag > 1.05 * s.dvEject)
		ui.TextIn (R(i++), "Plane change: orbit not aligned", ui.Colour (mfdui::ALERT), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);

	// the parking orbit seen from above its plane: the burn, its direction,
	// the departure asymptote and the direction to the parent
	if (ref && s.burnValid && diag.W() > u * 3.0f) {
		Vector r0 (orbR), v0 (orbV);
		Vector h (crossp (r0, v0).unit());
		Vector X ((moon ? s.vinfDepVec : s.vinfDepVec) - h * dotp (s.vinfDepVec, h));
		if (X.length() < 1e-9) X = r0;
		X.unify();
		Vector Y (crossp (h, X).unit());
		float cx = diag.CX(), cy = diag.CY(), rad = std::min (diag.W(), diag.H()) * 0.36f;
		double scale = rad / r0.length();
		auto P = [&](const Vector &p) { return oapi::FVECTOR2 ((float)(cx + dotp (p, X) * scale), (float)(cy - dotp (p, Y) * scale)); };
		ui.Circle (cx, cy, (float)(ref->Size() * scale), ui.Colour (mfdui::FILL), 1.0f, true);
		ui.Circle (cx, cy, (float)(ref->Size() * scale), ui.Colour (mfdui::DIM), 1.0f);
		std::vector<oapi::FVECTOR2> orb;
		Vector rr, vv;
		double mu = Ggrav * ref->Mass ();
		double en = 0.5 * dotp (v0, v0) - mu / r0.length();
		double T = (en < 0.0) ? Pi2 * sqrt (pow (-mu / (2.0 * en), 3) / mu) : 6000.0;
		for (int k = 0; k <= 96; k++)
			if (maneuver::Kepler (r0, v0, mu, T * k / 96.0, rr, vv)) orb.push_back (P (rr));
		ui.Polyline (orb.data(), (int)orb.size(), ui.Colour (mfdui::TEXT), 1.2f);
		oapi::FVECTOR2 pv = P (r0);
		ui.Circle (pv.x, pv.y, u * 0.16f, ui.Colour (mfdui::TEXT), 1.0f, true);
		// burn point and direction
		if (maneuver::Kepler (r0, v0, mu, (s.burnMjd - orbT) * 86400.0, rr, vv)) {
			oapi::FVECTOR2 pb = P (rr);
			Vector dvg (maneuver::ToGlobal (s.burnDv, rr, vv).unit());
			float al = u * 1.6f;
			oapi::FVECTOR2 tip ((float)(pb.x + dotp (dvg, X) * al), (float)(pb.y - dotp (dvg, Y) * al));
			ui.Line (pb.x, pb.y, tip.x, tip.y, ui.Colour (mfdui::ACCENT), 2.0f);
			oapi::FVECTOR2 dm[4] = { { pb.x, pb.y - u*0.25f }, { pb.x + u*0.25f, pb.y }, { pb.x, pb.y + u*0.25f }, { pb.x - u*0.25f, pb.y } };
			ui.FillPolygon (dm, 4, ui.Colour (mfdui::ACCENT));
		}
		// asymptote / transfer direction
		{
			oapi::FVECTOR2 e (cx + rad * 1.25f, cy);
			for (float t = 0.15f; t < 1.0f; t += 0.12f)
				ui.Line (cx + (e.x - cx) * t, cy, cx + (e.x - cx) * std::min (1.0f, t + 0.07f), cy, ui.Colour (mfdui::BRIGHT), 1.0f);
			ui.Text (e.x, cy - u * 0.1f, moon ? "dv" : "v-inf", ui.Colour (mfdui::BRIGHT), mfdui::ALIGN_RIGHT | mfdui::ALIGN_BOTTOM, mfdui::FONT_SMALL);
		}
		if (const CelestialBody *par = ref->Primary ()) {
			Vector sd ((par->GPos() - ref->GPos()).unit());
			Vector sp (sd - h * dotp (sd, h));
			if (sp.length() > 1e-6) {
				sp.unify();
				float ex = cx + (float)dotp (sp, X) * rad * 1.3f, ey = cy - (float)dotp (sp, Y) * rad * 1.3f;
				ui.Line (cx, cy, ex, ey, ui.Colour (mfdui::INFO), 1.0f);
				ui.Text (ex, ey, par->Name(), ui.Colour (mfdui::INFO), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
			}
		}
	}

	transfer::Plan plan = pl->GetPlan ();
	Rect br = a.Bottom (u * 1.15f);
	if (ui.Button ("nodes", br.Col (0, 3, u * 0.2f), "Make nodes", false, !landed)) Action (3);
	ui.Tooltip (landed ? "On the ground: make the nodes once in the parking orbit" : "Manoeuvre nodes of this transfer");
	if (ui.Button ("nodesarm", br.Col (1, 3, u * 0.2f), "Make & arm", false, !landed)) Action (6);
	bool armed = plan.armed;
	if (plan.valid && ui.Toggle ("armed", br.Col (2, 3, u * 0.2f), "Armed", &armed)) pl->Arm (armed);
}

// -----------------------------------------------------------------------
// Launch: from the ground into a parking orbit that contains the transfer's
// departure direction

void Instrument_Planner::DrawLaunch (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u;
	Rect a = L.area;
	transfer::LaunchPlan lp = pl->Launch ();
	if (!lp.landed) {
		ui.TextIn (a.Top (a.H() * 0.5f), "In flight: no launch to plan", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_BOTTOM);
		ui.TextIn (a.CutTop (a.H() * 0.5f).Top (u), "(from the ground: Setup > Search)", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		return;
	}
	const CelestialBody *ref = pl->Departure ();
	char buf[128], d0[32], t[32];
	snprintf (buf, sizeof (buf), "Site %.2f%c %.2f%c on %s", fabs (lp.lat) * DEG, lp.lat >= 0.0 ? 'N' : 'S',
		fabs (lp.lng) * DEG, lp.lng >= 0.0 ? 'E' : 'W', ref ? ref->Name() : "-");
	ui.TextIn (a.Top (u * 0.8f), buf, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);

	// parking orbit, ascent, least coast
	transfer::Settings &st = pl->settings;
	Rect sl = a.CutTop (u * 0.75f).Top (u * 0.6f);
	Rect sr = a.CutTop (u * 1.35f).Top (u * 1.0f);
	float gap = u * 0.15f;
	static const char *sln[3] = { "Parking orbit", "Ascent", "Least coast" };
	for (int k = 0; k < 3; k++)
		ui.TextIn (sl.Col (k, 3, gap), sln[k], ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	double alt = lp.parkAlt, asc = st.ascentTime / 60.0, coast = st.minCoast / 60.0;
	bool ch = false;
	Rect c0 = sr.Col (0, 3, gap), c1 = sr.Col (1, 3, gap), c2 = sr.Col (2, 3, gap);
	if (ui.Number ("Parking", c0, &alt, mfdui::NumberSpec (10e3, 50e3, 5e6, "%.0f", "km", 1e-3))) st.parkAlt = alt, ch = true;
	ui.Tooltip ("Parking orbit altitude");
	if (ui.Number ("Ascent", c1, &asc, mfdui::NumberSpec (1.0, 2.0, 60.0, "%.0f", "min"))) st.ascentTime = asc * 60.0, ch = true;
	ui.Tooltip ("Lift-off to orbit insertion");
	if (ui.Number ("Coast", c2, &coast, mfdui::NumberSpec (5.0, 0.0, 1440.0, "%.0f", "min"))) st.minCoast = coast * 60.0, ch = true;
	ui.Tooltip ("Least coast in the parking orbit before the ejection");
	if (ch) pl->UpdateLaunch ();

	// the opportunities
	float lh = u * 1.1f * std::min<size_t> (std::max<size_t> (lp.opp.size(), 1), 4) + 2.0f;
	Rect lr = a.CutTop (u * 2.5f).Top (lh);
	if (lp.opp.empty()) {
		ui.TextIn (lr, "No launch before the departure: choose a later one", ui.Colour (mfdui::ALERT), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	} else {
		std::vector<std::string> items;
		for (const auto &o : lp.opp) {
			struct tm tt = *mjddate (o.launchMjd);
			snprintf (buf, sizeof (buf), "%02d-%02d %02d:%02d  az %03.0f  i %.1f  %.0f m/s", std::clamp (tt.tm_mon, 1, 12), tt.tm_mday,
				tt.tm_hour, tt.tm_min, o.azimuth * DEG, o.incl * DEG, o.dvTotal);
			items.push_back (buf);
		}
		std::vector<const char*> ip;
		for (auto &s : items) ip.push_back (s.c_str());
		int sel = lp.sel;
		if (ui.List ("launches", lr, ip.data(), (int)ip.size(), &sel)) pl->SelectLaunch (sel);
		ui.Tooltip ("Launch opportunities (UTC): heading from north, orbit inclination, total velocity change");
	}

	// the chosen one
	if (lp.sel < 0) return;
	const transfer::LaunchOpportunity &o = lp.opp[lp.sel];
	Rect dr = a.CutTop (u * 2.65f + lh);
	float rh = std::min (u * 0.78f, dr.H() / 7.0f);
	auto R = [&](int i) { return Rect (dr.x0, dr.y0 + i * rh, dr.x1, dr.y0 + (i + 1) * rh); };
	DWORD tc = ui.Colour (mfdui::TEXT);
	int i = 0;
	FmtDate (d0, sizeof (d0), o.launchMjd);
	FmtSpan (t, sizeof (t), (o.launchMjd - td.MJD0) * 86400.0);
	snprintf (buf, sizeof (buf), "%s  T-%s", d0, t);
	Row (ui, R(i++), "Lift-off", buf, ui.Colour (mfdui::ACCENT), 0.3f);
	snprintf (buf, sizeof (buf), "%.1f deg (%s)", o.azimuth * DEG, o.azimuth < Pi ? (o.azimuth < Pi05 ? "north-east" : "south-east") : (o.azimuth < 1.5 * Pi ? "south-west" : "north-west"));
	Row (ui, R(i++), "Heading", buf, ui.Colour (mfdui::BRIGHT), 0.3f);
	snprintf (buf, sizeof (buf), "%.0f km, inclination %.1f deg", lp.parkAlt * 1e-3, o.incl * DEG);
	Row (ui, R(i++), "Orbit", buf, tc, 0.3f);
	FmtSpan (t, sizeof (t), o.coast);
	snprintf (buf, sizeof (buf), "%s in orbit", t);
	Row (ui, R(i++), "Coast", buf, tc, 0.3f);
	FmtDate (d0, sizeof (d0), o.burnMjd);
	snprintf (buf, sizeof (buf), "%s  %.0f m/s", d0, o.dvEject);
	Row (ui, R(i++), "Ejection", buf, tc, 0.3f);
	snprintf (buf, sizeof (buf), "%.0f + %.0f m/s", lp.vAscent, o.dvPenalty);
	Row (ui, R(i++), "Ascent", buf, tc, 0.3f);
	ui.Tooltip ("Orbital speed less the ground's speed when launching due east, without gravity and drag losses; and the extra for this heading");
	snprintf (buf, sizeof (buf), "%.0f m/s after insertion", o.dvEject + (st.objective == transfer::OBJ_TOTAL ? o.dvCapture : 0.0));
	Row (ui, R(i++), "Transfer", buf, ui.Colour (mfdui::BRIGHT), 0.3f);
}

// -----------------------------------------------------------------------
// Cruise: the plan's burns, the predicted arrival

void Instrument_Planner::DrawCruise (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u;
	transfer::Plan plan = pl->GetPlan ();
	if (!plan.valid) {
		ui.TextIn (L.area, "No plan yet: make nodes (Chart, Burn)", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE);
		return;
	}
	Rect a = L.area;
	float rh = u * 0.82f;
	char buf[128], t[32];
	double now = td.MJD0;
	int i = 0;
	auto R = [&](int k) { return Rect (a.x0, a.y0 + k * rh, a.x1, a.y0 + (k + 1) * rh); };
	if (!plan.route.empty ()) {
		// a route: the burns and the flybys in time order (a scrolling list)
		std::vector<std::pair<double, std::string>> ev;
		for (const auto &n : plan.nodes) {
			FmtSpan (t, sizeof (t), (n.mjd - now) * 86400.0);
			bool pending = (n.role == transfer::ROLE_MCC || n.role == transfer::ROLE_DSM) && !n.done;
			const char *state = n.mjd < now ? "past" : pending ? (n.isNode ? "node, refined when due" : "planned") : n.isNode ? "node" : "not needed";
			if (n.role == transfer::ROLE_MCC && !n.done && !n.isNode) snprintf (buf, sizeof (buf), "%-11s T%s%s  %s", RoleName (n.role), (n.mjd - now) >= 0.0 ? "-" : "+", t[0] == '-' ? t + 1 : t, state);
			else snprintf (buf, sizeof (buf), "%-11s T%s%s  %.1f m/s %s", RoleName (n.role), (n.mjd - now) >= 0.0 ? "-" : "+", t[0] == '-' ? t + 1 : t, n.dv.length (), state);
			ev.push_back ({ n.mjd, buf });
		}
		for (size_t k = 0; k < plan.flybys.size (); k++) {
			const transfer::RouteFlyby &f = plan.flybys[k];
			FmtSpan (t, sizeof (t), (f.mjd - now) * 86400.0);
			double tgt = k < plan.flybyTarget.size () ? plan.flybyTarget[k] : f.alt;
			snprintf (buf, sizeof (buf), "%-11s T%s%s  %.0f km (target %.0f)", f.body->Name (), (f.mjd - now) >= 0.0 ? "-" : "+", t[0] == '-' ? t + 1 : t, f.alt * 1e-3, tgt * 1e-3);
			ev.push_back ({ f.mjd + 1e-9, buf });
		}
		std::stable_sort (ev.begin (), ev.end (), [](const auto &x, const auto &y) { return x.first < y.first; });
		std::vector<const char*> ip;
		for (auto &e : ev) ip.push_back (e.second.c_str ());
		int sel = -1;
		Rect lr (a.x0, a.y0, a.x1, a.y1 - u * 1.3f - rh * 3.2f);
		ui.List ("routeplan", lr, ip.data (), (int)ip.size (), &sel);
		float y = lr.y1 + u * 0.1f;
		auto Q = [&](int k) { return Rect (a.x0, y + k * rh, a.x1, y + (k + 1) * rh); };
		FmtDate (t, sizeof (t), plan.arrMjd);
		Row (ui, Q(0), "Arrival", t, ui.Colour (mfdui::TEXT), 0.3f);
		snprintf (buf, sizeof (buf), "%.1f km (target %.1f)", plan.periAlt * 1e-3, plan.periTarget * 1e-3);
		Row (ui, Q(1), "Periapsis", buf, fabs (plan.periAlt - plan.periTarget) < 0.05 * plan.periTarget + 2e3 ? ui.Colour (mfdui::TEXT) : ui.Colour (mfdui::ALERT), 0.3f);
		snprintf (buf, sizeof (buf), "%.1f deg (least %.1f)", plan.incl * DEG, plan.inclMin * DEG);
		Row (ui, Q(2), "Inclination", buf, ui.Colour (mfdui::TEXT), 0.3f);
		Rect br = a.Bottom (u * 1.15f);
		if (ui.Button ("correct", br.Col (0, 3, u * 0.2f), "Correct now")) Action (5);
		ui.Tooltip ("Compute the next correction now (aimed at the next encounter)");
		bool armed = plan.armed;
		if (ui.Toggle ("armed", br.Col (1, 3, u * 0.2f), "Armed", &armed)) pl->Arm (armed);
		if (ui.Button ("clear", br.Col (2, 3, u * 0.2f), "Clear plan")) Action (7);
		return;
	}
	// the burns
	for (const auto &n : plan.nodes) {
		Rect r = R(i++);
		bool past = n.mjd < now;
		DWORD c = past ? ui.Colour (mfdui::DIM) : (n.role == transfer::ROLE_MCC && !n.done) ? ui.Colour (mfdui::INFO) : ui.Colour (mfdui::TEXT);
		FmtSpan (t, sizeof (t), (n.mjd - now) * 86400.0);
		const char *state = past ? "past" : (n.role == transfer::ROLE_MCC && !n.done) ? "planned" : n.isNode ? "node" : "not needed";
		ui.TextIn (r.Left (r.W() * 0.3f), RoleName (n.role), c, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		snprintf (buf, sizeof (buf), "T%s%s", (n.mjd - now) >= 0.0 ? "-" : "+", t[0] == '-' ? t + 1 : t);
		ui.TextIn (r.CutLeft (r.W() * 0.3f).Left (r.W() * 0.3f), buf, c, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		if (n.role == transfer::ROLE_MCC && !n.done) snprintf (buf, sizeof (buf), "%s", state);
		else snprintf (buf, sizeof (buf), "%.1f m/s  %s", n.dv.length (), state);
		ui.TextIn (r.CutLeft (r.W() * 0.6f), buf, c, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
	i++;
	// arrival
	FmtDate (t, sizeof (t), plan.arrMjd);
	Row (ui, R(i++), "Arrival", t, ui.Colour (mfdui::TEXT), 0.3f);
	snprintf (buf, sizeof (buf), "%.1f km (target %.1f)", plan.periAlt * 1e-3, plan.periTarget * 1e-3);
	Row (ui, R(i++), "Periapsis", buf, fabs (plan.periAlt - plan.periTarget) < 0.05 * plan.periTarget + 2e3 ? ui.Colour (mfdui::TEXT) : ui.Colour (mfdui::ALERT), 0.3f);
	snprintf (buf, sizeof (buf), "%.1f deg (least %.1f)", plan.incl * DEG, plan.inclMin * DEG);
	Row (ui, R(i++), "Inclination", buf, ui.Colour (mfdui::TEXT), 0.3f);

	// the B-plane: target and predicted point
	Rect bp = Rect (a.x0 + a.W() * 0.62f, R(i).y0 - rh * 3.0f, a.x1, a.y1 - u * 1.3f);
	if (bp.H() > u * 3.0f && bp.W() > u * 3.0f) {
		double ext = std::max ({ fabs (plan.btT), fabs (plan.brT), fabs (plan.bt), fabs (plan.br), 1e3 }) * 1.4e-3;
		mfdui::PlotSpec spec (-ext, ext, -ext, ext);
		spec.zoom = spec.pan = true;
		spec.xlabel = "B.T km";
		if (ui.BeginPlot ("bplane", bp, spec)) {
			ui.PlotMarker (plan.btT * 1e-3, plan.brT * 1e-3, ui.Colour (mfdui::ACCENT), mfdui::MARK_CROSS);
			ui.PlotMarker (plan.bt * 1e-3, plan.br * 1e-3, ui.Colour (mfdui::BRIGHT), mfdui::MARK_CIRCLE);
			ui.EndPlot ();
		}
	}

	Rect br = a.Bottom (u * 1.15f);
	if (ui.Button ("correct", br.Col (0, 3, u * 0.2f), "Correct now")) Action (5);
	ui.Tooltip ("Compute the next correction now (from the trajectory as flown)");
	bool armed = plan.armed;
	if (ui.Toggle ("armed", br.Col (1, 3, u * 0.2f), "Armed", &armed)) pl->Arm (armed);
	ui.Tooltip ("Fly the burns with the burn autopilot");
	if (ui.Button ("clear", br.Col (2, 3, u * 0.2f), "Clear plan")) Action (7);
}

// -----------------------------------------------------------------------
// Budget: the plan's velocity change against the vessel's

void Instrument_Planner::DrawBudget (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u;
	Rect a = L.area;
	transfer::Solution s = pl->Selected ();
	transfer::Plan plan = pl->GetPlan ();
	double avail = pl->AvailableDv ();
	double ej = 0.0, mcc = 0.0, cap = 0.0, fly = 0.0;
	transfer::Route rt = pl->GetRoute ();
	if (plan.valid) {
		double now = td.MJD0;
		for (const auto &n : plan.nodes) {
			if (n.mjd < now) continue; // (flown)
			if (n.role == transfer::ROLE_EJECTION) ej += n.dv.length();
			else if (n.role == transfer::ROLE_MCC) mcc += (n.done || n.isNode) ? n.dv.length() : 0.0;
			else if (n.role == transfer::ROLE_FLYBY || n.role == transfer::ROLE_DSM) fly += n.dv.length();
			else cap += n.dv.length();
		}
	} else if (s.valid) {
		ej = s.burnValid ? s.burnDvMag : s.dvEject;
		cap = (pl->settings.objective == transfer::OBJ_TOTAL) ? s.dvCapture : 0.0;
		if (rt.valid) fly = rt.dvFlyby + rt.dvDsm;
	}
	double reserve = 0.01 * (ej + cap) + 5.0; // (corrections still to come)
	if (rt.valid) reserve += 15.0 * rt.flybys.size (); // (corrections after the flybys)
	double need = ej + mcc + fly + cap + reserve;
	double full = std::max (avail, need) * 1.08;
	if (full <= 0.0) {
		ui.TextIn (a, "No transfer selected", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE);
		return;
	}
	float rh = u * 0.9f;
	Rect bar (a.x0, a.y0 + u * 0.4f, a.x1, a.y0 + u * 1.6f);
	ui.FillRect (bar, ui.Colour (mfdui::FILL));
	struct Seg { const char *name; double v; DWORD col; } seg[5] = {
		{ "Ejection", ej, 0x20B0FF }, { "Corrections", mcc, 0xFF9030 }, { rt.dsm ? "Manoeuvres" : "Flyby burns", fly, 0xD060D0 },
		{ "Capture", cap, 0x40D040 }, { "Reserve", reserve, 0x808080 } };
	double x = 0.0;
	for (auto &sg : seg) {
		if (sg.v <= 0.0) continue;
		Rect r (bar.x0 + (float)(x / full) * bar.W(), bar.y0, bar.x0 + (float)((x + sg.v) / full) * bar.W(), bar.y1);
		ui.FillRect (r, sg.col);
		x += sg.v;
	}
	float ax = bar.x0 + (float)(avail / full) * bar.W();
	ui.Line (ax, bar.y0 - u * 0.4f, ax, bar.y1 + u * 0.4f, ui.Colour (mfdui::BRIGHT), 2.0f);
	ui.Text (std::min (ax, bar.x1 - u * 2.0f), bar.y1 + u * 0.4f, "available", ui.Colour (mfdui::BRIGHT), mfdui::ALIGN_CENTRE | mfdui::ALIGN_TOP, mfdui::FONT_SMALL);

	char buf[96];
	int i = 0;
	auto R = [&](int k) { return Rect (a.x0, bar.y1 + u * 1.5f + k * rh, a.x1, bar.y1 + u * 1.5f + (k + 1) * rh); };
	for (auto &sg : seg) {
		if (&sg == &seg[2] && !rt.valid) continue; // (direct transfers: no flybys)
		Rect r = R(i++);
		ui.FillRect (Rect (r.x0, r.CY() - u * 0.2f, r.x0 + u * 0.4f, r.CY() + u * 0.2f), sg.col);
		snprintf (buf, sizeof (buf), "%.0f m/s", sg.v);
		Row (ui, r.CutLeft (u * 0.6f), sg.name, buf, ui.Colour (mfdui::TEXT), 0.45f);
	}
	i++;
	snprintf (buf, sizeof (buf), "%.0f m/s", need);
	Row (ui, R(i++).CutLeft (u * 0.6f), "Required", buf, ui.Colour (mfdui::BRIGHT), 0.45f);
	double thrust, ve;
	Vector dir;
	if (vessel->MainEngines (thrust, ve, dir)) snprintf (buf, sizeof (buf), "%.0f m/s (Isp %.0f s)", avail, ve / 9.80665);
	else snprintf (buf, sizeof (buf), "no main engines");
	Row (ui, R(i++).CutLeft (u * 0.6f), "Available", buf, ui.Colour (mfdui::BRIGHT), 0.45f);
	double margin = avail - need;
	snprintf (buf, sizeof (buf), "%+.0f m/s", margin);
	Row (ui, R(i++).CutLeft (u * 0.6f), margin >= 0.0 ? "Margin" : "Short by", buf, margin >= 0.0 ? ui.Colour (mfdui::TEXT) : ui.Colour (mfdui::ALERT), 0.45f);
}

// -----------------------------------------------------------------------
// Route (gravity assists): the flyby bodies and the route's options, the
// optimised route's legs and flybys, or the suggested routes

void Instrument_Planner::DrawRoute (const Layout &L)
{
	transfer::Planner *pl = Planner ();
	mfdui::Context &ui = UI ();
	float u = L.u, gap = u * 0.15f, rh = u * 1.08f;
	transfer::Settings &s = pl->settings;
	Rect a = L.area;
	auto R = [&](int i) { return Rect (a.x0, a.y0 + i * rh, a.x1, a.y0 + i * rh + rh - gap); };
	char buf[160], d0[32];
	s.flybyAlt.resize (s.flybys.size (), 0.0);

	// the flybys
	std::vector<std::string> fl;
	for (size_t i = 0; i < s.flybys.size(); i++) {
		double alt = s.FlybyAlt (i) > 0.0 ? s.FlybyAlt (i) : transfer::Planner::DefaultFlybyAlt (s.flybys[i]);
		snprintf (buf, sizeof (buf), "%d  %s  (>= %.0f km)", (int)i + 1, s.flybys[i]->Name (), alt * 1e-3);
		fl.push_back (buf);
	}
	if (fl.empty ()) fl.push_back ("(none: a direct transfer)");
	std::vector<const char*> fp;
	for (auto &t : fl) fp.push_back (t.c_str ());
	if (flySel >= (int)s.flybys.size ()) flySel = (int)s.flybys.size () - 1;
	Rect top (a.x0, a.y0, a.x1, a.y0 + 3.0f * rh - gap);
	Rect lst = top.Left (a.W() * 0.6f), ctl = top.CutLeft (a.W() * 0.62f);
	ui.List ("flybys", lst, fp.data (), (int)fp.size (), &flySel);
	ui.Tooltip ("The flyby bodies in order, with the least periapsis altitude of each");
	auto cand = pl->FlybyBodies ();
	std::vector<const char*> cn;
	for (const CelestialBody *c : cand) cn.push_back (c->Name ());
	float ch = (ctl.H() - 2.0f * gap) / 3.0f;
	Rect c0 (ctl.x0, ctl.y0, ctl.x1, ctl.y0 + ch), c1 (ctl.x0, c0.y1 + gap, ctl.x1, c0.y1 + gap + ch), c2 (ctl.x0, c1.y1 + gap, ctl.x1, ctl.y1);
	addSel = std::clamp (addSel, 0, std::max ((int)cn.size () - 1, 0));
	if (!cn.empty ()) ui.Choice ("addbody", c0, cn.data (), (int)cn.size (), &addSel);
	ui.Tooltip ("Bodies that orbit the target's parent");
	if (ui.Button ("add", c1.Col (0, 2, gap), "Add", false, !cn.empty () && s.flybys.size () < 6)) {
		// (after the selected flyby, else at the end)
		size_t at = (flySel >= 0) ? flySel + 1 : s.flybys.size ();
		s.flybys.insert (s.flybys.begin () + at, cand[addSel]);
		s.flybyAlt.insert (s.flybyAlt.begin () + at, 0.0);
		flySel = (int)at;
		s.assists = true;
	}
	ui.Tooltip ("Add the body after the selected flyby");
	if (ui.Button ("remove", c1.Col (1, 2, gap), "Remove", false, flySel >= 0 && flySel < (int)s.flybys.size ())) {
		s.flybys.erase (s.flybys.begin () + flySel);
		s.flybyAlt.erase (s.flybyAlt.begin () + flySel);
		flySel = std::min (flySel, (int)s.flybys.size () - 1);
	}
	double alt = (flySel >= 0 && flySel < (int)s.flybys.size ()) ? (s.FlybyAlt (flySel) > 0.0 ? s.FlybyAlt (flySel) : transfer::Planner::DefaultFlybyAlt (s.flybys[flySel])) : 0.0;
	if (flySel >= 0 && flySel < (int)s.flybys.size ()) {
		if (ui.Number ("flybyalt", c2, &alt, mfdui::NumberSpec (10e3, 1e3, 1e10, "%.0f", "km", 1e-3))) s.flybyAlt[flySel] = alt;
		ui.Tooltip ("Least periapsis altitude of the selected flyby");
	}

	// options
	Rect o = R(3);
	ui.Toggle ("dsm", o.Col (0, 3, gap), "Manoeuvres", &s.dsm);
	ui.Tooltip ("One deep-space manoeuvre on each leg (else: burns at the flybys' periapses)");
	double revs = s.maxRevs, tof = s.maxTof;
	if (ui.Number ("revs", o.Col (1, 3, gap), &revs, mfdui::NumberSpec (1.0, 0.0, 3.0, "%.0f", "revs"))) s.maxRevs = (int)(revs + 0.5);
	ui.Tooltip ("Full revolutions about the parent allowed on a leg (more routes; a long leg may pass a planet again)");
	if (ui.Number ("maxtof", o.Col (2, 3, gap), &tof, mfdui::NumberSpec (30.0, 0.0, 36500.0, "%.0f", "d max"))) s.maxTof = tof;
	ui.Tooltip ("Longest flight (0: no limit)");
	Rect bt = R(4);
	transfer::Kind k = pl->KindOf (s.target);
	if (ui.Button ("routesearch", bt.Col (0, 3, gap), "Search", false, k != transfer::KIND_NONE)) Action (2);
	ui.Tooltip ("The best route by these flybys (patched conics)");
	if (ui.Button ("suggest", bt.Col (1, 3, gap), "Suggest", false, k != transfer::KIND_NONE)) pl->Suggest (), RedrawLive (0.3);
	ui.Tooltip ("Search routes with up to three flybys (the best few, and the direct transfer)");
	if (ui.Button ("routenodes", bt.Col (2, 3, gap), "Make nodes", false, pl->GetRoute ().valid)) Action (3);
	ui.Tooltip ("Target each flyby in full gravity and make the route's nodes");

	static const char *views[2] = { "Route", "Suggestions" };
	ui.Tabs ("routeview", R(5).Left (a.W() * 0.6f), views, 2, &routeView);
	Rect res (a.x0, R(6).y0, a.x1, a.y1);
	std::vector<std::string> rows;
	if (routeView == 1) {
		// the suggestions: a click takes one
		auto sg = pl->Suggestions ();
		for (const auto &x : sg) {
			std::string seq;
			for (const auto *f : x.flybys) { seq += f->Name (); seq += "-"; }
			seq += x.flybys.empty () ? "direct" : (s.target ? s.target->Name () : "?");
			FmtDate (d0, sizeof (d0), x.dep);
			snprintf (buf, sizeof (buf), "%s  %.0f m/s  %.0f d  %.10s", seq.c_str (), x.dv, x.tof, d0);
			rows.push_back (buf);
		}
		if (rows.empty ()) {
			ui.TextIn (res, "No suggestions yet: Suggest", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
			return;
		}
		std::vector<const char*> rp;
		for (auto &t : rows) rp.push_back (t.c_str ());
		int sel = -1;
		if (ui.List ("suggestions", res, rp.data (), (int)rp.size (), &sel) && sel >= 0 && sel < (int)sg.size ()) {
			routeView = 0;
			pl->UseSuggestion (sel);
			RedrawLive (0.3);
		}
		ui.Tooltip ("Click: search that route");
		return;
	}

	// the route: departure, flybys, manoeuvres, arrival
	transfer::Route rt = pl->GetRoute ();
	if (!rt.valid) {
		ui.TextIn (res, s.flybys.empty () ? "Add flybys, or Suggest" : "No route yet: Search", ui.Colour (mfdui::DIM), mfdui::ALIGN_CENTRE | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		return;
	}
	transfer::Plan plan = pl->GetPlan ();
	transfer::Solution sol = pl->Selected ();
	const CelestialBody *dep = pl->Departure ();
	FmtDate (d0, sizeof (d0), rt.dep);
	snprintf (buf, sizeof (buf), "Depart %s  %.16s  %.0f m/s", dep ? dep->Name () : "", d0, rt.dvEject);
	rows.push_back (buf);
	if (sol.burnValid && sol.burnDvMag > 1.05 * rt.dvEject) { // (the parking orbit's plane)
		snprintf (buf, sizeof (buf), "  from this orbit %.0f m/s (plane change)", sol.burnDvMag);
		rows.push_back (buf);
	}
	for (size_t i = 0; i < rt.legs.size (); i++) {
		const transfer::RouteLeg &lg = rt.legs[i];
		if (lg.dsmMjd > 0.0 && lg.dsmDv >= 0.5) {
			FmtDate (d0, sizeof (d0), lg.dsmMjd);
			snprintf (buf, sizeof (buf), "  Manoeuvre  %.10s  %.0f m/s", d0, lg.dsmDv);
			rows.push_back (buf);
		}
		if (lg.near) {
			snprintf (buf, sizeof (buf), "  ! passes %s at %.2f million km", lg.near->Name (), lg.nearDist * 1e-9);
			rows.push_back (buf);
		}
		if (i < rt.flybys.size ()) {
			const transfer::RouteFlyby &f = rt.flybys[i];
			FmtDate (d0, sizeof (d0), f.mjd);
			snprintf (buf, sizeof (buf), "%s  %.10s  %.0f km  %.0f deg%s", f.body->Name (), d0, f.alt * 1e-3, f.turn * DEG,
				lg.revs ? "  (1 rev)" : "");
			if (lg.revs > 1) { char t[16]; snprintf (t, sizeof (t), "  (%d revs)", lg.revs); char *q = strstr (buf, "  (1 rev)"); if (q) strcpy (q, t); }
			rows.push_back (buf);
			snprintf (buf, sizeof (buf), "  v-inf %.2f > %.2f km/s%s", f.vinfIn * 1e-3, f.vinfOut * 1e-3, f.dv + f.dvTurn >= 0.5 ? "" : ", unpowered");
			if (f.dv + f.dvTurn >= 0.5) { char t[48]; snprintf (t, sizeof (t), ", burn %.0f m/s", f.dv + f.dvTurn); strncat (buf, t, sizeof (buf) - strlen (buf) - 1); }
			rows.push_back (buf);
			if (plan.valid && i < plan.flybys.size ()) {
				snprintf (buf, sizeof (buf), "  predicted %.0f km", plan.flybys[i].alt * 1e-3);
				rows.push_back (buf);
			}
		}
	}
	FmtDate (d0, sizeof (d0), rt.dep + rt.tof);
	snprintf (buf, sizeof (buf), "Arrive %s  %.10s  v-inf %.2f km/s", s.target ? s.target->Name () : "", d0, rt.vinfArr * 1e-3);
	rows.push_back (buf);
	snprintf (buf, sizeof (buf), "Total %.0f m/s (capture %.0f), %.0f days", rt.dvTotal, rt.dvCapture, rt.tof);
	rows.push_back (buf);
	std::vector<const char*> rp;
	for (auto &t : rows) rp.push_back (t.c_str ());
	int sel = -1;
	ui.List ("route", res, rp.data (), (int)rp.size (), &sel);
}

// -----------------------------------------------------------------------
// Buttons and keys

void Instrument_Planner::CycleTarget (int dir)
{
	transfer::Planner *pl = Planner ();
	auto t = pl->Targets ();
	if (t.empty()) return;
	int i = 0;
	for (size_t k = 0; k < t.size(); k++) if (t[k] == pl->settings.target) i = (int)k;
	i = (i + dir + (int)t.size()) % (int)t.size();
	pl->settings.target = t[i];
	pl->settings.capAlt = 0.0;
}

void Instrument_Planner::Action (int what)
{
	transfer::Planner *pl = Planner ();
	if (!pl) return;
	switch (what) {
	case 0: page = (page + 1) % NPAGE; break;
	case 1: CycleTarget (1); break;
	case 2: pl->Search (); break;
	case 3: pl->MakeNodes (false); break;
	case 4: pl->Arm (!pl->GetPlan ().armed); break;
	case 5: pl->Correct (); break;
	case 6: pl->MakeNodes (true); break;
	case 7: pl->Clear (); break;
	case 8: pl->Select (-1.0, 0.0); break;
	}
	Refresh ();
	RedrawLive (0.3);
}

bool Instrument_Planner::KeyBuffered (DWORD key)
{
	switch (key) {
	case OAPI_KEY_P: Action (0); return true;
	case OAPI_KEY_T: Action (1); return true;
	case OAPI_KEY_S: Action (2); return true;
	case OAPI_KEY_N: Action (3); return true;
	case OAPI_KEY_A: Action (4); return true;
	case OAPI_KEY_C: Action (5); return true;
	case OAPI_KEY_O: Action (8); return true;
	case OAPI_KEY_X: Action (7); return true;
	}
	return false;
}

bool Instrument_Planner::ProcessButton (int bt, int event)
{
	static const DWORD btkey[8] = { OAPI_KEY_P, OAPI_KEY_T, OAPI_KEY_S, OAPI_KEY_N, OAPI_KEY_A, OAPI_KEY_C, OAPI_KEY_O, OAPI_KEY_X };
	if (event & PANEL_MOUSE_LBDOWN && bt >= 0 && bt < 8) return KeyBuffered (btkey[bt]);
	return false;
}

const char *Instrument_Planner::BtnLabel (int bt) const
{
	static const char *label[8] = { "PG", "TGT", "SRC", "NOD", "ARM", "COR", "OPT", "CLR" };
	return (bt >= 0 && bt < 8) ? label[bt] : 0;
}

int Instrument_Planner::BtnMenu (const MFDBUTTONMENU **menu) const
{
	static const MFDBUTTONMENU mnu[8] = {
		{ "Next page", 0, 'P' },
		{ "Next target", 0, 'T' },
		{ "Search transfers", 0, 'S' },
		{ "Make nodes", 0, 'N' },
		{ "Arm/disarm the plan", 0, 'A' },
		{ "Correction now", 0, 'C' },
		{ "Select the optimum", 0, 'O' },
		{ "Clear the plan", 0, 'X' }
	};
	if (menu) *menu = mnu;
	return 8;
}

int Instrument_Planner::ProcessMessage (int msg, void *data)
{
	return 0;
}

bool Instrument_Planner::ReadParams (std::ifstream &ifs)
{
	char cbuf[256], *pc;
	if (!FindScnHeader (ifs)) return false;
	transfer::Planner *pl = Planner ();
	bool firstFlyby = true;
	for (;;) {
		if (!ifs.getline (cbuf, 256)) return false;
		pc = trim_string (cbuf);
		if (!_strnicmp (pc, "END_MFD", 7)) break;
		if (!pl) continue;
		transfer::Settings &s = pl->settings;
		if (!_strnicmp (pc, "TARGET", 6)) {
			if (CelestialBody *cb = g_psys->GetGravObj (trim_string (pc + 6), true)) s.target = cb;
		} else if (!_strnicmp (pc, "OBJECTIVE", 9)) {
			s.objective = atoi (pc + 9);
		} else if (!_strnicmp (pc, "ARRIVAL", 7)) {
			s.arrival = atoi (pc + 7);
		} else if (!_strnicmp (pc, "ALT", 3)) {
			sscanf (pc + 3, "%lf%lf", &s.capAlt, &s.capApo);
		} else if (!_strnicmp (pc, "MCC", 3)) {
			int u0 = 1, u1 = 1, ac = 1;
			sscanf (pc + 3, "%lf%lf%d%d%d", &s.mcc[0], &s.mcc[1], &u0, &u1, &ac);
			s.useMcc[0] = u0 != 0, s.useMcc[1] = u1 != 0, s.autoCorrect = ac != 0;
		} else if (!_strnicmp (pc, "PAGE", 4)) {
			page = std::clamp (atoi (pc + 4), 0, NPAGE - 1);
		} else if (!_strnicmp (pc, "ASSISTS", 7)) {
			// gravity assists: on, deep-space manoeuvres, revolutions, longest flight, departure window
			int on = 0, dsm = 0;
			sscanf (pc + 7, "%d%d%d%lf%lf", &on, &dsm, &s.maxRevs, &s.maxTof, &s.routeSpan);
			s.assists = on != 0, s.dsm = dsm != 0;
		} else if (!_strnicmp (pc, "FLYBY", 5)) {
			// a flyby: body, least altitude [m]
			char name[128] = "";
			double alt = 0.0;
			if (firstFlyby) s.flybys.clear (), s.flybyAlt.clear (), firstFlyby = false;
			if (sscanf (pc + 5, "%127s%lf", name, &alt) >= 1)
				if (CelestialBody *cb = g_psys->GetGravObj (name, true)) s.flybys.push_back (cb), s.flybyAlt.push_back (alt);
		}
	}
	return true;
}

void Instrument_Planner::WriteParams (std::ostream &ofs) const
{
	ofs << "  TYPE Planner" << endl;
	if (transfer::Planner *pl = Planner ()) {
		const transfer::Settings &s = pl->settings;
		if (s.target) ofs << "  TARGET " << s.target->Name() << endl;
		ofs << "  OBJECTIVE " << s.objective << endl;
		ofs << "  ARRIVAL " << s.arrival << endl;
		ofs << "  ALT " << s.capAlt << ' ' << s.capApo << endl;
		ofs << "  MCC " << s.mcc[0] << ' ' << s.mcc[1] << ' ' << (int)s.useMcc[0] << ' ' << (int)s.useMcc[1] << ' ' << (int)s.autoCorrect << endl;
		if (s.assists || !s.flybys.empty ()) {
			ofs << "  ASSISTS " << (int)s.assists << ' ' << (int)s.dsm << ' ' << s.maxRevs << ' ' << s.maxTof << ' ' << s.routeSpan << endl;
			for (size_t i = 0; i < s.flybys.size (); i++) ofs << "  FLYBY " << s.flybys[i]->Name () << ' ' << s.FlybyAlt (i) << endl;
		}
	}
	ofs << "  PAGE " << page << endl;
}
