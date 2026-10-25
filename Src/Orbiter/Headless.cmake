# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Headless Orbiter executable for non-Windows platforms.
# Builds the simulation core without graphics client, launchpad, Win32
# dialogs or DirectInput. Platform-specific replacements live in Src/Platform.

FetchContent_MakeAvailable(imgui)
FetchContent_MakeAvailable(implot)
find_package(Threads REQUIRED)

set(PLATFORM_DIR ${CMAKE_SOURCE_DIR}/Src/Platform)

set(headless_src
# General source files
	Astro.cpp
	Camera.cpp
	cmdline.cpp
	Config.cpp
	console_ng.cpp
	Element.cpp
	elevmgr.cpp
	Help.cpp
	Keymap.cpp
	LightEmitter.cpp
	Mesh.cpp
	Nav.cpp
	Orbiter.cpp
	PlaybackEd.cpp
	Psys.cpp
	Script.cpp
	Shadow.cpp
	State.cpp
	Vecmat.cpp
	VectorMap.cpp
	ConsoleManager.cpp
	TimeData.cpp
# Dialogs (ImGui based)
	DlgCamera.cpp
	DlgCapture.cpp
	DlgFocus.cpp
	DlgFunction.cpp
	DlgHelp.cpp
	DlgInfo.cpp
	DlgMap.cpp
	DlgMenuCfg.cpp
	DlgMgr.cpp
	DlgOptions.cpp
	DlgRecorder.cpp
	DlgTacc.cpp
	Select.cpp
# Body classes
	Body.cpp
	BodyIntegrator.cpp
	PinesGrav.cpp
	Celbody.cpp
	Planet.cpp
	Rigidbody.cpp
	Star.cpp
# Vessel classes
	FlightRecorder.cpp
	SuperVessel.cpp
	ContactSolver.cpp
	CollisionHull.cpp
	VesselCollision.cpp
	Vessel.cpp
	Vesselbase.cpp
	Vesselstatus.cpp
# Surface base classes
	Base.cpp
	Baseobj.cpp
# Cockpit classes
	Defpanel.cpp
	hud.cpp
	MenuInfoBar.cpp
	Pane.cpp
	Panel.cpp
	Panel2D.cpp
	VCockpit.cpp
# MFD classes
	Mfd.cpp
	MfdAlign.cpp
	MfdComms.cpp
	MfdDocking.cpp
	MfdHsi.cpp
	MfdLanding.cpp
	MfdMap.cpp
	MfdMap_old.cpp
	MfdOrbit.cpp
	MfdSurface.cpp
	MfdSync.cpp
	MfdTransfer.cpp
	MfdUser.cpp
# API implementations
	CamAPI.cpp
	CelSphereAPI.cpp
	DrawAPI.cpp
	GraphicsAPI.cpp
	MFDAPI.cpp
	ModuleAPI.cpp
	OrbiterAPI.cpp
# Utils
	D3dmath.cpp
	Log.cpp
	Memstat.cpp
	Util.cpp
	ZTreeMgr.cpp
# Platform layer
	${PLATFORM_DIR}/posix/InputPosix.cpp
	${PLATFORM_DIR}/posix/OrbiterPosix.cpp
	${PLATFORM_DIR}/posix/PosixPlatform.cpp
)

# The simulation core is a shared library, so that plugin modules and tests
# can link against the Orbiter API (on Windows, they link Orbiter.exe's
# import library instead).
add_library(OrbiterCore SHARED ${headless_src})

target_include_directories(OrbiterCore PUBLIC
	${PLATFORM_DIR}/posix/include
	${PLATFORM_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/Orbitersdk/include
	${CMAKE_SOURCE_DIR}/OVP
	${CMAKE_CURRENT_BINARY_DIR}
	${TRACY_CLIENT_INCLUDE}
	${imgui_SOURCE_DIR}
	${implot_SOURCE_DIR}
)

target_compile_definitions(OrbiterCore PRIVATE NOGRAPHICS)

target_link_libraries(OrbiterCore
	PRIVATE
	Orbitersdk
	lua::lib
	zlib
	${CMAKE_DL_LIBS}
	Threads::Threads
	${TRACY_CLIENT}
)

set_target_properties(OrbiterCore PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	INSTALL_RPATH "$ORIGIN"
	ORBITER_NOT_A_MODULE ON
	FOLDER Core
)

add_executable(Orbiter ${PLATFORM_DIR}/posix/OrbiterMain.cpp)

target_link_libraries(Orbiter OrbiterCore)

set_target_properties(Orbiter PROPERTIES
	INSTALL_RPATH "$ORIGIN"
	RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	OUTPUT_NAME Orbiter
	FOLDER Core
)

install(TARGETS Orbiter OrbiterCore
	RUNTIME DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
	LIBRARY DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)

string(TIMESTAMP DATE "%d %b %Y")
string(TIMESTAMP VERSION "%y%m%d")
configure_file(about.hpp.in about.hpp @ONLY)
