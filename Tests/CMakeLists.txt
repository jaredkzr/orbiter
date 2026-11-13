Include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
)

# Link the test framework statically (BUILD_SHARED_LIBS is enabled globally by
# cmake/tracy.cmake). Catch2WithMain provides main(), which must be part of the
# test executable.
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(Catch2)

# Utility function
function(add_test_file test_name)
	add_executable(${test_name} "${test_name}.cpp")

	set_target_properties( ${test_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}" )
	if (ORBITER_HEADLESS)
		set_target_properties( ${test_name} PROPERTIES INSTALL_RPATH "$ORIGIN" )
	endif()

	target_include_directories(${test_name}
		PRIVATE ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
		PRIVATE ${MODULE_COMMON_DIR}
		PRIVATE ${ORBITER_SOURCE_ROOT_DIR}/Src/Module/LuaScript/LuaInterpreter
	)

	target_link_libraries(${test_name}
		${ORBITER_LIB}
		${ORBITER_SDK_LIB}
		lua::lib
		LuaInterpreter
		Catch2::Catch2WithMain
	)

	add_dependencies(${test_name}
		${OrbiterTgt}
		Orbitersdk
		LuaInterpreter
	)

	add_test( 
		NAME ${test_name} 
		COMMAND $<TARGET_FILE:${test_name}>
		WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	)
	set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
endfunction()

# Register unit tests
add_test_file(Lua.Interpreter)

# Convex geometry for collisions: compiled into the test (no Orbiter
# dependencies but the vector maths)
add_executable(Collision.Geometry Collision.Geometry.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/ConvexGeometry.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/ConvexDecomposition.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Collision.Geometry PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Collision.Geometry Catch2::Catch2WithMain)
set_target_properties(Collision.Geometry PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Collision.Geometry COMMAND $<TARGET_FILE:Collision.Geometry>)
set_tests_properties(Collision.Geometry PROPERTIES TIMEOUT 60)

# The GPU particle pool's bookkeeping (CPU only, no Vulkan)
add_executable(Particles.Pool Particles.Pool.cpp
	${CMAKE_SOURCE_DIR}/OVP/VulkanClient/ParticlePool.cpp)
target_include_directories(Particles.Pool PRIVATE ${CMAKE_SOURCE_DIR}/OVP/VulkanClient)
target_link_libraries(Particles.Pool Catch2::Catch2WithMain)
set_target_properties(Particles.Pool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Particles.Pool COMMAND $<TARGET_FILE:Particles.Pool>)
set_tests_properties(Particles.Pool PROPERTIES TIMEOUT 60)

# Conics of transfer planning (Lambert, departure hyperbola, B-plane)
add_executable(Transfer.Conics Transfer.Conics.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Conics.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Maneuver.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Transfer.Conics PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Transfer.Conics Catch2::Catch2WithMain)
set_target_properties(Transfer.Conics PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Transfer.Conics COMMAND $<TARGET_FILE:Transfer.Conics>)
set_tests_properties(Transfer.Conics PROPERTIES TIMEOUT 60)

# The external camera's geometry (frames, pitch limit, trackball, flyby)
add_executable(Camera.Rig Camera.Rig.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/CamRig.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Camera.Rig PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Camera.Rig Catch2::Catch2WithMain)
set_target_properties(Camera.Rig PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Camera.Rig COMMAND $<TARGET_FILE:Camera.Rig>)
set_tests_properties(Camera.Rig PROPERTIES TIMEOUT 60)

# Gravity-assist routes (patched conics, optimiser; the Cassini1 benchmark)
add_executable(Transfer.Routes Transfer.Routes.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Routes.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Conics.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Maneuver.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Transfer.Routes PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Transfer.Routes Catch2::Catch2WithMain)
set_target_properties(Transfer.Routes PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Transfer.Routes COMMAND $<TARGET_FILE:Transfer.Routes>)
set_tests_properties(Transfer.Routes PROPERTIES TIMEOUT 300)

# Special relativity of relativistic flight (boosts, light, motion, trips)
add_executable(Relativity.Kinematics Relativity.Kinematics.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Relativity.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Relativity.Kinematics PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Relativity.Kinematics Catch2::Catch2WithMain)
set_target_properties(Relativity.Kinematics PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Relativity.Kinematics COMMAND $<TARGET_FILE:Relativity.Kinematics>)
set_tests_properties(Relativity.Kinematics PROPERTIES TIMEOUT 120)

# The sea's swell (shared by the physics and the client)
add_executable(Water.Swell Water.Swell.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/SeaSwell.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Water.Swell PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Water.Swell Catch2::Catch2WithMain)
set_target_properties(Water.Swell PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Water.Swell COMMAND $<TARGET_FILE:Water.Swell>)
set_tests_properties(Water.Swell PROPERTIES TIMEOUT 60)

# What a body shows the water: the union of its hulls, its panels
add_executable(Water.Volume Water.Volume.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/HullWater.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/ConvexGeometry.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Water.Volume PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Water.Volume Catch2::Catch2WithMain)
set_target_properties(Water.Volume PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Water.Volume COMMAND $<TARGET_FILE:Water.Volume>)
set_tests_properties(Water.Volume PROPERTIES TIMEOUT 60)

# The water's forces and a body's motion in it
add_executable(Water.Hydro Water.Hydro.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Hydro.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/HullWater.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/ConvexGeometry.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Vecmat.cpp)
target_include_directories(Water.Hydro PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Water.Hydro Catch2::Catch2WithMain)
set_target_properties(Water.Hydro PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Water.Hydro COMMAND $<TARGET_FILE:Water.Hydro>)
set_tests_properties(Water.Hydro PROPERTIES TIMEOUT 120)

# Rotating black holes: Kerr spacetime, geodesics, observers
add_executable(Kerr.Geodesics Kerr.Geodesics.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Kerr.cpp
)
target_include_directories(Kerr.Geodesics PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Kerr.Geodesics Catch2::Catch2WithMain)
set_target_properties(Kerr.Geodesics PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Kerr.Geodesics COMMAND $<TARGET_FILE:Kerr.Geodesics>)
set_tests_properties(Kerr.Geodesics PROPERTIES TIMEOUT 120)

# The audio engine, offline (no device): buses, volumes, fades, WAV files
find_package(Threads REQUIRED)
add_executable(Audio.Engine Audio.Engine.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/AudioEngine.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Ears.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/WavWriter.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Miniaudio.cpp)
target_include_directories(Audio.Engine PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter ${CMAKE_SOURCE_DIR}/Extern/miniaudio)
target_link_libraries(Audio.Engine Catch2::Catch2WithMain ${CMAKE_DL_LIBS} Threads::Threads m)
set_target_properties(Audio.Engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Audio.Engine COMMAND $<TARGET_FILE:Audio.Engine>)
set_tests_properties(Audio.Engine PROPERTIES TIMEOUT 60)

# How sound travels: the air, travel time, Doppler, the shock cone
add_executable(Audio.Propagation Audio.Propagation.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Medium.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Track.cpp)
target_include_directories(Audio.Propagation PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter)
target_link_libraries(Audio.Propagation Catch2::Catch2WithMain)
set_target_properties(Audio.Propagation PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Audio.Propagation COMMAND $<TARGET_FILE:Audio.Propagation>)
set_tests_properties(Audio.Propagation PROPERTIES TIMEOUT 60)

# The synthesizers: levels, spectra, booms, rocket noise
add_executable(Audio.Synth Audio.Synth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/AudioEngine.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Ears.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/PathNode.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Synth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/RocketSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/RcsSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/WindSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/BoomSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/ContactSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/BlastSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/FireSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/SplashSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/ThunderSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/CreakSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/AmbienceSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/MusicSynth.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/WavWriter.cpp
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Audio/Miniaudio.cpp)
target_compile_definitions(Audio.Synth PRIVATE AUDIO_SYNTH_TEST)
target_include_directories(Audio.Synth PRIVATE ${CMAKE_SOURCE_DIR}/Src/Orbiter ${CMAKE_SOURCE_DIR}/Extern/miniaudio)
target_link_libraries(Audio.Synth Catch2::Catch2WithMain ${CMAKE_DL_LIBS} Threads::Threads m)
set_target_properties(Audio.Synth PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}")
add_test(NAME Audio.Synth COMMAND $<TARGET_FILE:Audio.Synth> WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}) # (Sound/Contacts: the contacts' recordings)
set_tests_properties(Audio.Synth PROPERTIES TIMEOUT 120)

