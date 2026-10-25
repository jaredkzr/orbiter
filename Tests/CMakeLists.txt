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

endif()
