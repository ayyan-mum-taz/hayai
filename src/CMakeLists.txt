##################
# shaders -> romfs
##################
find_program(UAM uam PATHS "${DEVKITPRO}/tools/bin" REQUIRED)

set(ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${ROMFS_DIR}/shaders")

set(SHADER_OUTPUTS "")
foreach(shader video_vsh:vert video_fsh:frag ui_vsh:vert ui_fsh:frag)
	string(REPLACE ":" ";" parts "${shader}")
	list(GET parts 0 name)
	list(GET parts 1 stage)
	set(out "${ROMFS_DIR}/shaders/${name}.dksh")
	add_custom_command(OUTPUT "${out}"
		COMMAND "${UAM}" -s "${stage}" -o "${out}" "${CMAKE_SOURCE_DIR}/shaders/${name}.glsl"
		MAIN_DEPENDENCY "${CMAKE_SOURCE_DIR}/shaders/${name}.glsl"
		VERBATIM)
	list(APPEND SHADER_OUTPUTS "${out}")
endforeach()
add_custom_target(hayai_shaders DEPENDS ${SHADER_OUTPUTS})

##################
# hayai
##################
add_executable(hayai
	main.cpp
	core/log.cpp
	core/telemetry.cpp
	core/thread.cpp
	gfx/pool.cpp
	gfx/presenter.cpp
	video/decoder.cpp
	video/renderer.cpp
	audio/audio.cpp
	input/input.cpp
	app/config.cpp
	app/discovery.cpp
	app/stream.cpp
	app/ui.cpp
	ui/draw.cpp)

add_dependencies(hayai hayai_shaders)

target_include_directories(hayai PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/vendor/stb")

# Experiment knobs, applied to the vendored lib via #ifndef guards there.
# Reorder timeout: FEC already covers intra-frame gaps; 2 ms guards only
# against cross-frame-boundary reordering instead of chiaki-ng's 16 ms.
# Rebuild with other values to A/B (see docs/latency.md).
set(HAYAI_REORDER_TIMEOUT_US "2000" CACHE STRING "Takion AV reorder timeout (us)")
set(HAYAI_FEEDBACK_MIN_MS "8" CACHE STRING "Min interval between feedback state sends (ms); also the motion rate")
target_compile_definitions(chiaki-lib PRIVATE
	"TAKION_AV_REORDER_TIMEOUT_US=${HAYAI_REORDER_TIMEOUT_US}"
	"FEEDBACK_STATE_TIMEOUT_MIN_MS=${HAYAI_FEEDBACK_MIN_MS}")

# libavcodec is used purely as an NVDEC front-end: no demuxer, no format layer,
# no swscale. Packets come off the wire already framed.
find_library(AVCODEC_LIB avcodec REQUIRED)
find_library(AVUTIL_LIB avutil REQUIRED)
find_library(SWRESAMPLE_LIB swresample REQUIRED)
find_library(DAV1D_LIB dav1d REQUIRED)
find_library(Z_LIB z REQUIRED)
find_library(DEKO3D_LIB deko3d REQUIRED)
find_library(NX_LIB nx REQUIRED)

target_link_libraries(hayai PRIVATE
	chiaki-lib
	"${AVCODEC_LIB}"
	"${SWRESAMPLE_LIB}"
	"${AVUTIL_LIB}"
	"${DAV1D_LIB}"
	"${Z_LIB}"
	"${DEKO3D_LIB}"
	"${NX_LIB}"
	m)

target_compile_options(hayai PRIVATE -Wall -Wno-unused-variable)

# assets/icon.local.jpg (gitignored) overrides the shipped icon, so a personal
# build can use artwork that should not be redistributed.
set(HAYAI_ICON "${CMAKE_SOURCE_DIR}/assets/icon.jpg")
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.local.jpg")
	set(HAYAI_ICON "${CMAKE_SOURCE_DIR}/assets/icon.local.jpg")
	message(STATUS "using local icon override: assets/icon.local.jpg")
endif()

hayai_add_nro(hayai
	NAME hayai
	TITLE "hayai"
	AUTHOR "hayai contributors"
	VERSION "0.4.0"
	ICON "${HAYAI_ICON}"
	ROMFS "${ROMFS_DIR}")
