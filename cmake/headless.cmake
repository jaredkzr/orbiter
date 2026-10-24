# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Helpers for the headless (non-Windows) build.

# Plugin modules are built as Windows DLLs: CMake places them in their
# RUNTIME_OUTPUT_DIRECTORY, and Orbiter loads them by their bare names
# (e.g. Modules\Celbody\Earth.dll). For ELF shared objects, apply the same
# location via LIBRARY_OUTPUT_DIRECTORY and drop the "lib" name prefix, so
# that the platform layer finds them as Modules/Celbody/Earth.so.
# Modules may depend on other modules (e.g. Earth.so on Vsop87.so) and on
# libraries in the Orbiter root directory; Windows finds those via the PATH
# set up by Orbiter, ELF objects via their RUNPATH.
function(orbiter_fixup_modules dir)
	get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
	foreach(tgt ${targets})
		get_target_property(type ${tgt} TYPE)
		get_target_property(not_a_module ${tgt} ORBITER_NOT_A_MODULE)
		if(not_a_module)
			continue()
		endif()
		if(type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
			get_target_property(rtdir ${tgt} RUNTIME_OUTPUT_DIRECTORY)
			if(rtdir)
				set_target_properties(${tgt} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${rtdir})
			endif()
			set_target_properties(${tgt} PROPERTIES
				PREFIX ""
				INSTALL_RPATH "$ORIGIN;$ORIGIN/..;$ORIGIN/../.."
			)
		endif()
	endforeach()
	get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
	foreach(subdir ${subdirs})
		orbiter_fixup_modules(${subdir})
	endforeach()
endfunction()
