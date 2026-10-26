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
	Shaders/sky.vert
	Shaders/sky.frag
	Shaders/shadow.vert
	Shaders/shadow_alpha.vert
	Shaders/shadow_alpha.frag
	Shaders/shadow_inst.vert
	Shaders/tile_mesh.comp
	Shaders/shadow_impostor_inst.vert
	Shaders/mesh_inst.vert
	Shaders/prepass_inst.vert
	Shaders/impostor_inst.vert
	Shaders/prepass_impostor_inst.vert
	Shaders/post.vert
	Shaders/bloom_down.frag
	Shaders/bloom_up.frag
	Shaders/tonemap.frag
	Shaders/histogram.comp
	Shaders/adapt.comp
	Shaders/prepass.vert
	Shaders/prepass.frag
	Shaders/ao.comp
	Shaders/aoblur.comp
	Shaders/taa.comp
	Shaders/atm_transmittance.comp
	Shaders/atm_multiscat.comp
	Shaders/cloudnoise.comp
	Shaders/cloudmap.vert
	Shaders/cloudmap.frag
	Shaders/clouds.comp
	Shaders/cloudcomp.frag
	Shaders/envfilter.comp
	Shaders/prepass_tile.frag
	Shaders/prepass_tile.vert
	Shaders/ssr_hiz.comp
	Shaders/ssr_trace.comp
	Shaders/ssr_trace_rt.comp
	Shaders/rtlight_trace.comp
	Shaders/rtlight_temporal.comp
	Shaders/rtlight_filter.comp
	Shaders/ssr_resolve.comp
	Shaders/ssr_temporal.comp
	Shaders/plume.vert
	Shaders/plume.frag
	Shaders/shimmer.frag
	Shaders/scorch.vert
	Shaders/scorch.frag
	Shaders/particle_emit.comp
	Shaders/particle_update.comp
	Shaders/particle_update_rt.comp
	Shaders/particle_light.comp
	Shaders/particle_light_rt.comp
	Shaders/particle_shadow_splat.comp
	Shaders/particle_shadow_sum.comp
	Shaders/particle_sortkeys.comp
	Shaders/particle_sort.comp
	Shaders/smoke_splat.comp
	Shaders/smoke_resolve.comp
	Shaders/smoke_march.frag
	Shaders/smoke_thick.frag
	Shaders/smoke_blur.comp
	Shaders/smoke_screen.frag
	Shaders/gpuparticle.vert
	Shaders/gpuparticle.frag
	Shaders/ring.vert
	Shaders/ring.frag
	Shaders/ringshadow.frag
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
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader} ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/atmosphere.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/shadow.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/frame.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/lights.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/brdf.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/exposure.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/atmlut.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/material.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ssr.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ssr_trace.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/rt.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/rtlight.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/plume.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/particles.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/particle_update.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/particle_common.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/particle_light.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/surfdetail.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/tile_main.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/terraindetail.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/detailcore.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/smokevol.glsl ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ring.glsl ${CMAKE_CURRENT_SOURCE_DIR}/EmbedFile.cmake ${GLSLANG_DEP}
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
	LegacyTex.cpp
	AO.cpp
	SSR.cpp
	RayTracing.cpp
	RTLighting.cpp
	TAA.cpp
	Clouds.cpp
	GroundTracks.cpp
	EnvProbe.cpp
	Fsr.cpp
	StarSphere.cpp
	Draw2D.cpp
	Sketchpad.cpp
	Fonts.cpp
	Particles.cpp
	Plumes.cpp
	TileArchive.cpp
	PlanetSurface.cpp
	Atmosphere.cpp
	Shadows.cpp
	PostFX.cpp
	MaterialConfig.cpp
	Input.cpp
	ScatterAssets.cpp
	ScatterField.cpp
	TestInput.cpp
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

# AMD FSR 3 upscaler (FidelityFX SDK, MIT)
option(ORBITER_VK_FSR "Build the AMD FSR 3 upscaler into the Vulkan client" ON)
if(ORBITER_VK_FSR)
	include(ffx/FidelityFX.cmake)
	if(FFX_FOUND)
		target_link_libraries(VulkanClient PRIVATE ffx_fsr3_vk)
		target_compile_definitions(VulkanClient PRIVATE VKC_HAVE_FSR=1)
	endif()
endif()

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
