// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Radio.h"
#include "Loudness.h"
#include "AudioSystem.h"
#include "XrConfig.h"
#include "Ears.h"
#include "Vessel.h"
#include "Planet.h"
#include "Psys.h"
#include "Camera.h"
#include "Log.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

extern Vessel *g_focusobj;
extern PlanetarySystem *g_psys;

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
	constexpr double C_LIGHT = 299792458.0;
	/// Quindar tones: 2525 Hz as the ground keys its transmitter, 2475 Hz as
	/// it lets go (250 ms each)
	constexpr float QUINDAR_IN = 2525.0f, QUINDAR_OUT = 2475.0f, QUINDAR_LEN = 0.25f, QUINDAR_GAP = 0.08f;
	/// The squelch's tail (the carrier gone: noise for a moment)
	constexpr float SQUELCH_LEN = 0.12f;
	/// The radio's output: the voice's RMS on the radio bus (callouts' level)
	constexpr float RADIO_RMS = 0.08f;
	/// Each transmission's voice brought to this loudness first (BS.1770:
	/// recordings of all levels alike; about RADIO_RMS for speech)
	constexpr double RADIO_LUFS = -20.0;
	/// After the channel's clipping (its peaks at half scale): up to this
	/// (the radio about the callouts' loudness, a little under)
	constexpr float RADIO_OUT = 1.7f;
	/// A blackout: faster than this in air of this dynamic pressure
	constexpr double BLACKOUT_V = 5000.0, BLACKOUT_Q = 300.0;
	/// Entry interface: 120 km, coming down faster than 6 km/s
	constexpr double ENTRY_ALT = 120e3, ENTRY_V = 6000.0;
	const char *CHATTER_DIR = "XRSound/Default/Altea STS";
	const char *AOS_CLIP = "XRSound/Default/Altea STS/we have you back on air-to-ground_two.wav";

	/// A sound file decoded: mono, at a rate
	std::vector<float> Decode (const std::string &file, unsigned rate)
	{
		std::vector<float> out;
		std::string path = System::FindSound (file);
		if (path.empty ()) return out;
		ma_decoder_config c = ma_decoder_config_init (ma_format_f32, 1, rate);
		ma_decoder d;
		if (ma_decoder_init_file (path.c_str (), &c, &d) != MA_SUCCESS) return out;
		float tmp[4096];
		for (;;) {
			ma_uint64 got = 0;
			if (ma_decoder_read_pcm_frames (&d, tmp, 4096, &got) != MA_SUCCESS || got == 0) break;
			out.insert (out.end (), tmp, tmp + got);
		}
		ma_decoder_uninit (&d);
		return out;
	}
}

// ---- a transmission -------------------------------------------------------

std::unique_ptr<RadioSynth> RadioSynth::Make (Engine &e, std::vector<float> &&buf, bool quindar, float snr, uint32_t seed)
{
	std::unique_ptr<RadioSynth> s (new RadioSynth (std::move (buf), quindar, snr, seed));
	if (!s->Init (e)) return nullptr;
	const double L = Loudness (s->buf.data (), s->buf.size (), 1, e.SampleRate ());
	s->gain = std::isfinite (L) ? (float)std::pow (10.0, std::clamp (RADIO_LUFS - L, -20.0, 20.0) / 20.0) : 1.0f;
	return s;
}

double RadioSynth::Length () const
{
	const double tones = quindar ? 2.0 * (QUINDAR_LEN + QUINDAR_GAP) : 0.0;
	return tones + buf.size () / (double)rate + SQUELCH_LEN;
}

