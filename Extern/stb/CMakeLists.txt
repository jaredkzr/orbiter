# stb single-file libraries (public domain / MIT): stb_image for decoding
# the PNG and JPEG tiles of streamed terrain
Include(FetchContent)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4
  UPDATE_DISCONNECTED 1
)
FetchContent_MakeAvailable(stb)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})
