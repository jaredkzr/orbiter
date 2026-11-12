if(ORBITER_BUILD_XRSOUND)
	add_subdirectory(XRSound)
elseif(ORBITER_HEADLESS)
	# XRSound's API on Orbiter's own sound (Src/Orbiter/Audio/XRSoundShim.cpp):
	# its header for add-ons, its cfgs and recordings (the callouts, chatter),
	# and Orbiter's new voice lines (Sound/Voice)
	set(XR_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/XRSound/assets/XRSound)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/XRSound/src/XRSound.h ${ORBITER_BINARY_SDK_DIR}/XRSound/XRSound.h COPYONLY)
	file(GLOB xr_cfg ${XR_ASSETS}/*.cfg)
	file(GLOB voice ${CMAKE_CURRENT_SOURCE_DIR}/Voice/*.flac ${CMAKE_CURRENT_SOURCE_DIR}/Voice/*.txt)
	# the contacts' recordings (Src/Orbiter/Audio/ContactSynth.cpp)
	file(GLOB contacts ${CMAKE_CURRENT_SOURCE_DIR}/Contacts/*.flac)
	add_custom_target(XRSound_assets ALL
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/XRSound
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${xr_cfg} ${XR_ASSETS}/ReadMe.txt ${ORBITER_BINARY_ROOT_DIR}/XRSound/
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${XR_ASSETS}/Default ${ORBITER_BINARY_ROOT_DIR}/XRSound/Default
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${voice} ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice/
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Sound/Contacts
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${contacts} ${ORBITER_BINARY_ROOT_DIR}/Sound/Contacts/
	)
	install(DIRECTORY ${ORBITER_BINARY_ROOT_DIR}/XRSound DESTINATION ${ORBITER_INSTALL_ROOT_DIR})
	install(DIRECTORY ${ORBITER_BINARY_ROOT_DIR}/Sound/Voice ${ORBITER_BINARY_ROOT_DIR}/Sound/Contacts DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Sound)
	install(FILES ${ORBITER_BINARY_SDK_DIR}/XRSound/XRSound.h DESTINATION ${ORBITER_INSTALL_SDK_DIR}/XRSound)
endif()
