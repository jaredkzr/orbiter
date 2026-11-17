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
| `Contacts/<layer>/*.flac` (the vessels' contact sounds - 24 layers: the wheels', the hull's, docking's, mechanisms', surfaces', a crash's and its aftermath's - listed in `Contacts/contacts.txt`; cut, filtered and levelled from Freesound previews; `Src/Orbiter/Audio/ContactVoices.cpp`, `SampleBank.cpp`) | see below, and `Contacts/SOURCES.md` | CC0 1.0 and CC BY 3.0 / 4.0 (each verified on its Freesound page; the CC BY ones credited below) |
| `Clicks/*.flac` (switches, buttons and the interface's clicks; `Utils/Audio/make_click_samples.py`) | see below | CC0 1.0 (each verified on its Freesound page) |
| `HRTF/kemar.hrtf` (head-related transfer functions for headphones; `Utils/Audio/make_hrtf.py`) | the MIT Media Lab's KEMAR measurements: Bill Gardner and Keith Martin, "HRTF Measurements of a KEMAR Dummy-Head Microphone", MIT Media Lab Perceptual Computing Technical Report #280, 1994 ([sound.media.mit.edu/resources/KEMAR.html](https://sound.media.mit.edu/resources/KEMAR.html), the compact set) | "provided free with no restrictions on use, provided the authors are cited" (Copyright 1994 MIT Media Laboratory) |

## Contacts

The vessels' contact sounds (`Sound/Contacts/<layer>/`, listed in `Sound/Contacts/contacts.txt`): cut from these Freesound recordings (their previews for now), filtered, levelled and in places layered or pitched (offline, by scripts kept outside this repository). CC BY recordings are credited here as their licence asks.

| Layers | Source | Author | Licence |
| --- | --- | --- | --- |
| `brake`, `rolling` | [G07-11_B-29 4 Engine Prop Taxis.wav](https://freesound.org/people/craigsmith/sounds/437855/) | craigsmith | CC0 1.0 |
| `brake`, `rolling` | [G49-07-Interior Moving Train.wav](https://freesound.org/people/craigsmith/sounds/438796/) | craigsmith | CC0 1.0 |
| `brake`, `rolling` | [Driving.wav](https://freesound.org/people/acarney2019/sounds/489431/) | acarney2019 | CC0 1.0 |
| `chirp` | [Screeching Tyres](https://freesound.org/people/iainmccurdy/sounds/650934/) | iainmccurdy | CC BY 4.0 |
| `chirp` | [Chrysler LHS tire squeal 03 (04-25-2009).wav](https://freesound.org/people/audible-edge/sounds/71738/) | audible-edge | CC0 1.0 |
| `chirp` | [tire_skid_02](https://freesound.org/people/loganzsound/sounds/774265/) | loganzsound | CC0 1.0 |
| `clatter` | [QUICK_SMASH_DEBRIS_001.wav](https://freesound.org/people/JoelAudio/sounds/135459/) | JoelAudio | CC BY 4.0 |
| `clatter` | [Scrap_Metal_Rummaging_3.wav](https://freesound.org/people/mickyman5000/sounds/340580/) | mickyman5000 | CC0 1.0 |
| `clatter` | [metalClunk.wav](https://freesound.org/people/spukkin/sounds/70887/) | spukkin | CC0 1.0 |
| `crash` | [HEAVY_SMASH_002.wav](https://freesound.org/people/JoelAudio/sounds/135452/) | JoelAudio | CC BY 4.0 |
| `crash` | [assorted_crashing.aiff](https://freesound.org/people/nothayama/sounds/172498/) | nothayama | CC BY 4.0 |
| `crash`, `impact_gear`, `impact_metal` | [MetalGate.wav](https://freesound.org/people/ahill86/sounds/207137/) | ahill86 | CC0 1.0 |
| `crash` | [crash.wav](https://freesound.org/people/moxobna/sounds/40158/) | moxobna | CC0 1.0 |
| `crash` | [R29-28-Large Metal Vehicle Crashes.wav](https://freesound.org/people/craigsmith/sounds/487143/) | craigsmith | CC0 1.0 |
| `crash` | [A freight train violently shunting cars](https://freesound.org/people/serøutōnin--deprivəd/sounds/723528/) | serøutōnin--deprivəd | CC0 1.0 |
| `crunch` | [Gravel Crunch.wav](https://freesound.org/people/Percy Duke/sounds/25903/) | Percy Duke | CC BY 3.0 |
| `crunch` | [gravel.wav](https://freesound.org/people/thedapperdan/sounds/26907/) | thedapperdan | CC0 1.0 |
| `crunch` | [gravel-walking.wav](https://freesound.org/people/brianobush/sounds/397033/) | brianobush | CC BY 4.0 |
| `debris_land` | [Metal_heavy_dropping into large bin_x4.wav](https://freesound.org/people/markpSFX/sounds/262193/) | markpSFX | CC BY 3.0 |
| `debris_land` | [Falling scrap metal](https://freesound.org/people/13FPanská_Tolar_David/sounds/378670/) | 13FPanská_Tolar_David | CC0 1.0 |
| `debris_land` | [Metal Drop in Garage](https://freesound.org/people/loganzsound/sounds/874472/) | loganzsound | CC0 1.0 |
| `explosion` | [Nearby explosion with debris.wav](https://freesound.org/people/juskiddink/sounds/108641/) | juskiddink | CC BY 4.0 |
| `explosion` | [Explosion.mp3](https://freesound.org/people/Yap_Audio_Production/sounds/218483/) | Yap_Audio_Production | CC BY 4.0 |
| `explosion` | [Rumbling Rocky sound](https://freesound.org/people/Opsaaaaa/sounds/335337/) | Opsaaaaa | CC BY 3.0 |
| `explosion` | [Astronomical Explosion](https://freesound.org/people/SamsterBirdies/sounds/760509/) | SamsterBirdies | CC0 1.0 |
| `explosion` | [custom_rock_or_boulder_explosion_impact_sounds_part_05_02032026](https://freesound.org/people/Artninja/sounds/844127/) | Artninja | CC BY 4.0 |
| `fire` | [ELEMENTS_FIRE_02_Two-Stages.wav](https://freesound.org/people/suonho/sounds/17782/) | suonho | CC BY 4.0 |
| `fire` | [Fire_Loop.wav](https://freesound.org/people/ReadeOnly/sounds/186924/) | ReadeOnly | CC0 1.0 |
| `fire` | [Fire Burning Loop](https://freesound.org/people/midimagician/sounds/249418/) | midimagician | CC0 1.0 |
| `fire` | [Fire burning.wav](https://freesound.org/people/Marissrar/sounds/366892/) | Marissrar | CC0 1.0 |
| `hook` | [Cargo bike open and close](https://freesound.org/people/Eelke/sounds/522000/) | Eelke | CC BY 4.0 |
| `hook` | [Creaky microwave door](https://freesound.org/people/Clagnar/sounds/546749/) | Clagnar | CC0 1.0 |
| `impact_gear`, `impact_metal` | [storm door slam 01.wav](https://freesound.org/people/volivieri/sounds/161190/) | volivieri | CC BY 4.0 |
| `impact_gear`, `impact_metal` | [big_metal_unlock_and_slam.wav](https://freesound.org/people/simpsi/sounds/175663/) | simpsi | CC BY 3.0 |
| `impact_gear` | [Sailplane Landing.wav](https://freesound.org/people/Grotelue/sounds/204737/) | Grotelue | CC0 1.0 |
| `impact_gear`, `rattle` | [Light Metal Gate Close](https://freesound.org/people/qubodup/sounds/219587/) | qubodup | CC BY 4.0 |
| `impact_gear`, `impact_metal` | [Metallic_Slamming_Heavy_Many.wav](https://freesound.org/people/mickyman5000/sounds/340598/) | mickyman5000 | CC0 1.0 |
| `impact_gear`, `impact_metal` | [door metal big heavy close kinda slam thud echo offmic.wav](https://freesound.org/people/kyles/sounds/406197/) | kyles | CC0 1.0 |
| `impact_gear`, `impact_metal` | [Walking off the jetti. North Wales_01.wav](https://freesound.org/people/suckmadeck/sounds/686998/) | suckmadeck | CC BY 4.0 |
| `impact_gear`, `rattle` | [Rattling Metal Cabinet Door 8.wav](https://freesound.org/people/F.M.Audio/sounds/687131/) | F.M.Audio | CC BY 4.0 |
| `impact_gear`, `rattle` | [Junkyard Foley and FX Percs (Metal, Clanks, Scrapes, Bangs, Scrap, and Machines) 99.wav](https://freesound.org/people/CVLTIV8R/sounds/805908/) | CVLTIV8R | CC0 1.0 |
| `impact_metal` | [Large Metal Lift Door Closing.wav](https://freesound.org/people/lolamadeus/sounds/161230/) | lolamadeus | CC0 1.0 |
| `latch` | [Latch.wav](https://freesound.org/people/tec_studio/sounds/223981/) | tec_studio | CC0 1.0 |
| `latch` | [Door_Metal_Latch_Shut_3Feet.wav](https://freesound.org/people/pointparkcinema/sounds/391065/) | pointparkcinema | CC BY 4.0 |
| `latch` | [Squeaky Appliance Door](https://freesound.org/people/Nightflame/sounds/397603/) | Nightflame | CC0 1.0 |
| `latch` | [Hatch/Door Close](https://freesound.org/people/JustInvoke/sounds/446110/) | JustInvoke | CC BY 4.0 |
| `motor` | [Motor Whir.wav](https://freesound.org/people/jesabat/sounds/119739/) | jesabat | CC BY 3.0 |
| `motor` | [Electric skateboard 02](https://freesound.org/people/Eelke/sounds/451546/) | Eelke | CC BY 4.0 |
| `motor` | [DikaiPumpSystem-2023-06-02a.wav](https://freesound.org/people/kr15h/sounds/689263/) | kr15h | CC0 1.0 |
| `motor` | [Motorized recliner](https://freesound.org/people/Fwughox_Official/sounds/749885/) | Fwughox_Official | CC0 1.0 |
| `motor` | [COMCam-Blue Snowball Microphone, MCU_Fuji Instax Mini 9 Camera, Shutter, Take Picture, No Dispense, Servo Motor_Nicholas Judy_TDC](https://freesound.org/people/designerschoice/sounds/805374/) | designerschoice | CC BY 4.0 |
| `rattle` | [toolbox.wav](https://freesound.org/people/j1987/sounds/532137/) | j1987 | CC0 1.0 |
| `release` | [Renault Master 28dTi Foley Hood Open Close Mono.wav](https://freesound.org/people/Soundholder/sounds/425840/) | Soundholder | CC BY 3.0 |
| `release` | [Filing Cabinet Drawer Open/Close](https://freesound.org/people/SamsterBirdies/sounds/581404/) | SamsterBirdies | CC0 1.0 |
| `release`, `seal` | [starwars_bad_batch_clonewars_inspired_blast_door_open_and_close_sound_02102026](https://freesound.org/people/Artninja/sounds/844867/) | Artninja | CC BY 4.0 |
| `rolling_soil` | [Bike tire on dirt road.wav](https://freesound.org/people/SpliceSound/sounds/218301/) | SpliceSound | CC0 1.0 |
| `rolling_soil` | [gravel road](https://freesound.org/people/seth-m/sounds/341069/) | seth-m | CC0 1.0 |
| `scrape` | [concrete1.mp3](https://freesound.org/people/Faceonmars/sounds/106258/) | Faceonmars | CC0 1.0 |
| `scrape` | [Metal Tool Drag1.aif](https://freesound.org/people/timgormly/sounds/170962/) | timgormly | CC BY 4.0 |
| `scrape` | [MetalScrape-Piezo2.wav](https://freesound.org/people/eyenorth/sounds/464850/) | eyenorth | CC BY 4.0 |
| `scrape` | [Metal Scraping.wav](https://freesound.org/people/Riaan3ignite/sounds/543086/) | Riaan3ignite | CC BY 4.0 |
| `scrape` | [ScrapingMetal.wav](https://freesound.org/people/JamGrif/sounds/682644/) | JamGrif | CC0 1.0 |
| `scrape` | [SWORD OR SICKLE DRAGGING ON CONCRETE SEVERAL VERSIONS.wav](https://freesound.org/people/metrostock99/sounds/704956/) | metrostock99 | CC0 1.0 |
| `scrape_soil` | [cleats scuffling small rocks 2](https://freesound.org/people/oscaraudiogeek/sounds/328932/) | oscaraudiogeek | CC0 1.0 |
| `scrape_soil` | [Dragging Cardboard Box](https://freesound.org/people/IENBA/sounds/864763/) | IENBA | CC0 1.0 |
| `seal` | [Hatch Seal.wav](https://freesound.org/people/Paul368/sounds/264063/) | Paul368 | CC0 1.0 |
| `seal` | [beerhiss1.wav](https://freesound.org/people/deleted_user_228014/sounds/387239/) | deleted_user_228014 | CC BY 3.0 |
| `seal` | [starwars_inspired_andor_interrogation_room_door_opening_and_closing_sound_06262025](https://freesound.org/people/Artninja/sounds/813566/) | Artninja | CC BY 4.0 |
| `spray` | [gravel impact 3](https://freesound.org/people/jorickhoofd/sounds/169557/) | jorickhoofd | CC BY 4.0 |
| `spray` | [Falling In Dirt.wav](https://freesound.org/people/FocusBay/sounds/210867/) | FocusBay | CC0 1.0 |
| `spray` | [auto car or van stop brake skid gravel short.flac](https://freesound.org/people/kyles/sounds/637161/) | kyles | CC0 1.0 |
| `spray` | [EXT_Rocks_Impact_Short.WAV](https://freesound.org/people/Triad330670/sounds/696519/) | Triad330670 | CC BY 4.0 |
| `spray` | [bassy_small_fate_stay_night_impact_jump_or_land_sound_11212025](https://freesound.org/people/Artninja/sounds/834347/) | Artninja | CC BY 4.0 |
| `strut` | [Pneumatic grease bomb](https://freesound.org/people/prnkys/sounds/437205/) | prnkys | CC BY 3.0 |
| `strut` | [Futuristic Drill Machine](https://freesound.org/people/IENBA/sounds/611356/) | IENBA | CC0 1.0 |
| `strut` | [Hydraulic Truck Brakes](https://freesound.org/people/Filmscore/sounds/823069/) | Filmscore | CC0 1.0 |
| `tear` | [medium_snappybends.wav](https://freesound.org/people/NoiseCollector/sounds/67386/) | NoiseCollector | CC BY 3.0 |
| `tear` | [Metal Bullet Ricochets](https://freesound.org/people/mikiko850/sounds/854335/) | mikiko850 | CC BY 4.0 |
| `tick` | [rubbing_squishy_ball_on_metal01.wav](https://freesound.org/people/wjoojoo/sounds/275704/) | wjoojoo | CC BY 4.0 |
| `tick` | [toaster_on_fridge.wav](https://freesound.org/people/Günne/sounds/350846/) | Günne | CC0 1.0 |
| `tick` | [metal contraction rattle click.wav](https://freesound.org/people/martian/sounds/613135/) | martian | CC0 1.0 |
| `tick` | [Maquina_de_escribirRoyal_Front_Hit_Trump_Hot](https://freesound.org/people/byronabadia/sounds/724738/) | byronabadia | CC BY 4.0 |
| `tick` | [Squeaky metal screwed 01](https://freesound.org/people/xkeril/sounds/742600/) | xkeril | CC0 1.0 |
| `tick` | [Metal gate opening and closing](https://freesound.org/people/ThabzMalik/sounds/767058/) | ThabzMalik | CC0 1.0 |

## Clicks

Recordings under `Sound/Clicks/`, all Creative Commons 0 (public domain) on Freesound. Each is a cut of the source's high-quality preview (48 kHz mono FLAC, a high-pass at 80 Hz - 300 Hz for the interface's - short fades, peak -1 dBFS).

| Files | Used for | Source | Author |
| --- | --- | --- | --- |
| `switch_on_1..4.flac`, `switch_off_1..4.flac` | a cockpit's switches and knobs (one way, then the other) | [Small Toggle Switch](https://freesound.org/people/loganzsound/sounds/877305/) (on: 0.609, 1.809, 3.009, 5.409 s; off: 0.855, 2.056, 3.241, 5.650 s; 180 ms each) | loganzsound |
| `button_1..4.flac` | MFD buttons | [Button Switch Toggle Operation Click](https://freesound.org/people/DustyWind/sounds/700985/) (0.152, 1.628, 2.399, 3.140 s; 200 ms each) | DustyWind |
| `ui_1..3.flac` | the launchpad's and the menus' controls | [Switch Click](https://freesound.org/people/adamdavidsmith/sounds/877181/) (1.976, 0.425, 3.782 s; 80 ms each) | adamdavidsmith |
