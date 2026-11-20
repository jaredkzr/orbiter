# Copyright (c) Martin Schweiger
# Licensed under the MIT License


if(SPICE_BUILD)
    add_subdirectory(SPICE)
    add_subdirectory(Atmosphere)
else()
    add_subdirectory(Sol)
    add_subdirectory(Vsop87)
    add_subdirectory(Moon)
    add_subdirectory(Phobos)
    add_subdirectory(Deimos)
    add_subdirectory(Vesta)
    add_subdirectory(Galsat)
    add_subdirectory(Satsat)
    add_subdirectory(Miranda)
    add_subdirectory(Ariel)
    add_subdirectory(Umbriel)
    add_subdirectory(Titania)
    add_subdirectory(Oberon)
    add_subdirectory(Triton)
    add_subdirectory(Proteus)
    add_subdirectory(Nereid)
endif()



# The bodies' configuration files (their bases among them) are copied into the
# build tree by a pre-build step of each body's module, which runs only when
# that module is linked again: a changed base file alone stayed behind. This
# target brings them up to date on every build (only the files that differ).
if(SPICE_BUILD)
	set(celbody_cfg_dirs ${CMAKE_CURRENT_SOURCE_DIR}/SPICE/Config)
else()
	file(GLOB celbody_cfg_dirs
		${CMAKE_CURRENT_SOURCE_DIR}/*/Config
		${CMAKE_CURRENT_SOURCE_DIR}/*/*/Config
	)
	list(FILTER celbody_cfg_dirs EXCLUDE REGEX "/SPICE/")
	list(SORT celbody_cfg_dirs)
endif()
set(celbody_cfg_cmds)
foreach(dir ${celbody_cfg_dirs})
	list(APPEND celbody_cfg_cmds
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${dir} ${CMAKE_BINARY_DIR}/Config)
endforeach()
add_custom_target(CelbodyConfig ALL
	${celbody_cfg_cmds}
	COMMENT "Bodies' configuration files"
	VERBATIM
)