# Scenario tests need an Orbiter executable that runs without graphics client
if (ORBITER_HEADLESS)
	set(ORBITER_TEST_EXE $<TARGET_FILE:${OrbiterTgt}>)
elseif (BUILD_ORBITER_SERVER)
	set(ORBITER_TEST_EXE $<TARGET_FILE:Orbiter_server>)
endif()

if (ORBITER_TEST_EXE)

	# Sanity check for scenario tests
	add_test(
		NAME "Scenario.SanityCheck"
		COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Scenarios/Delta-glider/Smack!.scn" "--maxframes=60"
		WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	)
	set_tests_properties(Scenario.SanityCheck PROPERTIES TIMEOUT 60)

	# Register scenario tests (with a fixed time step: repeatable, and not
	# bound to real time)
	file(GLOB TestScenarios "${CMAKE_SOURCE_DIR}/Scenarios/Tests/*.scn")
	foreach(Scenario ${TestScenarios})
		get_filename_component(test_name ${Scenario} NAME_WE)
		add_test(
			NAME "Scenario.${test_name}"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${Scenario}" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Scenario.${test_name} PROPERTIES TIMEOUT 60)
	endforeach()
	# (the slope test needs the slope of the elevation data, without procedural
	# detail, and no boulders in the way)
	set_tests_properties(Scenario.ContactSlopeTest PROPERTIES ENVIRONMENT "ORBITER_NO_TERRAIN_DETAIL=1;ORBITER_NO_SCATTER=1")
	# (every pad of every base: and the bases' collision hulls are made anew
	# where their files changed, which is minutes on the first run)
	set_tests_properties(Scenario.BasePadsTest PROPERTIES TIMEOUT 600)
	# transfer planning: searches and targeting, flights to the Moon and to Mars
	set_tests_properties(Scenario.TransferPlanTest Scenario.TransferFlightTest PROPERTIES TIMEOUT 180)
	set_tests_properties(Scenario.TransferMarsFlightTest PROPERTIES TIMEOUT 400)
	# gravity assists: route search, full-gravity targeting, suggestions
	set_tests_properties(Scenario.RoutePlanTest PROPERTIES TIMEOUT 180)
	set_tests_properties(Scenario.RouteFlightTest PROPERTIES TIMEOUT 1000)
	# relativistic flight: the ideal drive, clocks, impacts at speed, prediction
	set_tests_properties(Scenario.RelFlightTest PROPERTIES TIMEOUT 120)
	set_tests_properties(Scenario.RelTripTest PROPERTIES TIMEOUT 400)
	# interstellar flight: deep space, ship-time warp, a trip to a star
	set_tests_properties(Scenario.InterstellarTest PROPERTIES TIMEOUT 400)
	# black holes: orbit, hover and fall in a Kerr spacetime
	set_tests_properties(Scenario.BlackHoleTest PROPERTIES TIMEOUT 300)
	set_tests_properties(Scenario.BlackHoleTidesTest PROPERTIES TIMEOUT 120 ENVIRONMENT "ORBITER_DAMAGE=1")
	# the Solar System's neighbours: stars of the catalogue, a binary's orbit, the origin in another system
	set_tests_properties(Scenario.SystemsTest PROPERTIES TIMEOUT 200)
	# exoplanets: their orbits, a transit, tidal locking, the procedural ground
	set_tests_properties(Scenario.ExoplanetTest PROPERTIES TIMEOUT 200)
	# saving and loading damage: the save test writes the scenario the load
	# test runs (in the build tree)
	set_tests_properties(Scenario.DamageSaveTest PROPERTIES FIXTURES_SETUP DamageRoundtrip ENVIRONMENT "ORBITER_DAMAGE=1")
	add_test(
		NAME "Scenario.DamageLoadTest"
		COMMAND ${ORBITER_TEST_EXE} "--scenariox=${ORBITER_BINARY_ROOT_DIR}/Scenarios/Tests/DamageRoundtrip.scn" "--fixedstep=0.02"
		WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	)
	set_tests_properties(Scenario.DamageLoadTest PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED DamageRoundtrip)
	# real water: a calm sea (no swell) for the floating test
	set_tests_properties(Scenario.WaterFloatTest PROPERTIES ENVIRONMENT "ORBITER_SEA_WIND=0;ORBITER_DAMAGE=0")
	set_tests_properties(Scenario.WaterEntryTest PROPERTIES ENVIRONMENT "ORBITER_DAMAGE=0")
	set_tests_properties(Scenario.WaterSinkTest PROPERTIES TIMEOUT 120 ENVIRONMENT "ORBITER_DAMAGE=1")
	set_tests_properties(Scenario.WaterWarpTest PROPERTIES TIMEOUT 120 ENVIRONMENT "ORBITER_DAMAGE=0")
	# (the destruction test needs the damage model on)
	set_tests_properties(Scenario.DestructionTest Scenario.FocusLossTest PROPERTIES ENVIRONMENT "ORBITER_DAMAGE=1")

	# Sound rendered offline from a session (ORBITER_AUDIO_RENDER), then
	# checked by Tests/Audio/analyze.py: 100 steps of 0.02 s, 2 s of sound
	find_package(Python3 COMPONENTS Interpreter)
	if (Python3_FOUND)
		add_test(
			NAME "Audio.Render"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Scenarios/Delta-glider/Smack!.scn" "--maxframes=100" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Render PROPERTIES TIMEOUT 60 FIXTURES_SETUP AudioRender
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Render.wav;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		add_test(
			NAME "Audio.RenderCheck"
			COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/Tests/Audio/analyze.py "${ORBITER_BINARY_ROOT_DIR}/Audio.Render.wav"
				--duration 2.0 --silent
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.RenderCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioRender)

		# Sound through the air (Tests/Audio/Flyby*.scn, Script/Tests/AudioFlyby.lua):
		# a 440 Hz tone on a delta-glider flown past a listener on the sea.
		# Mach 0.5, 100 m up, overhead at 17.647 s: heard at twice the pitch
		# coming (from the left), two thirds going (to the right), 440 Hz
		# when the sound from overhead arrives, 100 m / 340 m/s later.
		# Mach 2, 1000 m up, overhead at 20 s: silence until the shock cone
		# arrives (22.57 s: 1748 m behind it), a boom then.
		set(AUDIO_ANALYZE ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/Tests/Audio/analyze.py)
		foreach(pass Sub Super)
			add_test(
				NAME "Audio.Flyby${pass}"
				COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Flyby${pass}.scn" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Audio.Flyby${pass} PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioFlyby${pass}
				ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Flyby${pass}.wav;ORBITER_AUDIO_TEST_TONE=SRC;ORBITER_AUDIO_VESSELS=0;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		endforeach()
		set_tests_properties(Audio.FlybySuper PROPERTIES PASS_REGULAR_EXPRESSION "Audio: boom from test tone on SRC at 22\\.5[0-9]")
		add_test(
			NAME "Audio.FlybySubCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.FlybySub.wav" --duration 36.0 --no-clip --no-clicks
				--freq 1:8:860:885 --freq 10:14:860:885 --freq 29:34:290:296
				--cross 440:17.941:0.03:200:1000
				--channel-balance 10:14:-16:-8 --channel-balance 29:34:8:16
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.FlybySubCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioFlybySub)
		add_test(
			NAME "Audio.FlybySuperCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.FlybySuper.wav" --duration 45.0 --no-clip --no-clicks
				--level 0.5:22.45:all:-inf:-90 --level 22.7:24:all:-60:-20
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.FlybySuperCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioFlybySuper)

		# The cockpit and the vacuum (Tests/Audio/Cockpit.scn, Script/Tests/AudioCockpit.lua):
		# a tone on a delta-glider in orbit, 3 s each: from the cockpit (through
		# the hull), outside (silent when physical, heard when cinematic), the
		# cockpit at 20x (faded out), at 1x (heard again)
		foreach(realism Physical Cinematic)
			add_test(
				NAME "Audio.Cockpit${realism}"
				COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Cockpit.scn" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Audio.Cockpit${realism} PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioCockpit${realism}
				ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Cockpit${realism}.wav;ORBITER_AUDIO_TEST_TONE=SRC;ORBITER_AUDIO_VESSELS=0;ORBITER_AUDIO_REALISM=${realism};ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		endforeach()
		add_test(
			NAME "Audio.CockpitPhysicalCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.CockpitPhysical.wav" --duration 12.0 --no-clip --no-clicks
				--level 0.5:2.8:all:-40:-10 --level 3.5:5.8:all:-inf:-90 --level 6.5:8.8:all:-inf:-90 --level 9.6:11.8:all:-40:-10
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.CockpitPhysicalCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioCockpitPhysical)
		add_test(
			NAME "Audio.CockpitCinematicCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.CockpitCinematic.wav" --duration 12.0 --no-clip --no-clicks
				--level 0.5:2.8:all:-40:-10 --level 3.5:5.8:all:-45:-15 --level 6.5:8.8:all:-inf:-90 --level 9.6:11.8:all:-40:-10
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.CockpitCinematicCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioCockpitCinematic)

		# A vessel's own sounds (VesselSounds): a delta-glider's main engines on
		# KSC's runway heard from 300 m - silent until the roar arrives (full at
		# 2 s, 0.88 s on the way), loud, low; a delta-glider lifting off on the
		# Moon - silent outside, its engines heard in the cockpit
		foreach(scene Takeoff MoonLiftoff)
			add_test(
				NAME "Audio.${scene}"
				COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/${scene}.scn" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Audio.${scene} PROPERTIES TIMEOUT 120 FIXTURES_SETUP Audio${scene}
				ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.${scene}.wav;ORBITER_AUDIO_REALISM=physical;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		endforeach()
		add_test(
			NAME "Audio.TakeoffCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Takeoff.wav" --duration 12.0 --no-clip
				--level 0.5:2.8:all:-inf:-90 --level 5:11:all:-35:-3 --level 5:11:low:-40:-3
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.TakeoffCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioTakeoff)
		add_test(
			NAME "Audio.MoonLiftoffCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.MoonLiftoff.wav" --duration 12.0 --no-clip
				--level 0.5:5.8:all:-inf:-90 --level 6.5:11.5:all:-40:-6
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.MoonLiftoffCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioMoonLiftoff)

		# Contacts (ContactSynth, Vessel::SoundContacts; off in the game for now,
		# ORBITER_AUDIO_CONTACTS=1 here): a delta-glider set down
		# on KSC's runway, heard from 40 m - silent until it touches down (1.28 s),
		# the touchdown's thump, rolling; set down on its belly - the scrape, then
		# silence when it has stopped. Docking (Scenarios/Tests/DockingTest): the
		# clunk of A1's docking.
		foreach(scene Land Belly)
			add_test(
				NAME "Audio.Contacts${scene}"
				COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Contacts${scene}.scn" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Audio.Contacts${scene} PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioContacts${scene}
				ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Contacts${scene}.wav;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_AUDIO_CONTACTS=1")
		endforeach()
		set_tests_properties(Audio.ContactsLand PROPERTIES PASS_REGULAR_EXPRESSION "Audio: touchdown blow of LAND at 1\\.[2-3]")
		add_test(
			NAME "Audio.ContactsLandCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.ContactsLand.wav" --duration 12.0 --no-clip
				--level 0.3:1.25:all:-inf:-90 --level 1.3:1.6:low:-75:-30 --level 2:5:all:-65:-30
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.ContactsLandCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioContactsLand)
		add_test(
			NAME "Audio.ContactsBellyCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.ContactsBelly.wav" --duration 12.0 --no-clip
				--level 1.25:2:all:-75:-35 --level 4:11:all:-inf:-90
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.ContactsBellyCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioContactsBelly)
		# The sound API (oapiAudio*, Lua's audio table; Script/Tests/AudioApi):
		# sound files on a vessel and the interface, looped, re-pitched,
		# stopped; tune keys; the DG's nose cone's actuator and latch
		file(COPY ${CMAKE_SOURCE_DIR}/Sound/XRSound/assets/XRSound/Default/BeepHigh.wav DESTINATION ${ORBITER_BINARY_ROOT_DIR}/Sound/Tests)
		file(RENAME ${ORBITER_BINARY_ROOT_DIR}/Sound/Tests/BeepHigh.wav ${ORBITER_BINARY_ROOT_DIR}/Sound/Tests/beep.wav)
		add_test(
			NAME "Audio.Api"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Api.scn" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Api PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioApi
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Api.wav;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_AUDIO_CONTACTS=1")
		add_test(
			NAME "Audio.ApiCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Api.wav" --duration 29.0 --no-clip
				--level 0.5:0.8:all:-60:-5 --level 1.2:1.9:all:-inf:-80 --level 2.0:2.3:all:-60:-5
				--level 5.3:5.9:all:-inf:-80 --level 8:24:120-200:-80:-20 --level 27:29:all:-inf:-80
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.ApiCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioApi)
		add_test(
			NAME "Audio.Docking"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Scenarios/Tests/DockingTest.scn" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Docking PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Audio: docking of A1"
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Docking.wav;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_AUDIO_CONTACTS=1")
		# an explosion's blast (Effects, BlastSynth): a fuelled DG breaking up
		# 1 km away - silent until its blast arrives, 1001 m / 340 m/s later
		# (frames capped: a script that didn't load would run on)
		add_test(
			NAME "Audio.Blast"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Blast.scn" "--fixedstep=0.02" "--maxframes=600"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Blast PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioBlast PASS_REGULAR_EXPRESSION "Audio: blast heard at 4\\.[45][0-9]* s, 100[0-9] m"
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Blast.wav;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=1;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		add_test(
			NAME "Audio.BlastCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Blast.wav" --duration 8.0 --no-clip
				--level 0.5:4.3:all:-inf:-50 --level 4.45:4.7:all:-30:0 --level 4.45:5.5:low:-40:0
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.BlastCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioBlast)
		# water (Effects, SplashSynth): the water entry test heard - a ShuttlePB
		# dropped flat at 9 m/s, a delta-glider ditching at 70 m/s: one entry
		# each, at their speeds into the water
		add_test(
			NAME "Audio.Splash"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Scenarios/Tests/WaterEntryTest.scn" "--fixedstep=0.02" "--maxframes=4000"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Splash PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioSplash PASS_REGULAR_EXPRESSION "DROP into the water at .* 9\\.[0-9] m/s"
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Splash.wav;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		add_test(
			NAME "Audio.SplashCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Splash.wav" --no-clip --no-clicks --level 0.1:2.0:all:-60:-10
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.SplashCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioSplash)
		# thunder (Effects, ThunderSynth; Lua audio.lightning): a channel 2.8 km
		# off at its nearest - silent until its first sound arrives (8.3 s),
		# then rolling, low
		add_test(
			NAME "Audio.Thunder"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Thunder.scn" "--fixedstep=0.02" "--maxframes=1200"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Thunder PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioThunder PASS_REGULAR_EXPRESSION "Audio: thunder heard at 9\\.[2-4]"
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Thunder.wav;ORBITER_AUDIO_LOG=1;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		add_test(
			NAME "Audio.ThunderCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Thunder.wav" --duration 20.0 --no-clip
				--level 0.5:9.0:all:-inf:-60 --level 9.2:12:all:-50:0 --level 9.2:14:high:-inf:-60
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.ThunderCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioThunder)
		# the structure creaking (Effects, CreakSynth): a delta-glider pulling up
		# hard 4-8 s, its cockpit - creaks as the load rises and falls, none in
		# steady flight
		add_test(
			NAME "Audio.Creak"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Creak.scn" "--fixedstep=0.02" "--maxframes=700"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Creak PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Audio: creak at [5-7]\\."
			FAIL_REGULAR_EXPRESSION "Audio: (creak|groan) at [1-3]\\."
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Creak.wav;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		# the radio (Radio): the Shuttle's liftoff answered by the ground, a
		# light's round trip and a moment later
		add_test(
			NAME "Audio.Radio"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Ascent.scn" "--fixedstep=0.02" "--maxframes=1500"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Radio PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Audio: radio at ([3-9]|1[0-2])\\.[0-9]+ s: Sound/Voice/Ground/Liftoff"
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Radio.wav;ORBITER_AUDIO_LOG=1;ORBITER_AUDIO_CALLOUTS=1;ORBITER_AUDIO_AMBIENCE=0;ORBITER_DAMAGE=0")
		# the voice's callouts (a takeoff, a descent with the gear up, Mach 1)
		# and XRSound's API on Orbiter's sound (Lua's xrsound)
		add_test(
			NAME "Audio.Callouts"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Callouts.scn" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Callouts PROPERTIES TIMEOUT 300 FIXTURES_SETUP AudioCallouts
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Callouts.wav;ORBITER_AUDIO_CALLOUTS=1;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_DAMAGE=0")
		add_test(
			NAME "Audio.CalloutsCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Callouts.wav" --no-clip --no-clicks
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.CalloutsCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioCallouts)
		# the music follows the vessel in focus: ground, its breakup
		# (silence), orbit, an approach, deep space, reentry
		add_test(
			NAME "Audio.Music"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Music.scn" "--fixedstep=0.02" "--maxframes=2000"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Music PROPERTIES TIMEOUT 300 FIXTURES_SETUP AudioMusic PASS_REGULAR_EXPRESSION "AudioMusic: all PASS"
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Music.wav;ORBITER_AUDIO_MUSIC=1;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=1;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0")
		add_test(
			NAME "Audio.MusicCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.Music.wav" --no-clip --no-clicks
				--level 18:21:all:-45:-20 --level 24:27:all:-50:-20
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.MusicCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioMusic)
		add_test(
			NAME "Audio.Ascent"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/Ascent.scn" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.Ascent PROPERTIES TIMEOUT 300
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Ascent.wav;ORBITER_AUDIO_CALLOUTS=1;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_DAMAGE=0")
		add_test(
			NAME "Audio.XRSound"
			COMMAND ${ORBITER_TEST_EXE} "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Audio/XRSound.scn" "--fixedstep=0.02"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.XRSound PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioXRSound
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.XRSound.wav;ORBITER_AUDIO_CALLOUTS=0;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_DAMAGE=0")
		add_test(
			NAME "Audio.XRSoundCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.XRSound.wav" --duration 10.0 --no-clip
				--level 0.3:0.9:all:-inf:-80 --level 1.0:1.3:all:-60:-5 --level 2.2:2.9:all:-60:-5
				--level 3.15:3.45:all:-inf:-80 --level 5.4:5.9:all:-inf:-80 --level 6.0:6.3:all:-60:-5
				--level 7.3:8.9:all:-inf:-80
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.XRSoundCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioXRSound)
	endif()

	# Launchpad (in the Vulkan client's window, rendered offscreen; needs a
	# Vulkan device): a scenario launched twice, returning to the launchpad.
	# (These tests leave out the simulated sky, ORBITER_VK_SKY=0: its data,
	# Config/CSphere/Sky, are built locally by Utils/SkyKit.)
	if (ORBITER_HEADLESS AND ORBITER_BUILD_VULKANCLIENT)
		add_test(
			NAME "Launchpad.Cycle"
			COMMAND ${ORBITER_TEST_EXE} "--maxframes=30"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Launchpad.Cycle PROPERTIES TIMEOUT 120
			ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_SKY=0;ORBITER_LAUNCHPAD_AUTO=Delta-glider/Smack!;ORBITER_LAUNCHPAD_RUNS=2"
			PASS_REGULAR_EXPRESSION "Launchpad: session 2 closed")

		# Scripted mouse and keyboard input (Tests/Input/*.input, see
		# OVP/VulkanClient/TestInput.h): the map view and the instant replay
		file(GLOB InputTests "${CMAKE_SOURCE_DIR}/Tests/Input/*.input")
		foreach(InputTest ${InputTests})
			get_filename_component(test_name ${InputTest} NAME_WE)
			# the scene: <test>.scn if there is one, else Orbit.scn
			set(InputScene "${CMAKE_SOURCE_DIR}/Tests/Input/Orbit.scn")
			if (EXISTS "${CMAKE_SOURCE_DIR}/Tests/Input/${test_name}.scn")
				set(InputScene "${CMAKE_SOURCE_DIR}/Tests/Input/${test_name}.scn")
			endif()
			add_test(
				NAME "Input.${test_name}"
				COMMAND ${ORBITER_TEST_EXE} -p VulkanClient "--scenariox=${InputScene}" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Input.${test_name} PROPERTIES TIMEOUT 120
				ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_SKY=0;ORBITER_MFD_WINDOWS=0;ORBITER_VK_TEST_INPUT=${InputTest}")
		endforeach()

		# Sound in the instant replay (Tests/Audio/Replay.input): a burn heard
		# from the cockpit, and again when the replay plays it
		if (Python3_FOUND)
			add_test(
				NAME "Audio.Replay"
				COMMAND ${ORBITER_TEST_EXE} -p VulkanClient "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Input/Orbit.scn" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Audio.Replay PROPERTIES TIMEOUT 120 FIXTURES_SETUP AudioReplay
				ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_SKY=0;ORBITER_MFD_WINDOWS=0;ORBITER_VK_TEST_INPUT=${CMAKE_SOURCE_DIR}/Tests/Audio/Replay.input;ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Replay.wav;ORBITER_AUDIO_AMBIENCE=0;ORBITER_AUDIO_RADIO=0;ORBITER_AUDIO_CALLOUTS=0")
			add_test(
				NAME "Audio.ReplayCheck"
				COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/Tests/Audio/analyze.py "${ORBITER_BINARY_ROOT_DIR}/Audio.Replay.wav"
					--level 0.3:0.9:all:-inf:-90 --level 1.5:3.5:all:-50:-5 --level 9.0:9.9:all:-inf:-60 --level 11.5:13.5:all:-50:-5 --level 16:17.5:all:-inf:-60
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Audio.ReplayCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioReplay)
		endif()

		# Remote control for playtesting (OVP/VulkanClient/RemoteControl.h,
		# Utils/Playtest/orbctl.py): a session driven over its socket
		find_package(Python3 COMPONENTS Interpreter)
		if (Python3_FOUND)
			add_test(
				NAME "Playtest.Remote"
				COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/Playtest/remote_test.py
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Playtest.Remote PROPERTIES TIMEOUT 300
				ENVIRONMENT "ORBITER_VK_SKY=0;ORBITER_BUILD=${ORBITER_BINARY_ROOT_DIR}")
		endif()

		# The simulated sky (needs its data, Config/CSphere/Sky, else left
		# out): the bake's brightness in some directions against reference
		# values (Tests/Sky/reference.txt), the zodiacal light against the
		# measured
		if (EXISTS "${ORBITER_BINARY_ROOT_DIR}/Config/CSphere/Sky/stars.bin")
			add_test(
				NAME "Sky.Check"
				COMMAND ${ORBITER_TEST_EXE} -p VulkanClient "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Sky/Sky.scn" "--maxframes=5"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Sky.Check PROPERTIES TIMEOUT 120
				ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_SKY_CACHE=0;ORBITER_VK_SKY_CHECK=${CMAKE_SOURCE_DIR}/Tests/Sky/reference.txt"
				PASS_REGULAR_EXPRESSION "sky check: [0-9]+ of [0-9]+ passed"
				FAIL_REGULAR_EXPRESSION "FAILED")
		endif()
		# the sky through a black hole: the client's ray tracer against the
		# core's Kerr geodesics
		add_test(
			NAME "Lens.Check"
			COMMAND ${ORBITER_TEST_EXE} -p VulkanClient "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Lens/Lens.scn" "--maxframes=40"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Lens.Check PROPERTIES TIMEOUT 120
			ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_LENS_CHECK=30;ORBITER_VK_SKY_OBSERVER=10,0,0"
			PASS_REGULAR_EXPRESSION "lens check at r = [0-9.]+ M: .* ok"
			FAIL_REGULAR_EXPRESSION "FAILED")
		# Nyx's donor star where the scene draws it (light-time, aberration)
		# and where the lens trace sees it: at rest relative to the hole, and
		# moving with the star (0.07 c: the relativistic view)
		foreach(mode Rest Comoving)
			add_test(
				NAME "Lens.Donor${mode}"
				COMMAND ${ORBITER_TEST_EXE} -p VulkanClient "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Lens/Donor.scn" "--maxframes=40"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set(donor_env "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_LENS_DONOR_CHECK=30")
			if (mode STREQUAL "Comoving")
				list(APPEND donor_env "DONOR_COMOVE=1")
			endif()
			set_tests_properties(Lens.Donor${mode} PROPERTIES TIMEOUT 120
				ENVIRONMENT "${donor_env}"
				PASS_REGULAR_EXPRESSION "donor check at [0-9.]+ M: .* ok"
				FAIL_REGULAR_EXPRESSION "FAILED")
		endforeach()
	endif()

endif()
