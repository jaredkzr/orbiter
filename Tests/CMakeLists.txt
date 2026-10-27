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
	# (the destruction test needs the damage model on)
	set_tests_properties(Scenario.DestructionTest PROPERTIES ENVIRONMENT "ORBITER_DAMAGE=1")

	# Launchpad (in the Vulkan client's window, rendered offscreen; needs a
	# Vulkan device): a scenario launched twice, returning to the launchpad
	if (ORBITER_HEADLESS AND ORBITER_BUILD_VULKANCLIENT)
		add_test(
			NAME "Launchpad.Cycle"
			COMMAND ${ORBITER_TEST_EXE} "--maxframes=30"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Launchpad.Cycle PROPERTIES TIMEOUT 120
			ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_LAUNCHPAD_AUTO=Delta-glider/Smack!;ORBITER_LAUNCHPAD_RUNS=2"
			PASS_REGULAR_EXPRESSION "Launchpad: session 2 closed")

		# Scripted mouse and keyboard input (Tests/Input/*.input, see
		# OVP/VulkanClient/TestInput.h): the map view and the instant replay
		file(GLOB InputTests "${CMAKE_SOURCE_DIR}/Tests/Input/*.input")
		foreach(InputTest ${InputTests})
			get_filename_component(test_name ${InputTest} NAME_WE)
			add_test(
				NAME "Input.${test_name}"
				COMMAND ${ORBITER_TEST_EXE} -p VulkanClient "--scenariox=${CMAKE_SOURCE_DIR}/Tests/Input/Orbit.scn" "--fixedstep=0.02"
				WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
			)
			set_tests_properties(Input.${test_name} PROPERTIES TIMEOUT 120
				ENVIRONMENT "ORBITER_VK_OFFSCREEN=1;ORBITER_VK_TEST_INPUT=${InputTest}")
		endforeach()
	endif()

endif()