void RadioSynth::Render (float *out, unsigned n, float dop)
{
	if (!setup) {
		setup = true;
		hp1.HighPass (300.0f, 0.7f, rate), hp2.HighPass (300.0f, 0.7f, rate);
		lp1.LowPass (3400.0f, 0.7f, rate), lp2.LowPass (3400.0f, 0.7f, rate);
		hissBp.BandPass (1800.0f, 0.5f, rate);
	}
	const size_t nTone = quindar ? (size_t)((QUINDAR_LEN + QUINDAR_GAP) * rate) : 0;
	const size_t nVoice = buf.size (), nTail = (size_t)(SQUELCH_LEN * rate);
	const size_t total = 2 * nTone + nVoice + nTail;
	const float hiss = std::pow (10.0f, -snrDb / 20.0f) * 2.0f; // (static against the voice, band-limited)
	if (cut.load (std::memory_order_acquire) && !cutAt) cutAt = pos + (size_t)(0.15f * rate);
	for (unsigned i = 0; i < n; i++, pos++) {
		if (pos >= total || (cutAt && pos >= cutAt)) { out[i] = 0.0f; continue; }
		float x = 0.0f, tone = 0.0f, carrier = 1.0f;
		const float tt = (float)pos / rate;
		if (pos < nTone) {
			if (tt < QUINDAR_LEN) tone = 0.5f * std::sin (TWO_PI * QUINDAR_IN * tt);
		} else if (pos < nTone + nVoice) {
			x = buf[pos - nTone] * gain;
		} else if (pos < 2 * nTone + nVoice) {
			const float to = (float)(pos - nTone - nVoice) / rate - QUINDAR_GAP;
			if (to >= 0.0f && to < QUINDAR_LEN) tone = 0.5f * std::sin (TWO_PI * QUINDAR_OUT * to);
		} else carrier = 0.0f; // (the squelch's tail)
		// (lost: a burst of static, then the squelch shuts)
		float noise = hiss * RADIO_RMS;
		if (cutAt || carrier == 0.0f) noise = 0.6f * RADIO_RMS;
		const float hs = noise * 3.0f * hissBp.Process (rng.White ());
		float y = lp2.Process (lp1.Process (hp2.Process (hp1.Process (x + hs)))) + tone * RADIO_RMS * 1.5f * carrier;
		// (the channel's clipping: a little)
		y = std::tanh (2.0f * y) / 2.0f;
		out[i] = RADIO_OUT * y;
	}
	if (pos >= total || (cutAt && pos >= cutAt)) done.store (true, std::memory_order_release);
}

// ---- the radio --------------------------------------------------------------

Radio::Radio (System &s): sys (s)
{
}

Radio::~Radio ()
{
	Clear ();
}

double Radio::Snr (double d)
{
	// (the received power falls as 1/d^2: the signal to noise as 20 log10 d;
	// ground stations' dishes and gain keep it usable far out)
	return std::clamp (80.0 - 20.0 * std::log10 (std::max (d, 1e6) / 1e6), 10.0, 50.0);
}

bool Radio::Init ()
{
	Engine &e = sys.GetEngine ();
	if (!e.Active ()) return false;
	node = std::make_unique<PathNode>();
	if (!node->Init (e.Ma (), 0.0f)) { node.reset (); return false; }
	ma_node_attach_output_bus (node->Node (), PathNode::AIR, e.Group (RADIO), 0);
	PathNode::Target tg;
	tg.gain = 1.0f;
	node->Set (PathNode::AIR, tg);
	return true;
}

void Radio::Clear ()
{
	playing.clear ();
	if (node) node->Uninit (), node.reset ();
	replies.clear ();
	nextChatter = -1.0, blackoutSince = -1.0;
	blackout = entrySaid = false;
	vessel = nullptr;
}

bool Radio::Busy () const
{
	for (auto &p : playing) if (!p.s->Done ()) return true;
	return false;
}

bool Radio::Transmit (const std::string &file, bool ground)
{
	if (!node && !Init ()) return false;
	Engine &e = sys.GetEngine ();
	std::vector<float> buf = Decode (file, e.SampleRate ());
	if (buf.empty ()) { LOGOUT ("Audio: radio: no file %s", file.c_str ()); return false; }
	auto s = RadioSynth::Make (e, std::move (buf), ground, (float)Snr (earthDist), (uint32_t)(Rand () * 1e9));
	if (!s || !s->Attach (node->Node ())) return false;
	s->Start ();
	playing.push_back ({std::move (s)});
	played++;
	LOGOUT ("Audio: radio: %s (S/N %.0f dB)", file.c_str (), Snr (earthDist));
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
	if (log) printf ("Audio: radio at %.2f s: %s\n", t, file.c_str ()), fflush (stdout);
	return true;
}

void Radio::Reply (const char *file)
{
	// (heard after the light's round trip to Earth and a moment to react)
	const double rtt = 2.0 * earthDist / C_LIGHT;
	replies.push_back ({file, t + rtt + REACT + Rand (), true});
}

void Radio::OnEvent (Event e)
{
	static const char *files[NEVENT] = { "Sound/Voice/Ground/Liftoff.flac", "Sound/Voice/Ground/MECO.flac", "Sound/Voice/Ground/Orbit.flac",
		"Sound/Voice/Ground/Docked.flac", "Sound/Voice/Ground/Undocked.flac", "Sound/Voice/Ground/Splashdown.flac", "Sound/Voice/Ground/Landed.flac" };
	if (e < 0 || e >= NEVENT || !vessel) return;
	Reply (files[e]);
}

