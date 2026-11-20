# Copyright (c) Martin Schweiger
# Licensed under the MIT License
#
# AMD FSR 4 (FSR Upscaling 4.1.2), an opt-in upscaler beside FSR 3.1.
#
# AMD ships FSR 4 for Vulkan only as signed Windows DLLs (FidelityFX SDK
# 2.4: Kits/FidelityFX/signedbin, listed among the MIT-licensed files of
# the SDK's license.md): no source, no Linux library. They are run in the
# client's own process by ffx-vk-linux (MIT), a PE loader with stand-ins
# for the Windows functions they call. Both are fetched at fixed versions
# into the build tree (nothing of them is in this repository) and deployed
# to Modules/Plugin/FSR4: libffx_vk_linux.so, the two DLLs and their
# licences. The client opens the library at run time, when FSR 4 is chosen
# (Fsr4.cpp): without these files it has FSR 3.1 as before.
#
# Linux on x86-64 only. Needs OpenSSL, libdrm_amdgpu, the Vulkan registry
# (vk.xml) and the SPIRV-Tools library with its headers (Debian/Ubuntu:
# spirv-tools-dev, spirv-headers; or their packages unpacked into
# <build>/_deps/spirv-tools-dev: dpkg -x). Missing: FSR 4 is left out.
#
# Sets FSR4_FOUND; defines the target ffx_vk_linux and FSR4_INCLUDE_DIRS.

