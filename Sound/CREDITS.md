# Sound credits

Every sound file Orbiter ships beyond its synthesized sounds, where it came
from and its licence.

| Files | Source | Licence |
| --- | --- | --- |
| `XRSound/assets/XRSound/Default/*` (callouts, effects, `Cabin Ambience/`, `Altea STS/`) | XRSound 3.0 by Douglas Beachy | MIT (`XRSound/LICENSE`) |
| `XRSound/assets/XRSound/Default/Music/Solar Serenity.ogg` | by Grzegorz Lorens ("Loru"), licensed to XRSound | see `XRSound/assets/XRSound/ReadMe.txt` |
| `Voice/*.flac` (the onboard voice: every XRSound default callout, re-voiced, and Orbiter's own; `Voice/lines.txt`, kept seeds in `Voice/lines.takes.json`) | made offline with [Qwen3-TTS-12Hz-1.7B-Base](https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-Base) (revision `fd4b2543`, Apache-2.0), cloning a *designed* voice: `Utils/Audio/voices/*.flac`, generated from a text description by [MOSS-VoiceGenerator](https://huggingface.co/OpenMOSS-Team/MOSS-VoiceGenerator) (revision `97521ec2`, Apache-2.0) - no recording of any person; `Utils/Audio/make_voice.py`. `Voice/Docking.flac` begins with XRSound's `Docking.wav` latch sound (XRSound 3.0 by Douglas Beachy, MIT) | Apache-2.0 models; the generated audio is Orbiter's (MIT) |
| `Voice/Ground/*.flac` (Mission Control's replies: each line in both CAPCOMs' voices, `<Event>_<c1|c2>_<n>`; `Voice/Ground/lines.txt`, kept seeds in `Voice/Ground/lines.takes.json`) | made offline with [Qwen3-TTS-12Hz-1.7B-Base](https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-Base) (revision `fd4b2543`, Apache-2.0), cloning two *designed* voices (`Utils/Audio/voices/capcom1.flac`, `capcom2.flac`, generated from text descriptions by [MOSS-VoiceGenerator](https://huggingface.co/OpenMOSS-Team/MOSS-VoiceGenerator), revision `97521ec2`, Apache-2.0) - no recording of any person; `Utils/Audio/make_voice.py` | Apache-2.0 models; the generated audio is Orbiter's (MIT) |
| `Generated/*.flac` (the cabin's air and events, hull creaks, fire, plasma, the Relativistic Travel drive, staging pyros, the airlock's hiss, the caution chirp and master alarm; `Generated/prompts.txt`) | made offline with [MOSS-SoundEffect v2.0](https://huggingface.co/OpenMOSS-Team/MOSS-SoundEffect-v2.0) (revision `e35df4d8`, Apache-2.0) from the text prompts and seeds in `prompts.txt`, chosen by ear; `caution` and `master_alarm` synthesized (tones); `Utils/Audio/make_sfx.py` | Apache-2.0 model; the generated audio is Orbiter's (MIT) |
| `XRSound/assets/XRSound/Default/Altea STS/*` (radio chatter) | NASA air-to-ground recordings of STS-114 and STS-121, as packaged with XRSound | public domain (NASA); XRSound's MIT |
| `Contacts/*.flac` (the contacts' recordings, cut and normalized from Freesound previews; `Src/Orbiter/Audio/ContactSynth.cpp`) | see below | CC0 1.0 (each verified on its Freesound page) |
| `Clicks/*.flac` (switches, buttons and the interface's clicks; `Utils/Audio/make_click_samples.py`) | see below | CC0 1.0 (each verified on its Freesound page) |
| `HRTF/kemar.hrtf` (head-related transfer functions for headphones; `Utils/Audio/make_hrtf.py`) | the MIT Media Lab's KEMAR measurements: Bill Gardner and Keith Martin, "HRTF Measurements of a KEMAR Dummy-Head Microphone", MIT Media Lab Perceptual Computing Technical Report #280, 1994 ([sound.media.mit.edu/resources/KEMAR.html](https://sound.media.mit.edu/resources/KEMAR.html), the compact set) | "provided free with no restrictions on use, provided the authors are cited" (Copyright 1994 MIT Media Laboratory) |

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

## Clicks

Recordings under `Sound/Clicks/`, all Creative Commons 0 (public domain) on Freesound. Each is a cut of the source's high-quality preview (48 kHz mono FLAC, a high-pass at 80 Hz - 300 Hz for the interface's - short fades, peak -1 dBFS).

| Files | Used for | Source | Author |
| --- | --- | --- | --- |
| `switch_on_1..4.flac`, `switch_off_1..4.flac` | a cockpit's switches and knobs (one way, then the other) | [Small Toggle Switch](https://freesound.org/people/loganzsound/sounds/877305/) (on: 0.609, 1.809, 3.009, 5.409 s; off: 0.855, 2.056, 3.241, 5.650 s; 180 ms each) | loganzsound |
| `button_1..4.flac` | MFD buttons | [Button Switch Toggle Operation Click](https://freesound.org/people/DustyWind/sounds/700985/) (0.152, 1.628, 2.399, 3.140 s; 200 ms each) | DustyWind |
| `ui_1..3.flac` | the launchpad's and the menus' controls | [Switch Click](https://freesound.org/people/adamdavidsmith/sounds/877181/) (1.976, 0.425, 3.782 s; 80 ms each) | adamdavidsmith |
