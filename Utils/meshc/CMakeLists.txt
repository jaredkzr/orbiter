# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_executable(meshc
	meshc.cpp
	Mesh.cpp
)

target_include_directories(meshc
	PUBLIC ${ORBITER_SOURCE_DIR}
)

if(ORBITER_HEADLESS)
	target_include_directories(meshc PRIVATE
		${CMAKE_SOURCE_DIR}/Src/Platform/posix/include
		${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	)
endif()

set_target_properties(meshc
	PROPERTIES
	FOLDER Tools
)

install(TARGETS meshc
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/Utils
)
