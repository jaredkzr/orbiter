Sound of vessel classes (Src/Orbiter/Audio/VesselSounds.h)

Every vessel sounds by itself: its engines, attitude thrusters, the air
over it, its contacts (wheels, hull, other vessels) and docking. (The
contacts, docking and animations' motors are off for now, while they are
reworked: ORBITER_AUDIO_CONTACTS=1 turns them on.) A file
<class>.cfg here (the vessel's class name, e.g. DeltaGlider.cfg) tunes it:

  ; factors on its sounds (1: as usual)
  engine_level = 1.0    ; the engines' loudness
  engine_pitch = 1.0    ; their roar's and turbopumps' pitch
  rumble = 1.0          ; the engines' sub-bass
  whine = 1.0           ; the turbopumps' whine
  crackle = 1.0         ; the jet's crackle
  rcs_level = 1.0       ; the attitude thrusters' loudness
  rcs_pitch = 1.0       ; their pitch
  air_level = 1.0       ; the air rushing over it
  contact_level = 1.0   ; rolling, skids, blows, motors

and gives animations a motor's sound (while they move; doors and gear also
latch or lock at their ends):

  Anim = <animation index> <motor|door|gear> [level]

Vessel modules can do the same from code: oapiAudioTune, oapiAudioAnimation
(OrbiterAPI.h), or from Lua: audio.tune, audio.animation.

The voice (Src/Orbiter/Audio/Callouts.h)

The vessel in focus has its flight called out in the headset: liftoff,
touchdown, "100 knots", heights from 5000 m down, Mach numbers, docking
distances, modes (RCS, autopilot, aerodynamic controls), fuel and damage
warnings, ignition, Max Q, MECO, orbit, splashdown. XRSound's recordings
(XRSound/Default) and Orbiter's own lines (Sound/Voice, made with Piper by
Utils/Audio/make_voice.py). The files are XRSound's settings: XRSound/
XRSound.cfg, and a class's XRSound/XRSound-<class>.cfg over it ("none"
switches a line off). Settings, Audio: "Voice callouts".

XRSound

Add-ons written for XRSound (XRSound.h: XRSound::CreateInstance) and Lua's
xrsound play through Orbiter's own sound; there is no XRSound module. A
class's XRSound-<class>.cfg still gives its animations sounds
([animation_<n>]) and its engines files of their own (MainEngines...: in
place of the synthesized roar).

Events (Src/Orbiter/Audio/Effects.h)

Explosions (a vessel breaking up with propellant: its blast wave),
burning wreckage, water entries and wakes, thunder (oapiAudioLightning),
the structure creaking under load in the cabin of the vessel in focus.

The radio (Src/Orbiter/Audio/Radio.h)

Near Earth, air-to-ground chatter now and then (XRSound's [ATC] settings);
Mission Control answering the flight (liftoff, MECO, orbit, entry,
docking, splashdown, wheels stop) after the light's round trip; silence in
a reentry's blackout, the ground calling when it is through; Quindar tones,
a voice channel, static growing with the distance. Settings, Audio: the
Radio volume.

Dynamic range (Settings, Audio): how much the range from a quiet cabin to
a launch pad is squeezed - Off (the true levels), Light, Medium, Strong.
