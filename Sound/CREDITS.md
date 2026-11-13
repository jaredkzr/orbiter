# Sound credits

Every sound file Orbiter ships beyond its synthesized sounds, where it came
from and its licence.

| Files | Source | Licence |
| --- | --- | --- |
| `XRSound/assets/XRSound/Default/*` (callouts, effects, `Cabin Ambience/`, `Altea STS/`) | XRSound 3.0 by Douglas Beachy | MIT (`XRSound/LICENSE`) |
| `XRSound/assets/XRSound/Default/Music/Solar Serenity.ogg` | by Grzegorz Lorens ("Loru"), licensed to XRSound | see `XRSound/assets/XRSound/ReadMe.txt` |
| `Voice/*.flac` (Orbiter's own callouts; `Voice/lines.txt`) | made offline with [Piper](https://github.com/OHF-Voice/piper1-gpl) TTS, voice `en_US-ljspeech-high` ([rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices), MIT), trained on the [LJ Speech dataset](https://keithito.com/LJ-Speech-Dataset/) (public domain); `Utils/Audio/make_voice.py` | public domain dataset, MIT model; the generated audio is Orbiter's (MIT) |
| `Contacts/*.flac` (the contacts' recordings, cut and normalized from Freesound previews; `Src/Orbiter/Audio/ContactSynth.cpp`) | see below | CC0 1.0 (each verified on its Freesound page) |

## Contacts

Recordings under `Sound/Contacts/`, all Creative Commons 0 (public domain) on Freesound. Each is a cut of the source (48 kHz mono FLAC, fades, normalized).

| File | Used for | Source | Author |
| --- | --- | --- | --- |
| `clunk_heavy.flac` | the docking clunk, a belly landing's slam | [Heavy steal door closing and locking](https://freesound.org/people/Sandermotions/sounds/275441/) (2.40-3.50 s) | Sandermotions |
| `thump_door.flac` | a touchdown's strut thump, undocking's push-off | [car door close.wav](https://freesound.org/people/ninebilly/sounds/173009/) (0.30-0.76 s) | ninebilly |
| `door_close.flac` | the gear locking, a door's latch | [car door close.wav](https://freesound.org/people/monotraum/sounds/208695/) (0.10-0.57 s) | monotraum |
| `bump.flac` | a vessel bumping another | [car_door_closed.aif](https://freesound.org/people/Heigh-hoo/sounds/9876/) (0.00-0.55 s) | Heigh-hoo |
| `latch_1.flac` | a docking latch engaging | [Door, close, modern, metal, punch, low, bass, lock click, interior, room echo](https://freesound.org/people/fabuafrica/sounds/331138/) (0.02-0.50 s) | fabuafrica |
| `latch_2.flac` | a docking latch engaging | [Slide lock 1.wav](https://freesound.org/people/ingudios/sounds/119494/) (0.81-1.20 s) | ingudios |
| `latch_3.flac` | a latch releasing (undocking) | [Metallic latch release.wav](https://freesound.org/people/Crinkem/sounds/481789/) (0.17-0.42 s) | Crinkem |
| `lock.flac` | the gear's downlock | [Car Lock](https://freesound.org/people/hz37/sounds/396448/) (0.04-0.20 s) | hz37 |
| `squeal.flac` | tyres squealing (looped), a touchdown's chirp | [Tire.ogg](https://freesound.org/people/egomassive/sounds/536769/) (0.40-1.45 s) | egomassive |
| `scrape.flac` | the hull scraping over the ground (looped), a belly landing's crunch | [Metal Drag Short.mp3](https://freesound.org/people/belanhud/sounds/543625/) (0.20-3.60 s) | belanhud |
| `motor.flac` | the motors of animations (looped; servos, doors, the gear's hydraulics) | [Electric Motor Whir.aif](https://freesound.org/people/jasonLON/sounds/125401/) (2.00-6.00 s) | jasonLON |
| `Voice/Ground/*.flac` (Mission Control's replies; `Voice/Ground/lines.txt`) | made offline with Piper TTS, voice `en_US-norman-medium` ([rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices), MIT), trained on LibriVox recordings (public domain); `Utils/Audio/make_voice.py` | public domain dataset, MIT model; the generated audio is Orbiter's (MIT) |
| `XRSound/assets/XRSound/Default/Altea STS/*` (radio chatter) | NASA air-to-ground recordings of STS-114 and STS-121, as packaged with XRSound | public domain (NASA); XRSound's MIT |