set(FSR4_FOUND OFF)
set(FSR4_LOADER_URL https://github.com/DuIslingr/ffx-vk-linux.git)
set(FSR4_LOADER_COMMIT e1d5a415ed78dd5db7c6d00258d06acccb424307)
# (AMD's DLLs of that release: checked when fetched, and by the client before it loads them)
set(FSR4_LOADER_DLL_SHA256 38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9)
set(FSR4_UPSCALER_DLL_SHA256 4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7)
set(FSR4_SDK_TAG v2.4.0)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
	message(STATUS "FSR 4: Linux on x86-64 only, left out")
	return()
endif()

# what the loader needs
set(FSR4_DEPS_HINT ${CMAKE_BINARY_DIR}/_deps/spirv-tools-dev/usr)
find_package(OpenSSL QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
	pkg_check_modules(FSR4_DRM QUIET IMPORTED_TARGET libdrm libdrm_amdgpu)
endif()
find_library(FSR4_SPIRV_TOOLS NAMES SPIRV-Tools HINTS ${FSR4_DEPS_HINT}/lib ${FSR4_DEPS_HINT}/lib/x86_64-linux-gnu)
find_path(FSR4_SPIRV_TOOLS_INC spirv-tools/libspirv.h HINTS ${FSR4_DEPS_HINT}/include)
find_path(FSR4_SPIRV_HEADERS_INC spirv/unified1/spirv.hpp11 HINTS ${FSR4_DEPS_HINT}/include)
find_file(FSR4_VK_XML vk.xml PATHS ${Vulkan_INCLUDE_DIR}/../share/vulkan/registry /usr/share/vulkan/registry NO_DEFAULT_PATH)
find_package(Git QUIET)
set(missing)
if(NOT OpenSSL_FOUND)
	list(APPEND missing "OpenSSL")
endif()
if(NOT FSR4_DRM_FOUND)
	list(APPEND missing "libdrm_amdgpu")
endif()
if(NOT FSR4_SPIRV_TOOLS OR NOT FSR4_SPIRV_TOOLS_INC OR NOT FSR4_SPIRV_HEADERS_INC)
	list(APPEND missing "SPIRV-Tools (spirv-tools-dev, spirv-headers)")
endif()
if(NOT FSR4_VK_XML)
	list(APPEND missing "vk.xml")
endif()
if(NOT GIT_FOUND)
	list(APPEND missing "git")
endif()
if(missing)
	message(STATUS "FSR 4: left out, missing: ${missing}")
	return()
endif()

# the loader's source, at a fixed commit
set(FSR4_LOADER_DIR ${CMAKE_BINARY_DIR}/_deps/ffx-vk-linux)
if(NOT EXISTS ${FSR4_LOADER_DIR}/src/ffxvk.cpp)
	message(STATUS "Fetching ffx-vk-linux ${FSR4_LOADER_COMMIT} (FSR 4's loader)")
	file(REMOVE_RECURSE ${FSR4_LOADER_DIR})
	execute_process(COMMAND ${GIT_EXECUTABLE} clone --quiet ${FSR4_LOADER_URL} ${FSR4_LOADER_DIR} RESULT_VARIABLE res)
	if(res EQUAL 0)
		execute_process(COMMAND ${GIT_EXECUTABLE} -C ${FSR4_LOADER_DIR} -c advice.detachedHead=false checkout --quiet ${FSR4_LOADER_COMMIT} RESULT_VARIABLE res)
	endif()
	if(NOT res EQUAL 0)
		message(WARNING "FSR 4: fetching its loader failed, left out")
		file(REMOVE_RECURSE ${FSR4_LOADER_DIR})
		return()
	endif()
endif()

# AMD's DLLs and their licence, from the SDK's release
set(FSR4_SDK_DIR ${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk-2.4)
set(FSR4_SIGNED ${FSR4_SDK_DIR}/Kits/FidelityFX/signedbin)
if(NOT EXISTS ${FSR4_SIGNED}/amd_fidelityfx_upscaler_vk.dll)
	message(STATUS "Fetching FidelityFX SDK ${FSR4_SDK_TAG} (FSR 4's signed Vulkan DLLs, 36 MB)")
	file(REMOVE_RECURSE ${FSR4_SDK_DIR})
	execute_process(COMMAND ${GIT_EXECUTABLE} clone --quiet --filter=blob:none --no-checkout --depth 1 --branch ${FSR4_SDK_TAG} ${FFX_URL} ${FSR4_SDK_DIR}
		RESULT_VARIABLE res)
	if(res EQUAL 0)
		execute_process(COMMAND ${GIT_EXECUTABLE} -C ${FSR4_SDK_DIR} sparse-checkout set --no-cone
			/docs/license.md /Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll /Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll
			RESULT_VARIABLE res)
	endif()
	if(res EQUAL 0)
		execute_process(COMMAND ${GIT_EXECUTABLE} -C ${FSR4_SDK_DIR} -c advice.detachedHead=false checkout --quiet ${FSR4_SDK_TAG} RESULT_VARIABLE res)
	endif()
	if(NOT res EQUAL 0 OR NOT EXISTS ${FSR4_SIGNED}/amd_fidelityfx_upscaler_vk.dll)
		message(WARNING "FSR 4: fetching AMD's DLLs failed, left out")
		file(REMOVE_RECURSE ${FSR4_SDK_DIR})
		return()
	endif()
endif()
file(SHA256 ${FSR4_SIGNED}/amd_fidelityfx_loader_vk.dll fsr4_h1)
file(SHA256 ${FSR4_SIGNED}/amd_fidelityfx_upscaler_vk.dll fsr4_h2)
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${FSR4_LOADER_DIR} rev-parse HEAD OUTPUT_VARIABLE fsr4_head OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT fsr4_h1 STREQUAL FSR4_LOADER_DLL_SHA256 OR NOT fsr4_h2 STREQUAL FSR4_UPSCALER_DLL_SHA256 OR NOT fsr4_head STREQUAL FSR4_LOADER_COMMIT)
	message(WARNING "FSR 4: its fetched files are not the ones expected (AMD's DLLs' hashes, the loader's commit), left out")
	return()
endif()

# the loader, built as its own CMakeLists builds it (its tools and tests left out)
set(FSR4_GEN ${CMAKE_CURRENT_BINARY_DIR}/fsr4)
file(MAKE_DIRECTORY ${FSR4_GEN})
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(OUTPUT ${FSR4_GEN}/vk_thunks.inc
	COMMAND ${Python3_EXECUTABLE} -I ${FSR4_LOADER_DIR}/tools/gen_vk_thunks.py ${FSR4_VK_XML} ${FSR4_GEN}/vk_thunks.inc
	DEPENDS ${FSR4_LOADER_DIR}/tools/gen_vk_thunks.py ${FSR4_VK_XML}
	COMMENT "FSR 4 loader: Vulkan thunks from vk.xml")
set(FSR4_SRC)
foreach(f pe teb win32 win32_runtime win32_security adapter spirv_images vk_thunks ffxvk)
	list(APPEND FSR4_SRC ${FSR4_LOADER_DIR}/src/${f}.cpp)
endforeach()
add_library(ffx_vk_linux SHARED ${FSR4_SRC} ${FSR4_GEN}/vk_thunks.inc)
set(FSR4_INCLUDE_DIRS ${FSR4_LOADER_DIR}/include ${FSR4_LOADER_DIR}/third_party/ffx/api/include ${FSR4_LOADER_DIR}/third_party/ffx/upscalers/include)
target_include_directories(ffx_vk_linux PRIVATE ${FSR4_INCLUDE_DIRS} ${FSR4_LOADER_DIR}/src ${FSR4_GEN}
	${FSR4_SPIRV_TOOLS_INC} ${FSR4_SPIRV_HEADERS_INC})
target_link_libraries(ffx_vk_linux PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS} pthread OpenSSL::Crypto ${FSR4_SPIRV_TOOLS} PkgConfig::FSR4_DRM)
target_compile_options(ffx_vk_linux PRIVATE -w)
set(FSR4_DEPLOY_DIR ${ORBITER_BINARY_PLUGIN_DIR}/FSR4)
set_target_properties(ffx_vk_linux PROPERTIES
	CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden
	LIBRARY_OUTPUT_DIRECTORY ${FSR4_DEPLOY_DIR}
	FOLDER OVP)
add_custom_command(TARGET ffx_vk_linux POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${FSR4_SIGNED}/amd_fidelityfx_loader_vk.dll ${FSR4_SIGNED}/amd_fidelityfx_upscaler_vk.dll ${FSR4_DEPLOY_DIR}
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FSR4_SDK_DIR}/docs/license.md ${FSR4_DEPLOY_DIR}/FidelityFX-SDK-license.md
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FSR4_LOADER_DIR}/LICENSE ${FSR4_DEPLOY_DIR}/ffx-vk-linux-LICENSE
)
install(TARGETS ffx_vk_linux LIBRARY DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}/FSR4)
install(FILES ${FSR4_SIGNED}/amd_fidelityfx_loader_vk.dll ${FSR4_SIGNED}/amd_fidelityfx_upscaler_vk.dll
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}/FSR4)
install(FILES ${FSR4_SDK_DIR}/docs/license.md DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}/FSR4 RENAME FidelityFX-SDK-license.md)
install(FILES ${FSR4_LOADER_DIR}/LICENSE DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}/FSR4 RENAME ffx-vk-linux-LICENSE)
set(FSR4_FOUND ON)
