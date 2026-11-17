// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Contacts.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace audio {

const char *ContactKindName (ContactKind k)
{
	switch (k) {
	case ContactKind::WHEEL: return "wheel";
	case ContactKind::LEG: return "leg";
	case ContactKind::HULL: return "hull";
	case ContactKind::OBSTACLE: return "obstacle";
	case ContactKind::VESSEL: return "vessel";
	}
	return "?";
}

const char *SurfaceKindName (SurfaceKind s)
{
	switch (s) {
	case SurfaceKind::PAVED: return "paved";
	case SurfaceKind::SOIL: return "soil";
	case SurfaceKind::REGOLITH: return "regolith";
	case SurfaceKind::ROCK: return "rock";
	case SurfaceKind::SNOW: return "snow";
	case SurfaceKind::WATER: return "water";
	}
	return "?";
}

namespace {
	double SmoothStep (double e0, double e1, double x)
	{
		const double t = std::clamp ((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}
}

SurfaceKind SurfaceFromCover (double r, double g, double b, double water, bool have)
{
	if (!have) return SurfaceKind::SOIL;
	if (water > 0.5) return SurfaceKind::WATER;
	const double sum = r + g + b + 1e-3, lum = sum / 3.0, green = g / sum;
	const double mx = std::max ({r, g, b}), mn = std::min ({r, g, b});
	const double sat = (mx - mn) / (mx + 1e-3);
	// (snow: bright, grey - and not warm: a dry lake's bed, a salt flat are as
	// bright and grey but tan; snow's blue is at least its red)
	if (SmoothStep (0.55, 0.7, lum) * SmoothStep (0.25, 0.1, sat) > 0.5 && b >= 0.97 * r) return SurfaceKind::SNOW;
	if (SmoothStep (0.345, 0.385, green) > 0.5) return SurfaceKind::SOIL;
	return SurfaceKind::ROCK;
}

bool SnowLikely (double latDeg, double elev)
{
	return std::fabs (latDeg) > SNOW_LAT || elev > SNOW_ELEV;
}

SurfaceLayers LayersFor (SurfaceKind s)
{
	switch (s) {
	case SurfaceKind::SOIL:
	case SurfaceKind::ROCK:     // (dirt and gravel: rolling and scraping on them, a spray of it at touchdown and at the hull's blows)
		return {"rolling_soil", "scrape_soil", "spray", "spray", false, true, 1.0};
	case SurfaceKind::SNOW:     // (packed snow: the ground's rolling, no chirp - a tyre doesn't bark on snow - nor spray)
		return {"rolling_soil", "scrape_soil", nullptr, nullptr, false, true, 1.0};
	case SurfaceKind::REGOLITH: // (no air: heard through the structure only - the gear's crunch at every blow, a quieter rolling)
		return {"rolling_soil", "scrape_soil", nullptr, "crunch", true, false, 0.5};
	case SurfaceKind::WATER:    // (the splash is VesselWater's: nothing of the ground's)
		return {"rolling_soil", "scrape_soil", nullptr, nullptr, false, true, 1.0};
	case SurfaceKind::PAVED:
	default:
		return {"rolling", "scrape", "chirp", nullptr, false, true, 1.0};
	}
}

const char *FallbackLayer (const char *layer)
{
	if (!layer) return nullptr;
	const std::string l (layer);
	if (l == "rolling_soil") return "rolling";
	if (l == "scrape_soil") return "scrape";
	if (l == "spray") return "chirp";
	return nullptr;   // (crunch: none - silent)
}

std::string Describe (const ContactEvent &e)
{
	char buf[256];
	switch (e.type) {
	case ContactEvent::IMPACT:
		if (e.id >= 0) snprintf (buf, sizeof buf, "impact %s %d at %.3f s: %.2f m/s, %.4g J",
			ContactKindName (e.kind), e.id, e.t, e.speed, e.energy);
		else snprintf (buf, sizeof buf, "impact %s (%d points) at %.3f s: %.2f m/s, %.4g J",
			ContactKindName (e.kind), e.points, e.t, e.speed, e.energy);
		if (e.kind != ContactKind::VESSEL) {
			std::string s (buf);
			return s + ", " + SurfaceKindName (e.surface);
		}
		break;
	case ContactEvent::DOCK:
		snprintf (buf, sizeof buf, "dock at %.3f s: %.3f m/s, %.4g kg, %.4g J", e.t, e.speed, e.mass, e.energy);
		break;
	case ContactEvent::UNDOCK:
		snprintf (buf, sizeof buf, "undock at %.3f s: %.3f m/s", e.t, e.speed);
		break;
	case ContactEvent::BREAK:
		snprintf (buf, sizeof buf, "break at %.3f s: %.4g kg, %.2f m/s", e.t, e.mass, e.speed);
		break;
	case ContactEvent::CRASH:
		snprintf (buf, sizeof buf, "crash at %.3f s: %.2f m/s, %.4g kg, %.4g J", e.t, e.speed, e.mass, e.energy);
		break;
	}
	return buf;
}

void ContactModel::Step (double t, double h, const std::vector<ContactSample> &pts, SurfaceKind s)
{
	levels = ContactLevels ();
	// (the points of a key struck together: the fastest's speed and place, the largest energy)
	struct Strike { double vn = 0.0, energy = 0.0; int n = 0; V3 pos; ContactKind kind; int id; };
	std::map<int, Strike> struck;
	std::map<int, double> stepv;   // (each key's fastest closing this step)
	const double left = std::max (LEFT_AFTER, 1.5 * h);
	for (const ContactSample &p : pts) {
		levels.any = true;
		const double slip = std::hypot (p.slipLong, p.slipLat), F = std::hypot (p.Flong, p.Flat);
		switch (p.kind) {
		case ContactKind::WHEEL:
			levels.wheelLoad += p.Fn;
			levels.rollSpeed = std::max (levels.rollSpeed, std::fabs (p.slipLong));
			levels.scrubPower += std::fabs (p.Flat * p.slipLat);
			if (p.brake > 0.0) levels.brakePower += std::fabs (p.Flong * p.slipLong);
			break;
		case ContactKind::VESSEL:
			levels.vesselScrape += F * slip;
			break;
		default:
			levels.scrapePower += F * slip;
			levels.scrapeSpeed = std::max (levels.scrapeSpeed, slip);
			break;
		}
		const int k = Key (p);
		auto it = touch.find (k);
		const bool anew = it == touch.end () || t - it->second.lastSeen > left;
		// (kept: struck again only when its closing jumps - a bump, a bounce -, not
		// while it goes on closing ever more slowly, the strut compressing)
		const double vmin = anew ? STRIKE_V : std::max (RESTRIKE_V, it->second.vlow + RESTRIKE_V);
		auto sv = stepv.find (k);
		if (sv == stepv.end ()) stepv[k] = p.vn; else sv->second = std::max (sv->second, p.vn);
		if (p.vn < vmin) continue;
		if (it != touch.end () && t - it->second.lastStrike < STRIKE_GAP) continue;
		Strike &st = struck[k];
		if (p.vn > st.vn) st.vn = p.vn, st.pos = p.pos;
		st.energy = std::max (st.energy, 0.5 * p.meff * p.vn * p.vn); // (one body: each point's effective mass is about all of it)
		st.n++, st.kind = p.kind, st.id = p.id;
	}
	for (const auto &kv : stepv) {
		Touch &tc = touch[kv.first];
		tc.vlow = t - tc.lastSeen > left ? kv.second : std::min (tc.vlow, kv.second);
		tc.lastSeen = t;
	}
	for (auto &kv : struck) {
		Touch &tc = touch[kv.first];
		tc.lastStrike = t, tc.vlow = kv.second.vn;
		ContactEvent e;
		e.type = ContactEvent::IMPACT;
		e.t = t, e.kind = kv.second.kind, e.id = kv.second.id, e.points = kv.second.n;
		e.speed = kv.second.vn, e.energy = kv.second.energy, e.pos = kv.second.pos, e.surface = s;
		events.push_back (e);
	}
	// (points long gone: forgotten)
	for (auto it = touch.begin (); it != touch.end (); )
		it = t - it->second.lastSeen > 10.0 ? touch.erase (it) : std::next (it);
}

void ContactModel::Dock (double t, const V3 &port, double speed, double mred)
{
	ContactEvent e;
	e.type = ContactEvent::DOCK;
	e.t = t, e.pos = port, e.speed = speed, e.mass = mred, e.energy = 0.5 * mred * speed * speed;
	events.push_back (e);
}

void ContactModel::Undock (double t, const V3 &port, double speed)
{
	ContactEvent e;
	e.type = ContactEvent::UNDOCK;
	e.t = t, e.pos = port, e.speed = speed;
	events.push_back (e);
}

void ContactModel::Break (double t, const V3 &pos, double mass, double hitSpeed)
{
	ContactEvent e;
	e.type = ContactEvent::BREAK;
	e.t = t, e.pos = pos, e.mass = mass, e.speed = hitSpeed;
	events.push_back (e);
}

ContactBeds BedsFor (const ContactLevels &l)
{
	ContactBeds b;
	const double v = std::min (std::max (l.rollSpeed, 0.0), ROLL_VMAX);
	if (l.wheelLoad > 0.0 && v > 0.0)
		b.rollP = ROLL_P * std::pow (v / ROLL_V, 1.5) * std::sqrt (l.wheelLoad / ROLL_LOAD);
	b.rollRate = ROLL_RATE0 + (ROLL_RATE1 - ROLL_RATE0) * std::min (v / ROLL_RATE_V, 1.0);
	const double W = std::max (l.brakePower, 0.0) + std::max (l.scrubPower, 0.0);
	if (W > 0.0) b.brakeP = std::min (BRAKE_P * std::sqrt (W / BRAKE_W), 4.0 * BRAKE_P);
	b.brakeRate = BRAKE_RATE0 + (BRAKE_RATE1 - BRAKE_RATE0) * std::min (v / BRAKE_RATE_V, 1.0);
	const double S = std::max (l.scrapePower, 0.0) + std::max (l.vesselScrape, 0.0);
	const double u = std::clamp (l.scrapeSpeed / SCRAPE_V, 0.0, 1.0);
	if (S > 0.0) b.scrapeP = std::min (SCRAPE_P * std::sqrt (S / SCRAPE_W), SCRAPE_CAP * SCRAPE_P);
	b.scrapeRate = SCRAPE_RATE0 + (SCRAPE_RATE1 - SCRAPE_RATE0) * u;
	b.scrapeCutoff = SCRAPE_FC0 * std::pow (SCRAPE_FC1 / SCRAPE_FC0, u);
	return b;
}

double ScrapeBurst::Step (double t, double power)
{
	if (power <= 0.0) return 1.0;
	if (t - lastOn > SCRAPE_GAP) startT = t;   // (begun, or touching again after a while)
	lastOn = t;
	return 1.0 + (BURST_GAIN - 1.0) * std::exp (-(t - startT) / BURST_T);
}

BreakSound BreakFor (double mass, double hitSpeed, double u01)
{
	BreakSound b;
	const double m = std::max (mass, 0.0), v = std::max (hitSpeed, BREAK_VMIN);
	b.tearE = ETA_TEAR * 0.5 * m * v * v;
	b.clatterE = CLATTER_SHARE * b.tearE;
	b.rate = std::clamp (std::pow (std::max (m, 1.0) / 1000.0, -0.1), TEAR_RATE[0], TEAR_RATE[1]);
	b.clatterDelay = CLATTER_DELAY[0] + (CLATTER_DELAY[1] - CLATTER_DELAY[0]) * std::clamp (u01, 0.0, 1.0);
	return b;
}

std::vector<CrashPart> CrashFor (double energy, double mass, double u01a, double u01b)
{
	std::vector<CrashPart> parts;
	const double E = ETA_CRASH * std::max (energy, 0.0);
	if (E <= 0.0) return parts;
	const double rate = std::clamp (std::pow (std::max (mass, 1.0) / 15000.0, -0.12), 0.7, 1.3);   // (heavier: lower - as the blows')
	parts.push_back ({0, 0.0, CRASH_SHARE * E, rate});
	for (int k = 0; k < CRASH_TEARS; k++) {
		// (staggered over CRASH_TEAR_T: evenly, a little shuffled)
		const double u = (k + (k == 0 ? std::clamp (u01a, 0.0, 1.0) : k == 1 ? std::clamp (u01b, 0.0, 1.0) : 0.5)) / CRASH_TEARS;
		parts.push_back ({1, CRASH_TEAR_T * u, TEAR_SHARE * E, rate * (0.9 + 0.2 * u)});
	}
	parts.push_back ({2, CRASH_CLATTER_T, CLATTER_SHARE_C * E, rate});
	return parts;
}

std::vector<Chirp> Chirps::Step (double t, const std::vector<ContactSample> &pts, const std::vector<ContactEvent> &events, double mass)
{
	std::vector<Chirp> out;
	for (const ContactSample &p : pts)
		if (p.kind == ContactKind::WHEEL && p.id >= 0 && !seen.count (p.id)) seen[p.id] = -1e9, wheels++;
	const double g = 9.81;
	const double load = mass * g / std::max (wheels, 3);       // (a wheel's static load [N])
	const double mw = WHEEL_MASS * load / g;                    // (its wheel's mass [kg])
	for (const ContactEvent &e : events) {
		if (e.type != ContactEvent::IMPACT || e.kind != ContactKind::WHEEL || e.id < 0) continue;
		const ContactSample *p = nullptr;
		for (const ContactSample &q : pts) if (q.id == e.id && q.kind == ContactKind::WHEEL) { p = &q; break; }
		if (!p) continue;
		const double v = std::fabs (p->slipLong);
		if (v <= CHIRP_V) continue;
		if (t - seen[e.id] < CHIRP_AIR + 1e-9) continue;           // (rolling all along: spun up)
		auto c = chirped.find (e.id);
		if (c != chirped.end () && t - c->second < CHIRP_GAP) continue;
		const double Fn = std::max (p->Fn, load / 3.0);
		Chirp ch;
		ch.on = true, ch.id = e.id, ch.t = e.t, ch.speed = v;
		ch.spinUp = std::clamp (0.6 * mw * v / (MU_TYRE * Fn), CHIRP_T[0], CHIRP_T[1]);
		ch.work = MU_TYRE * Fn * 0.5 * v * ch.spinUp;
		chirped[e.id] = t;
		out.push_back (ch);
	}
	for (const ContactSample &p : pts)
		if (p.kind == ContactKind::WHEEL && p.id >= 0) seen[p.id] = t;
	return out;
}

} // namespace audio

namespace audio {

std::vector<MechShot> DockTimeline (double speed, double mred, Uniforms &u)
{
	std::vector<MechShot> out;
	auto between = [&](const double r[2]) { return r[0] + (r[1] - r[0]) * u (); };
	auto vary = [&]() { return 0.94 + 0.12 * u (); };
	// the ports meeting
	out.push_back ({MechShot::BLOW, 0.0, MECH_ETA * 0.5 * std::max (mred, 0.0) * speed * speed, 1.0});
	// captured; the hooks closing one after another; sealed
	const double tl = between (DOCK_LATCH_T);
	out.push_back ({MechShot::LATCH, tl, LATCH_P, vary ()});
	const int n = DOCK_HOOKS[0] + (int)(u () * (DOCK_HOOKS[1] - DOCK_HOOKS[0] + 1) * 0.999);
	double t = tl + HOOK_AFTER;
	for (int i = 0; i < n; i++) {
		out.push_back ({MechShot::HOOK, t, HOOK_P * (0.8 + 0.4 * u ()), vary ()});
		if (i < n - 1) t += between (HOOK_GAP);
	}
	const double ts = std::clamp (t + SEAL_AFTER, DOCK_SEAL_T[0], DOCK_SEAL_T[1]);
	out.push_back ({MechShot::SEAL, std::max (ts, t + 0.05), SEAL_P, vary ()});
	return out;
}

std::vector<MechShot> UndockTimeline (double speed, double mass, Uniforms &u)
{
	std::vector<MechShot> out;
	auto between = [&](const double r[2]) { return r[0] + (r[1] - r[0]) * u (); };
	auto vary = [&]() { return 0.94 + 0.12 * u (); };
	out.push_back ({MechShot::RELEASE, 0.0, RELEASE_P, vary ()});
	const int n = UNDOCK_HOOKS[0] + (int)(u () * (UNDOCK_HOOKS[1] - UNDOCK_HOOKS[0] + 1) * 0.999);
	double t = HOOK_AFTER;
	for (int i = 0; i < n; i++) {
		out.push_back ({MechShot::HOOK, t, HOOK_P * (0.7 + 0.3 * u ()), vary ()});
		if (i < n - 1) t += between (HOOK_GAP);
	}
	out.push_back ({MechShot::PUSH, t + PUSH_AFTER, MECH_ETA * 0.5 * (0.5 * std::max (mass, 0.0)) * speed * speed, 1.0});
	return out;
}

MotorTrack::Event MotorTrack::Step (double t, double dt, double state)
{
	Event e = NONE;
	if (last >= 0.0 && dt > 0.0) {
		speed = std::fabs (state - last) / dt;
		const bool now = speed > MOVING;
		if (now && !moving) e = START, startT = t;
		else if (!now && moving) e = (state < MOTOR_END || state > 1.0 - MOTOR_END) ? END : STOP;
		moving = now;
	}
	last = state;
	return e;
}

MotorSound MotorFor (int kind, double level, double speed, double t, double phase)
{
	MotorSound m;
	const int k = std::clamp (kind, 0, 2);
	m.p = MOTOR_P[k] * std::max (level, 0.0);
	const double s = std::clamp (speed / MOTOR_SPEED_REF, 0.0, 1.0);   // (slower: lower - a start's run-up too)
	m.rate = MOTOR_RATE[k] * (1.0 - MOTOR_PITCH + 2.0 * MOTOR_PITCH * s) * (1.0 + MOTOR_WOBBLE * std::sin (2.0 * 3.14159265358979 * MOTOR_WOBBLE_HZ * t + phase));
	return m;
}

} // namespace audio
