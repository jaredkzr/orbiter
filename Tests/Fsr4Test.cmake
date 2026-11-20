# Vulkan.Fsr4: a short offscreen session with FSR 4 as the upscaler (the
# default; half render scale). Passes if the session ends
# normally having upscaled with FSR 4, or with FSR 3 where FSR 4 is not
# there (not built in, no graphics card AMD's library gives it to): the
# fallback is what the default promises. Fails on a crash, a failed dispatch
# or temporal anti-aliasing other than FSR's.
# Vulkan.Fsr3 (-DUPSCALER=fsr3): the same with FSR 3 asked for; fails if
# another upscaler ran.
#   cmake -DEXE=<Orbiter> -DROOT=<binary root> -DSCN=<scenario> [-DUPSCALER=fsr3] -P Fsr4Test.cmake
if(NOT UPSCALER)
	set(UPSCALER fsr4)
endif()
set(ENV{ORBITER_VK_OFFSCREEN} 1)
set(ENV{ORBITER_VK_SKY} 0)
set(ENV{ORBITER_VK_UPSCALER} ${UPSCALER})
set(ENV{ORBITER_VK_RENDER_SCALE} 0.5)
set(ENV{ORBITER_VK_FRAMETIME} 0.016)
execute_process(COMMAND ${EXE} -p VulkanClient --scenariox=${SCN} --fixedstep=0.02 --maxframes=90
	WORKING_DIRECTORY ${ROOT} RESULT_VARIABLE res OUTPUT_QUIET ERROR_QUIET)
# (the session's log: the test's own where ctest names one, ORBITER_LOG)
set(logfile ${ROOT}/Orbiter.log)
if(DEFINED ENV{ORBITER_LOG})
	set(logfile $ENV{ORBITER_LOG})
endif()
file(READ ${logfile} log)
string(REGEX MATCH "temporal anti-aliasing: FSR [34]" aa "${log}")
string(REGEX MATCH "FSR 4 upscaler [^\n]*" up4 "${log}")
string(REGEX MATCH "FSR 4[^\n]*(not installed|could not be loaded|no FSR 4|did not start)[^\n]*" why "${log}")
string(REGEX MATCH "FSR [34] (dispatch failed|failed|context creation failed)[^\n]*" bad "${log}")
if(NOT res EQUAL 0)
	message("Fsr4Test: failed (the session ended with ${res})")
elseif(bad)
	message("Fsr4Test: failed (${bad})")
elseif(UPSCALER STREQUAL "fsr3")
	if(aa STREQUAL "temporal anti-aliasing: FSR 3" AND NOT up4)
		message("Fsr4Test: passed with FSR 3 as asked")
	else()
		message("Fsr4Test: failed (FSR 3 asked for: '${aa}')")
	endif()
elseif(aa STREQUAL "temporal anti-aliasing: FSR 4" AND up4)
	message("Fsr4Test: passed with ${up4}")
elseif(aa STREQUAL "temporal anti-aliasing: FSR 3")
	message("Fsr4Test: passed with FSR 3 in FSR 4's place (${why})")
else()
	message("Fsr4Test: failed (no FSR upscaler: '${aa}')")
endif()
