# Trimmed build of libchiaki for Nintendo Switch.
#
# Differences from upstream chiaki-ng's lib/CMakeLists.txt:
#   * src/remote/holepunch.c is replaced by src/holepunch_stub.c (in ../../src).
#     holepunch.c is the only file that pulls in libcurl, json-c, miniupnpc and
#     libevent, none of which are needed for LAN sessions. The RUDP transport it
#     sits on top of is still built, so restoring PSN remote connect later is a
#     matter of dropping the real file back in and adding those four libraries.
#   * ffmpegdecoder.c / pidecoder.c are not built: hayai owns the decode path so
#     that decoded frames stay on the GPU (see src/video).
#   * OpenSSL is replaced by mbedTLS, which is what devkitPro ships.

find_program(PROTOC protoc NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(PYTHON3 python3 NO_CMAKE_FIND_ROOT_PATH REQUIRED)

##################
# nanopb-generated takion protobuf messages
##################
set(PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${PROTO_DIR}")

add_custom_command(OUTPUT "${PROTO_DIR}/takion.pb"
	COMMAND "${PROTOC}" "-o${PROTO_DIR}/takion.pb"
		"${CMAKE_CURRENT_SOURCE_DIR}/protobuf/takion.proto"
		"-I${CMAKE_CURRENT_SOURCE_DIR}/protobuf"
	MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/takion.proto"
	VERBATIM)

get_filename_component(PROTOC_DIR "${PROTOC}" DIRECTORY)
add_custom_command(OUTPUT "${PROTO_DIR}/takion.pb.c" "${PROTO_DIR}/takion.pb.h"
	COMMAND "${CMAKE_COMMAND}" -E env "PATH=${PROTOC_DIR}:$ENV{PATH}"
		"${PYTHON3}" "${NANOPB_GENERATOR_PY}" "${PROTO_DIR}/takion.pb"
	MAIN_DEPENDENCY "${PROTO_DIR}/takion.pb"
	WORKING_DIRECTORY "${PROTO_DIR}"	# nanopb_generator writes next to its cwd
	VERBATIM)

##################
# chiaki-lib
##################
set(SOURCE_FILES
	src/common.c
	src/sock.c
	src/session.c
	src/thread.c
	src/base64.c
	src/http.c
	src/log.c
	src/ctrl.c
	src/rpcrypt.c
	src/takion.c
	src/senkusha.c
	src/streamconnection.c
	src/ecdh.c
	src/launchspec.c
	src/random.c
	src/gkcrypt.c
	src/audio.c
	src/audioreceiver.c
	src/audiosender.c
	src/videoreceiver.c
	src/frameprocessor.c
	src/packetstats.c
	src/discovery.c
	src/congestioncontrol.c
	src/stoppipe.c
	src/reorderqueue.c
	src/discoveryservice.c
	src/feedback.c
	src/feedbacksender.c
	src/controller.c
	src/takionsendbuffer.c
	src/time.c
	src/fec.c
	src/regist.c
	src/opusdecoder.c
	src/opusencoder.c
	src/orientation.c
	src/bitstream.c
	src/remote/rudp.c
	src/remote/rudpsendbuffer.c
	"${CMAKE_CURRENT_SOURCE_DIR}/../../src/compat/holepunch_stub.c"
	"${PROTO_DIR}/takion.pb.c")

add_library(chiaki-lib STATIC ${SOURCE_FILES})

set(CHIAKI_LIB_ENABLE_OPUS 1)
set(CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0)
set(CHIAKI_LIB_ENABLE_PI_DECODER 0)
configure_file(config.h.in include/chiaki/config.h)

target_include_directories(chiaki-lib
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/include"
		"${CMAKE_CURRENT_BINARY_DIR}/include"
	PRIVATE
		"${PROTO_DIR}")

find_library(MBEDTLS_LIB mbedtls REQUIRED)
find_library(MBEDX509_LIB mbedx509 REQUIRED)
find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)
find_library(OPUS_LIB opus REQUIRED)

target_link_libraries(chiaki-lib
	PUBLIC
		nanopb
		jerasure
		"${OPUS_LIB}"
		"${MBEDTLS_LIB}" "${MBEDX509_LIB}" "${MBEDCRYPTO_LIB}")

target_compile_definitions(chiaki-lib PUBLIC CHIAKI_LIB_ENABLE_MBEDTLS=1)
target_compile_options(chiaki-lib PRIVATE -Wall -Wno-unused-variable -Wno-unused-function)
