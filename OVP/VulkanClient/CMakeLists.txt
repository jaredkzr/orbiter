# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Vulkan graphics client (cross-platform, SDL3 windowing and input)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})

find_package(Vulkan REQUIRED)

# SDL3: use the system package if available, otherwise fetch it
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
	include(FetchContent)
	FetchContent_Declare(SDL3
		GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
		GIT_TAG release-3.2.10
	)
	set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(SDL3)
endif()

# Vulkan Memory Allocator (header-only)
include(FetchContent)
FetchContent_Declare(VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG v3.2.1
)
FetchContent_GetProperties(VulkanMemoryAllocator)
if(NOT vulkanmemoryallocator_POPULATED)
	FetchContent_Populate(VulkanMemoryAllocator)
endif()

FetchContent_MakeAvailable(imgui)
FetchContent_MakeAvailable(implot)

# --------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V, embedded into the module

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
if(GLSLANG_VALIDATOR)
	set(GLSLANG_CMD ${GLSLANG_VALIDATOR})
else()
	message(STATUS "glslangValidator not found: building glslang from source")
	FetchContent_Declare(glslang
		GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
		GIT_TAG 15.1.0
	)
	set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
	set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
	set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
	set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(glslang)
	set(GLSLANG_CMD $<TARGET_FILE:glslang-standalone>)
	set(GLSLANG_DEP glslang-standalone)
endif()

set(SHADER_SRC
	Shaders/mesh.vert
	Shaders/mesh.frag
	Shaders/stars.vert
	Shaders/stars.frag
	Shaders/draw2d.vert
	Shaders/draw2d.frag
	Shaders/particle.vert
	Shaders/particle.frag
	Shaders/tile.vert
	Shaders/tile.frag
)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
foreach(shader ${SHADER_SRC})
	get_filename_component(name ${shader} NAME)
	string(REPLACE "." "_" symbol ${name})
	set(spv ${SHADER_OUT_DIR}/${name}.spv)
	set(hdr ${SHADER_OUT_DIR}/${name}.h)
	add_custom_command(
		OUTPUT ${hdr}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
		COMMAND ${GLSLANG_CMD} -V --target-env vulkan1.3 -o ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
		COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DSYMBOL=spv_${symbol} -P ${CMAKE_CURRENT_SOURCE_DIR}/EmbedFile.cmake
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader} ${CMAKE_CURRENT_SOURCE_DIR}/EmbedFile.cmake ${GLSLANG_DEP}
		COMMENT "Compiling shader ${name}"
	)
	list(APPEND SHADER_HEADERS ${hdr})
endforeach()

# --------------------------------------------------------------------------

add_library(VulkanClient SHARED
	VulkanClient.cpp
	VkContext.cpp
	VkTexture.cpp
	VkMesh.cpp
	Scene.cpp
	VObjects.cpp
	StarSphere.cpp
	Draw2D.cpp
	Sketchpad.cpp
	Fonts.cpp
	Particles.cpp
	TileArchive.cpp
	PlanetSurface.cpp
	Input.cpp
	ImageIO.cpp
	Vma.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
	${SHADER_HEADERS}
)

target_include_directories(VulkanClient PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	${SHADER_OUT_DIR}
	${vulkanmemoryallocator_SOURCE_DIR}/include
	${imgui_SOURCE_DIR}
	${imgui_SOURCE_DIR}/backends
	${implot_SOURCE_DIR}
)

target_link_libraries(VulkanClient PRIVATE
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	SDL3::SDL3
	Vulkan::Vulkan
	zlib
)

add_dependencies(VulkanClient ${OrbiterTgt} Orbitersdk)

set_target_properties(VulkanClient PROPERTIES
	FOLDER OVP
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR}
	INSTALL_RPATH "$ORIGIN;$ORIGIN/..;$ORIGIN/../.."
)

# ImGui fonts used by the Orbiter dialogs
add_custom_command(TARGET VulkanClient POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_ROOT_DIR}/Fonts
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
		${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf
		${CMAKE_SOURCE_DIR}/Extern/imgui/fa-solid-900.ttf
		${CMAKE_SOURCE_DIR}/Extern/imgui/Lekton-Bold.ttf
		${CMAKE_SOURCE_DIR}/Extern/imgui/architext.regular.ttf
		${ORBITER_BINARY_ROOT_DIR}/Fonts
)

install(TARGETS VulkanClient
	RUNTIME DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
	LIBRARY DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)
