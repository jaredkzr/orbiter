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
		const double vmin = anew ? STRIKE_V : RESTRIKE_V;
		if (p.vn < vmin) continue;
		if (it != touch.end () && t - it->second.lastStrike < STRIKE_GAP) continue;
		Strike &st = struck[k];
		if (p.vn > st.vn) st.vn = p.vn, st.pos = p.pos;
		st.energy = std::max (st.energy, 0.5 * p.meff * p.vn * p.vn); // (one body: each point's effective mass is about all of it)
		st.n++, st.kind = p.kind, st.id = p.id;
	}
	for (const ContactSample &p : pts) touch[Key (p)].lastSeen = t;
	for (auto &kv : struck) {
		touch[kv.first].lastStrike = t;
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

} // namespace audio