void Radio::Update (double tnow)
{
	t = tnow;
	// (the transmissions over: gone)
	playing.erase (std::remove_if (playing.begin (), playing.end (), [](const Playing &p) { return p.s->Done (); }), playing.end ());
	const Vessel *v = g_focusobj && !g_focusobj->Destroyed () ? g_focusobj : nullptr;
	if (v != vessel) { replies.clear (); nextChatter = -1.0; entrySaid = false; blackout = false; vessel = v; }
	const CelestialBody *earth = g_psys ? g_psys->GetGravObj ("Earth", true) : nullptr;
	if (!v || !earth) return;
	earthDist = std::max (0.0, (v->GPos () - earth->GPos ()).length () - earth->Size ());

	// the blackout: the plasma about a vessel entering fast
	const SurfParam *sp = v->GetSurfParam ();
	const double U = sp ? sp->airspd : 0.0, q = sp ? sp->dynp : 0.0, alt = sp ? sp->alt : 1e9;
	const double vs = sp ? sp->vspd : 0.0;
	const bool bo = sp && sp->is_in_atm && U > BLACKOUT_V && q > BLACKOUT_Q;
	if (bo && !blackout) {
		blackout = true, blackoutSince = t;
		for (auto &p : playing) p.s->Cut ();
		LOGOUT ("Audio: radio blackout at %.1f s (%.0f m/s, %.0f Pa)", t, U, q);
		static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
		if (log) printf ("Audio: radio blackout at %.2f s\n", t), fflush (stdout);
	} else if (!bo && blackout) {
		blackout = false;
		// (through it: the ground has us back)
		if (t - blackoutSince > 5.0) replies.push_back ({AOS_CLIP, t + 2.0 * earthDist / C_LIGHT + REACT + 2.0 * Rand (), false});
		LOGOUT ("Audio: radio blackout over at %.1f s", t);
	}
	// entry interface: coming down through 120 km fast (once per descent)
	if (sp && alt < ENTRY_ALT && alt > 0.8 * ENTRY_ALT && U > ENTRY_V && vs < 0.0 && !entrySaid) {
		entrySaid = true;
		Reply ("Sound/Voice/Ground/Reentry.flac");
	}
	if (alt > 2.0 * ENTRY_ALT) entrySaid = false;
	if (blackout) return;

	// the ground's replies first (dropped if a minute late: a blackout)
	while (!replies.empty () && replies.front ().due < t - 60.0) replies.pop_front ();
	if (!replies.empty () && replies.front ().due <= t && !Busy ()) {
		Pending r = replies.front ();
		replies.pop_front ();
		Transmit (r.file, r.ground);
		return;
	}

	// the chatter: near Earth, now and then, as XRSound's [ATC] settings allow
	const XrConfig &cfg = XrConfig::For (v->ClassName () ? v->ClassName () : "");
	auto atc = [&cfg](const char *k, double def) {
		auto it = cfg.atc.find (k);
		return it == cfg.atc.end () || it->second.empty () ? def : atof (it->second.c_str ());
	};
	const bool landed = v->GroundContact ();
	const bool inAtm = sp && sp->is_in_atm && q > 1.0;
	const bool reentry = inAtm && U > 1500.0 && vs < 0.0 && alt > 20e3;
	bool allowed = earthDist < 2e9 && (!landed || atc ("allowwhilelanded", 1) != 0.0) && (!inAtm || atc ("allowinatmosphere", 1) != 0.0)
		&& (!reentry || atc ("allowduringreentry", 0) != 0.0);
	if (nextChatter < 0.0) nextChatter = t + atc ("mindelay", 15.0) + Rand () * (atc ("maxdelay", 120.0) - atc ("mindelay", 15.0));
	if (t >= nextChatter) {
		const double far = earthDist > atc ("delayplanetdistance", 400.0) * 1e3 ? atc ("delayplanetmultiplier", 5.0) : 1.0;
		nextChatter = t + far * (atc ("mindelay", 15.0) + Rand () * (atc ("maxdelay", 120.0) - atc ("mindelay", 15.0)));
		if (allowed && !Busy () && replies.empty ()) {
			if (bag.empty ()) {
				// (a shuffled bag: each clip once before any again; the AOS call
				// kept for a blackout's end)
				std::error_code ec;
				std::string dir = System::FindSound (CHATTER_DIR);
				if (!dir.empty ())
					for (auto &de : std::filesystem::directory_iterator (dir, ec))
						if (de.path ().extension () == ".wav" && de.path ().filename ().string ().find ("we have you back") == std::string::npos)
							bag.push_back (std::string (CHATTER_DIR) + "/" + de.path ().filename ().string ());
				for (size_t i = bag.size (); i > 1; i--) std::swap (bag[i - 1], bag[(size_t)(Rand () * i) % i]);
			}
			if (!bag.empty ()) {
				std::string f = bag.back ();
				bag.pop_back ();
				Transmit (f, false);
			}
		}
	}
}

} // namespace audio
