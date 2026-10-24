# Copyright (c) Martin Schweiger
# Licensed under the MIT License
#
# AMD FidelityFX SDK (MIT): the FSR 3 upscaler with the Vulkan backend.
# The needed part of the SDK is fetched (sparse checkout of a fixed
# release) unless FFX_SDK_DIR points to a copy. The shader permutation
# headers are generated with glslangValidator (tools/ffx_gen_shaders.py)
# instead of the SDK's Windows-only shader compiler.
#
# Defines the target ffx_fsr3_vk and sets FFX_FOUND.

set(FFX_FOUND OFF)
set(FFX_TAG v1.1.4)
set(FFX_URL https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git)

if(NOT FFX_SDK_DIR)
	set(FFX_SDK_DIR ${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk)
	if(NOT EXISTS ${FFX_SDK_DIR}/sdk/src/components/fsr3upscaler)
		find_package(Git QUIET)
		if(NOT GIT_FOUND)
			message(WARNING "FidelityFX SDK: git not found, building without FSR")
			return()
		endif()
		message(STATUS "Fetching FidelityFX SDK ${FFX_TAG} (FSR 3 upscaler subset)")
		file(REMOVE_RECURSE ${FFX_SDK_DIR})
		execute_process(COMMAND ${GIT_EXECUTABLE} clone --quiet --filter=blob:none --no-checkout --depth 1 --branch ${FFX_TAG} ${FFX_URL} ${FFX_SDK_DIR}
			RESULT_VARIABLE res)
		if(res EQUAL 0)
			execute_process(COMMAND ${GIT_EXECUTABLE} -C ${FFX_SDK_DIR} sparse-checkout set --no-cone
				/LICENSE.txt /sdk/LICENSE.txt /sdk/include/ /sdk/src/shared/ /sdk/src/components/fsr3upscaler/
				/sdk/src/backends/shared/ /sdk/src/backends/vk/ffx_vk.cpp /sdk/src/backends/vk/shaders/fsr3upscaler/
				RESULT_VARIABLE res)
		endif()
		if(res EQUAL 0)
			execute_process(COMMAND ${GIT_EXECUTABLE} -C ${FFX_SDK_DIR} checkout --quiet ${FFX_TAG} RESULT_VARIABLE res)
		endif()
		if(NOT res EQUAL 0)
			message(WARNING "FidelityFX SDK: fetch failed, building without FSR")
			file(REMOVE_RECURSE ${FFX_SDK_DIR})
			return()
		endif()
	endif()
endif()

set(FFX_SDK ${FFX_SDK_DIR}/sdk)

# The opaque context storage is sized for Windows, where wchar_t (used in
# resource names inside the context) has 2 bytes instead of 4: double it.
set(FFX_TYPES_H ${FFX_SDK}/include/FidelityFX/host/ffx_types.h)
file(READ ${FFX_TYPES_H} ffx_types)
string(REPLACE "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128)" "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)" ffx_types_patched "${ffx_types}")
if(NOT ffx_types STREQUAL ffx_types_patched)
	file(WRITE ${FFX_TYPES_H} "${ffx_types_patched}")
endif()

# The GLSL callbacks declare the luma history (an RGBA16F image) as rgba8.
set(FFX_GLSL_CB ${FFX_SDK}/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h)
file(READ ${FFX_GLSL_CB} ffx_cb)
string(REPLACE "FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8)" "FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f)" ffx_cb_patched "${ffx_cb}")
if(NOT ffx_cb STREQUAL ffx_cb_patched)
	file(WRITE ${FFX_GLSL_CB} "${ffx_cb_patched}")
endif()

# shader permutations: HDR input, render-size motion vectors (unjittered),
# inverted depth; with and without sharpening. Only the reference Lanczos
# reprojection: with the lookup-table variant the history blurs (the table
# texture does not come through the backend's upload correctly here).
set(FFX_PERMUTATIONS 0x16,0x36)
set(FFX_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/ffx_shaders)
file(GLOB FFX_PASS_SHADERS ${FFX_SDK}/src/backends/vk/shaders/fsr3upscaler/*.glsl)
file(GLOB FFX_GPU_HEADERS ${FFX_SDK}/include/FidelityFX/gpu/*.h ${FFX_SDK}/include/FidelityFX/gpu/fsr3upscaler/*.h)
set(FFX_GEN_HEADERS)
foreach(shader ${FFX_PASS_SHADERS})
	get_filename_component(name ${shader} NAME_WE)
	foreach(variant "" _wave64 _16bit _wave64_16bit)
		list(APPEND FFX_GEN_HEADERS ${FFX_GEN_DIR}/${name}${variant}_permutations.h)
	endforeach()
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
	OUTPUT ${FFX_GEN_HEADERS}
	COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/ffx_gen_shaders.py ${GLSLANG_CMD} ${FFX_SDK} ${FFX_GEN_DIR} ${FFX_PERMUTATIONS}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/ffx_gen_shaders.py ${FFX_PASS_SHADERS} ${FFX_GPU_HEADERS} ${GLSLANG_DEP}
	COMMENT "Generating FSR 3 shader permutations"
)

add_library(ffx_fsr3_vk STATIC
	${FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
	${FFX_SDK}/src/shared/ffx_assert.cpp
	${FFX_SDK}/src/shared/ffx_message.cpp
	${FFX_SDK}/src/shared/ffx_object_management.cpp
	${FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp
	${FFX_SDK}/src/backends/vk/ffx_vk.cpp
	${FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp
	${FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ffx/ffx_stubs.cpp
	${FFX_GEN_HEADERS}
)
target_compile_definitions(ffx_fsr3_vk PRIVATE FFX_FSR3UPSCALER)
target_include_directories(ffx_fsr3_vk PUBLIC ${FFX_SDK}/include)
target_include_directories(ffx_fsr3_vk PRIVATE ${FFX_SDK}/src/shared ${FFX_SDK}/src/components ${FFX_SDK}/src/backends/shared ${FFX_GEN_DIR})
if(NOT MSVC)
	# The Vulkan backend places its structures in the scratch memory at
	# offsets aligned only to 4 bytes, so stores the compiler merges into
	# aligned vector stores would fault: no auto-vectorisation.
	target_compile_options(ffx_fsr3_vk PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/ffx/ffx_posix_compat.h -w
		-fno-tree-vectorize -fno-tree-slp-vectorize)
endif()
set_target_properties(ffx_fsr3_vk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ffx_fsr3_vk PUBLIC Vulkan::Vulkan)
set(FFX_FOUND ON)
