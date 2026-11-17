if(ORBITER_BUILD_XRSOUND)
	add_subdirectory(XRSound)
elseif(ORBITER_HEADLESS)
	# XRSound's API on Orbiter's own sound (Src/Orbiter/Audio/XRSoundShim.cpp):
	# its header for add-ons, its cfgs and recordings (the callouts, chatter),
	# and Orbiter's new voice lines (Sound/Voice)
	set(XR_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/XRSound/assets/XRSound)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/XRSound/src/XRSound.h ${ORBITER_BINARY_SDK_DIR}/XRSound/XRSound.h COPYONLY)
	file(GLOB xr_cfg ${XR_ASSETS}/*.cfg)
	file(GLOB voice CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Voice/*.flac ${CMAKE_CURRENT_SOURCE_DIR}/Voice/*.txt ${CMAKE_CURRENT_SOURCE_DIR}/Voice/*.cfg)
	file(GLOB ground CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Voice/Ground/*.flac ${CMAKE_CURRENT_SOURCE_DIR}/Voice/Ground/*.txt ${CMAKE_CURRENT_SOURCE_DIR}/Voice/Ground/*.cfg)
	# the contacts' takes (Src/Orbiter/Audio/ContactVoices.cpp, SampleBank.cpp):
	# a folder per layer, the manifest contacts.txt, their sources SOURCES.md
	# (globbed only so that new takes reconfigure: the folder is copied whole)
	file(GLOB_RECURSE contacts CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Contacts/*)
	file(GLOB hrtf ${CMAKE_CURRENT_SOURCE_DIR}/HRTF/*.hrtf)
	file(GLOB clicks ${CMAKE_CURRENT_SOURCE_DIR}/Clicks/*.flac)
	# the generated sound effects (Utils/Audio/make_sfx.py: the cabin's air, its events, beds)
	file(GLOB generated CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Generated/*.flac ${CMAKE_CURRENT_SOURCE_DIR}/Generated/*.txt)
	add_custom_target(XRSound_assets ALL
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/XRSound
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${xr_cfg} ${XR_ASSETS}/ReadMe.txt ${ORBITER_BINARY_ROOT_DIR}/XRSound/
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${XR_ASSETS}/Default ${ORBITER_BINARY_ROOT_DIR}/XRSound/Default
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${voice} ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice/
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice/Ground
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ground} ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice/Ground/
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/Contacts ${ORBITER_BINARY_ROOT_DIR}/Sound/Contacts
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/HRTF
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${hrtf} ${ORBITER_BINARY_ROOT_DIR}/Sound/HRTF/
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/Clicks
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${clicks} ${ORBITER_BINARY_ROOT_DIR}/Sound/Clicks/
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/Generated
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${generated} ${ORBITER_BINARY_ROOT_DIR}/Sound/Generated/
		VERBATIM   # (file names with spaces and apostrophes: "Warning Docked Ship's Outer Door is Closed")
	)
	install(DIRECTORY ${ORBITER_BINARY_ROOT_DIR}/XRSound DESTINATION ${ORBITER_INSTALL_ROOT_DIR})
	install(DIRECTORY ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice ${ORBITER_BINARY_ROOT_DIR}/Sound/Contacts ${ORBITER_BINARY_ROOT_DIR}/Sound/HRTF ${ORBITER_BINARY_ROOT_DIR}/Sound/Clicks ${ORBITER_BINARY_ROOT_DIR}/Sound/Generated DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Sound)
	install(FILES ${ORBITER_BINARY_SDK_DIR}/XRSound/XRSound.h DESTINATION ${ORBITER_INSTALL_SDK_DIR}/XRSound)
endif()
