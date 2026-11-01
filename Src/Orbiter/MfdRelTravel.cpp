// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MfdRelTravel.h"
#include "RelTravel.h"
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
using namespace relativity;

extern Orbiter *g_pOrbiter;
extern PlanetarySystem *g_psys;
extern TimeData td;

struct RLayout {
	Rect area;   // the page
	float u;     // layout unit
};

Instrument_RelTravel::SavePrm Instrument_RelTravel::saveprm;

namespace {
	const double WORLD_ENERGY_YEAR = 6.2e20; // [J] world primary energy use a year (~620 EJ, 2023)

	void FmtDate (char *buf, size_t n, double mjd)
	{
		struct tm t = *mjddate (mjd);
		snprintf (buf, n, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, std::clamp (t.tm_mon, 1, 12), t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	}

	void FmtSpan (char *buf, size_t n, double sec)
	{
		const char *sg = sec < 0.0 ? "-" : "";
		sec = fabs (sec);
		if (sec < 60.0) { snprintf (buf, n, "%s%.2f s", sg, sec); return; }
		long long d = (long long)(sec / 86400.0);
		sec -= d * 86400.0;
		int h = (int)(sec / 3600.0), m = (int)((sec - h * 3600.0) / 60.0), s = (int)(sec - h * 3600.0 - m * 60.0);
		if (d >= 365) snprintf (buf, n, "%s%.2f years", sg, (d * 86400.0 + sec) / (365.25 * 86400.0));
		else if (d) snprintf (buf, n, "%s%lldd %02d:%02d:%02d", sg, d, h, m, s);
		else snprintf (buf, n, "%s%02d:%02d:%02d", sg, h, m, s);
	}

	void FmtSpeed (char *buf, size_t n, double v)
	{
		if (v >= 0.001 * C) snprintf (buf, n, "%.5f c  (%.0f km/s)", v / C, v * 1e-3);
		else snprintf (buf, n, "%.1f km/s", v * 1e-3);
	}

	void FmtMass (char *buf, size_t n, double kg)
	{
		if (!std::isfinite (kg) || kg > 1e300) snprintf (buf, n, "beyond any number");
		else if (kg >= 1e12) snprintf (buf, n, "%.3g Mt", kg * 1e-9);
		else if (kg >= 1e3) snprintf (buf, n, "%.3g t", kg * 1e-3);
		else snprintf (buf, n, "%.3g kg", kg);
	}

	// two text columns: label (dim) and value
	void Row (mfdui::Context &ui, const Rect &r, const char *label, const char *value, DWORD col, float split = 0.38f)
	{
		ui.TextIn (r.Left (r.W() * split), label, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		ui.TextIn (r.CutLeft (r.W() * split), value, col, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}

	/// The trip's targets: the celestial bodies (moons under their planets),
	/// then the other vessels; display names indented by depth
	void Targets (const Vessel *self, std::vector<const Body*> &list, std::vector<std::string> &names)
	{
		list.clear(), names.clear();
		std::vector<const CelestialBody*> cel;
		for (size_t i = 0; i < g_psys->nGrav(); i++) cel.push_back (g_psys->GetGravObj ((int)i));
		std::function<void (const CelestialBody*, int)> add = [&](const CelestialBody *p, int depth) {
			for (const CelestialBody *c : cel)
				if (c->ElRef() == p && c != p) {
					list.push_back (c);
					names.push_back (std::string (depth * 2, ' ') + c->Name());
					add (c, depth + 1);
				}
		};
		for (const CelestialBody *c : cel)
			if (!c->ElRef() || c->ElRef() == c) { list.push_back (c), names.push_back (c->Name()); add (c, 1); }
		for (size_t i = 0; i < g_psys->nVessel(); i++) {
			const Vessel *v = g_psys->GetVessel ((int)i);
			if (v != self) list.push_back (v), names.push_back (std::string ("[vessel] ") + v->Name());
		}
	}
}

// =======================================================================

Instrument_RelTravel::Instrument_RelTravel (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel)
: Instrument (_pane, _id, spec, _vessel)
{
	page = (saveprm.usr == _vessel) ? saveprm.page : PG_TARGET;
}

Instrument_RelTravel::~Instrument_RelTravel ()
{
	saveprm.usr = vessel;
	saveprm.page = page;
}

reltravel::Controller *Instrument_RelTravel::Trip () const
{
	return vessel->RelTrip ();
}

bool Instrument_RelTravel::Update (double upDTscale)
{
	// live while the drive works
	if (vessel->GetRelDrive().on || Trip()->Engaged ()) RedrawLive (0.25);
	return Instrument::Update (upDTscale);
}

void Instrument_RelTravel::UpdateDraw (oapi::Sketchpad *skp)
{
	reltravel::Controller *c = Trip ();
	mfdui::Context &ui = UI ();
	ui.Begin (skp, (float)IW, (float)IH);
	float u = ui.Unit ();

	// title and the autopilot's state
	char buf[160];
	const Body *tgt = c->settings.target;
	snprintf (buf, sizeof (buf), "Relativistic travel > %s", tgt ? tgt->Name() : "?");
	ui.TextIn (Rect (0.0f, 0.0f, IW * 0.68f, u), buf, col_grey1, mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE);
	int ph = c->GetPhase ();
	ui.TextIn (Rect (IW * 0.68f, 0.0f, IW - u * 0.3f, u), reltravel::PhaseName (ph),
		ui.Colour (c->Engaged () ? mfdui::ACCENT : ph == reltravel::PH_FAILED ? mfdui::ALERT : mfdui::DIM), mfdui::ALIGN_RIGHT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);

	static const char *tabs[NPAGE] = { "Target", "Trip", "Flight", "Cost" };
	ui.Tabs ("page", Rect (u * 0.15f, u * 1.05f, IW - u * 0.15f, u * 2.05f), tabs, NPAGE, &page);

	RLayout L{ Rect (u * 0.3f, u * 2.3f, IW - u * 0.3f, IH - u * 1.05f), u };
	switch (page) {
	case PG_TARGET: DrawTarget (L); break;
	case PG_TRIP:   DrawTrip (L); break;
	case PG_FLIGHT: DrawFlight (L); break;
	case PG_COST:   DrawCost (L); break;
	}
	ui.TextIn (Rect (u * 0.2f, IH - u * 1.0f, IW - u * 0.2f, IH - u * 0.05f), c->Message ().c_str(),
		ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	ui.End ();
}

// -----------------------------------------------------------------------
// Target: the body or vessel, the arrival

void Instrument_RelTravel::DrawTarget (const RLayout &L)
{
	reltravel::Controller *c = Trip ();
	reltravel::Settings &s = c->settings;
	mfdui::Context &ui = UI ();
	float u = L.u, rh = u * 1.12f, gap = u * 0.12f;
	Rect a = L.area;
	float lw = a.W() * 0.36f;
	bool engaged = c->Engaged ();

	std::vector<const Body*> list;
	std::vector<std::string> names;
	Targets (vessel, list, names);
	std::vector<const char*> items;
	int sel = -1;
	for (size_t i = 0; i < list.size(); i++) {
		items.push_back (names[i].c_str());
		if (list[i] == s.target) sel = (int)i;
	}
	Rect lr (a.x0, a.y0, a.x1, a.y1 - 3.0f * rh);
	if (!engaged && ui.List ("targets", lr, items.data(), (int)items.size(), &sel) && sel >= 0) s.target = list[sel];
	else if (engaged) ui.List ("targets", lr, items.data(), (int)items.size(), &sel);
	ui.Tooltip ("The trip's target: any body, or another vessel");

	auto R = [&](int i) { return Rect (a.x0, a.y1 - (3 - i) * rh, a.x1, a.y1 - (3 - i) * rh + rh - gap); };
	auto Label = [&](const Rect &r, const char *t) {
		ui.TextIn (r.Left (lw), t, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	};
	bool vesselTarget = dynamic_cast<const Vessel*> (s.target) != nullptr;
	static const char *arr[3] = { "At rest", "In orbit", "Passing (flyby)" };
	Label (R(0), "Arrival");
	int ar = s.arrival;
	if (ui.Choice ("arrival", R(0).CutLeft (lw), arr, 3, &ar) && !engaged)
		s.arrival = (vesselTarget && ar == reltravel::ARRIVE_ORBIT) ? reltravel::ARRIVE_REST : ar;
	ui.Tooltip ("At rest relative to the target, in a circular orbit about it, or passing it at speed");
	Label (R(1), s.arrival == reltravel::ARRIVE_FLYBY ? "Passing at" : "Altitude");
	double alt = s.altitude;
	if (ui.Number ("altitude", R(1).CutLeft (lw), &alt, mfdui::NumberSpec (0.1, 0.0, 1e12, "%.0f", "km", 1e-3, true)) && !engaged)
		s.altitude = std::max (alt, 0.0);
	ui.Tooltip ("Above the target's surface");
	if (s.target) {
		char buf[128];
		double d = (s.target->GPos() - vessel->GPos()).length();
		if (d >= 0.01 * AU) snprintf (buf, sizeof (buf), "%s: %.4f AU away", s.target->Name(), d / AU);
		else snprintf (buf, sizeof (buf), "%s: %.0f km away", s.target->Name(), d * 1e-3);
		ui.TextIn (R(2), buf, ui.Colour (mfdui::INFO), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
}

// -----------------------------------------------------------------------
// Trip: acceleration, timing, the plan

void Instrument_RelTravel::DrawTrip (const RLayout &L)
{
	reltravel::Controller *c = Trip ();
	reltravel::Settings &s = c->settings;
	mfdui::Context &ui = UI ();
	float u = L.u, rh = u * 1.12f, gap = u * 0.12f;
	Rect a = L.area;
	float lw = a.W() * 0.36f;
	bool engaged = c->Engaged ();
	auto R = [&](int i) { return Rect (a.x0, a.y0 + i * rh, a.x1, a.y0 + i * rh + rh - gap); };
	auto Label = [&](const Rect &r, const char *t) {
		ui.TextIn (r.Left (lw), t, ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	};
	char buf[160], b2[64], b3[64];

	Label (R(0), "Acceleration");
	double g = s.accel / G0;
	if (ui.Number ("accel", R(0).CutLeft (lw), &g, mfdui::NumberSpec (0.1, 0.01, reltravel::MAX_ACCEL / G0, "%.4g", "g", 1.0, true)) && !engaged)
		s.accel = std::clamp (g, 0.01, reltravel::MAX_ACCEL / G0) * G0;
	ui.Tooltip ("Proper acceleration, felt on board (the drive pushes either way: no turn at the midpoint)");
	static const char *timing[2] = { "As soon as possible", "Trip time" };
	int tm = s.fastest ? 0 : 1;
	Label (R(1), "Timing");
	Rect tr = R(1).CutLeft (lw);
	if (ui.Choice ("timing", s.fastest ? tr : tr.Left (tr.W() * 0.55f), timing, 2, &tm) && !engaged) s.fastest = tm == 0;
	if (!s.fastest) {
		double h = s.tripTime / 3600.0;
		if (ui.Number ("triptime", tr.CutLeft (tr.W() * 0.57f), &h, mfdui::NumberSpec (0.05, 0.01, 1e7, "%.3g", "h", 1.0, true)) && !engaged)
			s.tripTime = std::max (h, 0.01) * 3600.0;
		ui.Tooltip ("Solar System time to the arrival: the burns are shorter, with a coast between");
	}

	// the plan (engaged: the autopilot's)
	const reltravel::Plan &p = engaged ? c->Current () : c->Preview ();
	double ts = 0.0, tt = 0.0;
	bool have = c->Remaining (ts, tt);
	if (have) {
		FmtSpan (b2, sizeof (b2), ts), FmtSpan (b3, sizeof (b3), tt);
		snprintf (buf, sizeof (buf), "%s   ship %s", b2, b3);
		Row (ui, R(2), "Arrival in", buf, ui.Colour (mfdui::ACCENT));
		FmtDate (b2, sizeof (b2), td.MJD0 + ts / 86400.0);
		Row (ui, R(3), "Arrival date", b2, ui.Colour (mfdui::TEXT));
	} else {
		Row (ui, R(2), "Arrival", s.target ? "no trip found" : "no target", ui.Colour (mfdui::ALERT));
	}
	if (p.ok) {
		if (p.trip.vmax >= 0.001 * C) snprintf (buf, sizeof (buf), "%.5f c   gamma %.4g", p.trip.vmax / C, Gamma (p.trip.vmax));
		else snprintf (buf, sizeof (buf), "%.1f km/s", p.trip.vmax * 1e-3);
		Row (ui, R(4), "Top speed", buf, ui.Colour (mfdui::TEXT));
	}

	// speed against Solar System time and the ship's clock
	Rect plot (a.x0, a.y0 + 5 * rh, a.x1, a.y1 - rh);
	if (p.ok && plot.H() > u * 3.0f) {
		const int N = 120;
		std::vector<double> t (N + 1), tau (N + 1), beta (N + 1);
		double t0 = p.spec.start.t, bmax = 0.0;
		for (int i = 0; i <= N; i++) {
			relativity::State st = TripState (p.spec, p.trip, p.trip.tauArrive * i / N);
			t[i] = (st.t - t0) / 3600.0, tau[i] = (st.tau - p.spec.start.tau) / 3600.0;
			beta[i] = st.V().length() / C;
			bmax = std::max (bmax, beta[i]);
		}
		mfdui::PlotSpec spec (0.0, std::max (t[N], 1e-6), 0.0, std::max (bmax * 1.1, 1e-6));
		spec.xlabel = "hours";
		if (ui.BeginPlot ("speed", plot, spec)) {
			ui.PlotLine (t.data(), beta.data(), N + 1, ui.Colour (mfdui::TEXT), 1.5f);
			ui.PlotLine (tau.data(), beta.data(), N + 1, ui.Colour (mfdui::INFO), 1.5f);
			ui.EndPlot ();
		}
		// (legend, top right)
		Rect lg (plot.x1 - a.W() * 0.42f, plot.y0, plot.x1, plot.y0 + u * 0.8f);
		ui.TextIn (lg.Left (lg.W() * 0.5f), "speed (c):", ui.Colour (mfdui::DIM), mfdui::ALIGN_RIGHT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		ui.TextIn (lg.CutLeft (lg.W() * 0.52f), "Solar Sys.", ui.Colour (mfdui::TEXT), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
		ui.TextIn (Rect (lg.x0, lg.y1, lg.x1, lg.y1 + u * 0.8f).CutLeft (lg.W() * 0.52f), "ship", ui.Colour (mfdui::INFO), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	}
	Rect br (a.x0, a.y1 - rh + gap, a.x0 + a.W() * 0.5f, a.y1);
	if (ui.Button ("engage", br, engaged ? "Disengage" : "Engage", engaged, s.target != nullptr)) Action (2);
	ui.Tooltip ("The autopilot flies the trip: it turns the vessel, drives, and stops at the arrival");
}

// -----------------------------------------------------------------------
// Flight: the vessel's motion, clock, the drive

void Instrument_RelTravel::DrawFlight (const RLayout &L)
{
	reltravel::Controller *c = Trip ();
	const reltravel::Settings &s = c->settings;
	mfdui::Context &ui = UI ();
	float u = L.u, rh = u * 1.0f, gap = u * 0.1f;
	Rect a = L.area;
	auto R = [&](int i) { return Rect (a.x0, a.y0 + i * rh, a.x1, a.y0 + i * rh + rh - gap); };
	char buf[160], b2[64], b3[64];
	DWORD txt = ui.Colour (mfdui::TEXT), acc = ui.Colour (mfdui::ACCENT);

	// speed (relative to the target, if any; else the Solar System's frame)
	Vector vrel (vessel->GVel());
	Vector r;
	if (s.target) vrel = AddVelocity (-s.target->GVel(), vessel->GVel()), r = s.target->GPos() - vessel->GPos();
	double v = vrel.length(), gam = Gamma (v);
	FmtSpeed (b2, sizeof (b2), v);
	Row (ui, R(0), s.target ? "Speed (target)" : "Speed", b2, acc);
	snprintf (buf, sizeof (buf), "%.6g   rapidity %.4g", gam, Rapidity (std::min (v, 0.999999999999 * C)));
	Row (ui, R(1), "Lorentz factor", buf, txt);

	// the drive and what it is felt as
	const Vessel::RelDrive &d = vessel->GetRelDrive ();
	double g = d.on ? d.accel / G0 : 0.0;
	snprintf (buf, sizeof (buf), "%s %.4g g%s", d.on ? "on" : "off", fabs (g), d.on && g < 0.0 ? " (reversed)" : "");
	Row (ui, R(2), "Drive", buf, d.on ? acc : txt);
	if (fabs (g) > 10.0) Row (ui, R(3), "g-load", "no crew could survive this", ui.Colour (mfdui::ALERT));
	else if (fabs (g) > 3.0) Row (ui, R(3), "g-load", "hard on a crew for long", ui.Colour (mfdui::ACCENT));
	else Row (ui, R(3), "g-load", d.on ? "bearable" : "-", txt);

	// the clock carried along
	double ofs = vessel->ClockOffset ();
	FmtDate (b2, sizeof (b2), td.MJD0 + ofs / 86400.0);
	Row (ui, R(4), "Ship clock", b2, acc);
	FmtSpan (b3, sizeof (b3), ofs);
	Row (ui, R(5), "Clock offset", b3, txt);
	ui.Tooltip ("The ship's clock less Solar System time: time dilation by speed and gravity");

	// distance: in the Solar System's frame, and in the ship's (contracted along the motion)
	if (s.target) {
		double D = r.length();
		Vector n (v > 0.0 ? vrel / v : Vector ());
		double par = dotp (r, n);
		double Dship = std::sqrt (std::max (0.0, D * D - par * par) + (par / gam) * (par / gam));
		auto fd = [](char *bf, size_t nn, double x) { if (x >= 0.01 * AU) snprintf (bf, nn, "%.5f AU", x / AU); else snprintf (bf, nn, "%.0f km", x * 1e-3); };
		fd (b2, sizeof (b2), D), fd (b3, sizeof (b3), Dship);
		snprintf (buf, sizeof (buf), "%s   ship %s", b2, b3);
		Row (ui, R(6), "Distance", buf, txt);
		double ts, tt;
		if (c->Engaged () && c->Remaining (ts, tt)) {
			FmtSpan (b2, sizeof (b2), ts), FmtSpan (b3, sizeof (b3), tt);
			snprintf (buf, sizeof (buf), "%s   ship %s", b2, b3);
			Row (ui, R(7), "Arrival in", buf, acc);
		}
	}

	// engage, or the drive by hand
	Rect br (a.x0, a.y1 - 2.0f * rh, a.x1, a.y1 - rh - gap);
	if (ui.Button ("engage2", br.Left (a.W() * 0.48f), c->Engaged () ? "Disengage" : "Engage", c->Engaged (), s.target != nullptr)) Action (2);
	if (!c->Engaged ()) {
		Rect mr (a.x0, a.y1 - rh, a.x1, a.y1);
		bool on = d.on;
		if (ui.Toggle ("manual", mr.Left (a.W() * 0.3f), "Drive", &on)) Action (on ? 6 : 7);
		ui.Tooltip ("The drive by hand: along the vessel's +z axis (reversed: -z)");
		if (ui.Number ("manualg", mr.CutLeft (a.W() * 0.32f).Left (a.W() * 0.36f), &manualG, mfdui::NumberSpec (0.1, 0.001, Vessel::RELDRIVE_MAX_ACCEL / G0, "%.4g", "g", 1.0, true)) && d.on)
			vessel->SetRelDrive ((manualRev ? -1.0 : 1.0) * manualG * G0, true);
		if (ui.Toggle ("reverse", mr.CutLeft (a.W() * 0.7f), "Reverse", &manualRev) && d.on)
			vessel->SetRelDrive ((manualRev ? -1.0 : 1.0) * manualG * G0, true);
	}
}

// -----------------------------------------------------------------------
// Cost: the rocket a real trip would need

void Instrument_RelTravel::DrawCost (const RLayout &L)
{
	reltravel::Controller *c = Trip ();
	mfdui::Context &ui = UI ();
	float u = L.u, rh = u * 1.0f, gap = u * 0.1f;
	Rect a = L.area;
	auto R = [&](int i) { return Rect (a.x0, a.y0 + i * rh, a.x1, a.y0 + i * rh + rh - gap); };
	char buf[160], b2[64], b3[64];
	DWORD txt = ui.Colour (mfdui::TEXT), acc = ui.Colour (mfdui::ACCENT);

	ui.TextIn (R(0), "A real rocket for this trip (the drive needs none):", ui.Colour (mfdui::INFO), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	const reltravel::Plan &p = c->Engaged () ? c->Current () : c->Preview ();
	if (!p.ok) { Row (ui, R(1), "Trip", "no trip planned", ui.Colour (mfdui::ALERT)); return; }
	double m = vessel->Mass ();
	double dv = p.trip.properDv; // (acceleration x burn time: the rapidity change times c)
	snprintf (buf, sizeof (buf), "%.4g c  (rapidity)", dv / C);
	Row (ui, R(1), "Velocity change", buf, txt);
	FmtMass (b2, sizeof (b2), m);
	Row (ui, R(2), "Vessel (payload)", b2, txt);

	// photon rocket: matter and antimatter annihilated into a collimated beam
	RocketCost pr = Rocket (dv, C, m);
	ui.TextIn (R(3), "Photon rocket (matter-antimatter, ideal)", ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	snprintf (buf, sizeof (buf), "%.4g", pr.massRatio);
	Row (ui, R(4), "Mass ratio", buf, acc);
	FmtMass (b2, sizeof (b2), pr.propellant), FmtMass (b3, sizeof (b3), 0.5 * pr.propellant);
	snprintf (buf, sizeof (buf), "%s (antimatter %s)", b2, b3);
	Row (ui, R(5), "Propellant", buf, txt);
	snprintf (buf, sizeof (buf), "%.3g J = %.3g yr of world use", pr.energy, pr.energy / WORLD_ENERGY_YEAR);
	Row (ui, R(6), "Energy", buf, txt);

	// another exhaust speed
	ui.TextIn (R(7).Left (a.W() * 0.45f), "Exhaust speed", ui.Colour (mfdui::DIM), mfdui::ALIGN_LEFT | mfdui::ALIGN_MIDDLE, mfdui::FONT_SMALL);
	ui.Number ("exhaust", R(7).CutLeft (a.W() * 0.45f).Left (a.W() * 0.35f), &exhaust, mfdui::NumberSpec (0.1, 1e-5, 1.0, "%.3g", "c", 1.0, true));
	ui.Tooltip ("Fusion exhaust: about 0.03-0.1 c; chemical: 1.5e-5 c");
	RocketCost ec = Rocket (dv, exhaust * C, m);
	snprintf (buf, sizeof (buf), "%.4g", ec.massRatio);
	Row (ui, R(8), "Mass ratio", buf, std::isfinite (ec.massRatio) && ec.massRatio < 1e3 ? txt : ui.Colour (mfdui::ALERT));
	FmtMass (b2, sizeof (b2), ec.propellant);
	Row (ui, R(9), "Propellant", b2, txt);
}

// -----------------------------------------------------------------------

void Instrument_RelTravel::CycleTarget (int dir)
{
	reltravel::Controller *c = Trip ();
	if (c->Engaged ()) return;
	std::vector<const Body*> list;
	std::vector<std::string> names;
	Targets (vessel, list, names);
	if (list.empty()) return;
	int i = 0;
	for (size_t k = 0; k < list.size(); k++) if (list[k] == c->settings.target) i = (int)k;
	i = (i + dir + (int)list.size()) % (int)list.size();
	c->settings.target = list[i];
	Refresh ();
}

void Instrument_RelTravel::Action (int what)
{
	reltravel::Controller *c = Trip ();
	switch (what) {
	case 0: page = (page + 1) % NPAGE; break;
	case 1: CycleTarget (1); break;
	case 2: c->Engage (!c->Engaged ()); break;
	case 3: if (!c->Engaged ()) c->settings.arrival = (c->settings.arrival + 1) % 3; break;
	case 4: if (!c->Engaged ()) c->settings.accel = std::min (c->settings.accel * 10.0, reltravel::MAX_ACCEL); break;
	case 5: if (!c->Engaged ()) c->settings.accel = std::max (c->settings.accel * 0.1, 0.01 * G0); break;
	case 6: if (!c->Engaged ()) vessel->SetRelDrive ((manualRev ? -1.0 : 1.0) * manualG * G0, true); break;
	case 7: if (!c->Engaged ()) vessel->SetRelDrive (vessel->GetRelDrive ().accel, false); break;
	}
	Refresh ();
}

bool Instrument_RelTravel::KeyBuffered (DWORD key)
{
	switch (key) {
	case OAPI_KEY_P: Action (0); return true;
	case OAPI_KEY_T: Action (1); return true;
	case OAPI_KEY_E: Action (2); return true;
	case OAPI_KEY_A: Action (3); return true;
	case OAPI_KEY_U: Action (4); return true;
	case OAPI_KEY_D: Action (5); return true;
	case OAPI_KEY_M: Action (vessel->GetRelDrive ().on ? 7 : 6); return true;
	}
	return false;
}

bool Instrument_RelTravel::ProcessButton (int bt, int event)
{
	static const DWORD btkey[7] = { OAPI_KEY_P, OAPI_KEY_T, OAPI_KEY_E, OAPI_KEY_A, OAPI_KEY_U, OAPI_KEY_D, OAPI_KEY_M };
	if (event & PANEL_MOUSE_LBDOWN && bt >= 0 && bt < 7) return KeyBuffered (btkey[bt]);
	return false;
}

const char *Instrument_RelTravel::BtnLabel (int bt) const
{
	static const char *label[7] = { "PG", "TGT", "ENG", "ARR", "AC+", "AC-", "DRV" };
	return (bt >= 0 && bt < 7) ? label[bt] : 0;
}

int Instrument_RelTravel::BtnMenu (const MFDBUTTONMENU **menu) const
{
	static const MFDBUTTONMENU mnu[7] = {
		{ "Next page", 0, 'P' },
		{ "Next target", 0, 'T' },
		{ "Engage/disengage", "the autopilot", 'E' },
		{ "Arrival: rest,", "orbit, flyby", 'A' },
		{ "Acceleration x10", 0, 'U' },
		{ "Acceleration /10", 0, 'D' },
		{ "Drive by hand", "on/off", 'M' }
	};
	if (menu) *menu = mnu;
	return 7;
}

bool Instrument_RelTravel::ReadParams (std::ifstream &ifs)
{
	char cbuf[256], *pc;
	if (!FindScnHeader (ifs)) return false;
	for (;;) {
		if (!ifs.getline (cbuf, 256)) return false;
		pc = trim_string (cbuf);
		if (!_strnicmp (pc, "END_MFD", 7)) break;
		if (!_strnicmp (pc, "PAGE", 4)) page = std::clamp (atoi (pc + 4), 0, NPAGE - 1);
		else if (!_strnicmp (pc, "EXHAUST", 7)) exhaust = std::clamp (atof (pc + 7), 1e-5, 1.0);
	}
	return true;
}

void Instrument_RelTravel::WriteParams (std::ostream &ofs) const
{
	// (the trip itself is the vessel's: RELTRIP)
	ofs << "  TYPE RelTravel" << endl;
	ofs << "  PAGE " << page << endl;
	ofs << "  EXHAUST " << exhaust << endl;
}
