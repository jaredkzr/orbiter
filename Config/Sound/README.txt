Sound of vessel classes (Src/Orbiter/Audio/VesselSounds.h)

Every vessel sounds by itself: its engines, attitude thrusters, the air
over it, its contacts (wheels, hull, other vessels) and docking. A file
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
