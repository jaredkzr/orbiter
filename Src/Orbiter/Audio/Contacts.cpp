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
	}
	return "?";
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
	return b;
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
