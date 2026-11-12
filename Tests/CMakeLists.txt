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
			ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Render.wav")
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
				ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Flyby${pass}.wav;ORBITER_AUDIO_TEST_TONE=SRC;ORBITER_AUDIO_LOG=1;ORBITER_DAMAGE=0")
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
				ENVIRONMENT "ORBITER_AUDIO_RENDER=${ORBITER_BINARY_ROOT_DIR}/Audio.Cockpit${realism}.wav;ORBITER_AUDIO_TEST_TONE=SRC;ORBITER_AUDIO_REALISM=${realism};ORBITER_DAMAGE=0")
		endforeach()
		add_test(
			NAME "Audio.CockpitPhysicalCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.CockpitPhysical.wav" --duration 12.0 --no-clip --no-clicks
				--level 0.5:2.8:all:-30:-6 --level 3.5:5.8:all:-inf:-90 --level 6.5:8.8:all:-inf:-90 --level 9.6:11.8:all:-30:-6
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.CockpitPhysicalCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioCockpitPhysical)
		add_test(
			NAME "Audio.CockpitCinematicCheck"
			COMMAND ${AUDIO_ANALYZE} "${ORBITER_BINARY_ROOT_DIR}/Audio.CockpitCinematic.wav" --duration 12.0 --no-clip --no-clicks
				--level 0.5:2.8:all:-30:-6 --level 3.5:5.8:all:-40:-15 --level 6.5:8.8:all:-inf:-90 --level 9.6:11.8:all:-30:-6
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Audio.CockpitCinematicCheck PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED AudioCockpitCinematic)
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
