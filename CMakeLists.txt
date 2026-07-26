cmake_minimum_required(VERSION 3.16)
project(hayai C CXX)

# hayai - low-latency PlayStation Remote Play client for Nintendo Switch.
#
# The protocol core is a trimmed vendored copy of chiaki-ng's libchiaki
# (see vendor/PROVENANCE.md). Everything above the protocol is ours and is
# built for one goal: the shortest possible path from a UDP datagram to a lit
# pixel, and from a button press to a datagram.

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT NSWITCH)
	message(FATAL_ERROR
		"hayai only targets Nintendo Switch. Configure with:\n"
		"  cmake -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# -O3 over -Os: we care about decode/convert throughput far more than about
# a few hundred KB of NRO size.
set(CMAKE_C_FLAGS_RELEASE "-O3 -ffast-math -fno-strict-aliasing")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -fno-strict-aliasing")

# devkitPro's Switch.cmake puts libnx on the include path but not the portlibs
# (mbedtls, opus, ffmpeg, deko3d all live there).
include_directories(SYSTEM "${PORTLIBS}/include")

include(cmake/nro.cmake)

add_subdirectory(vendor)
add_subdirectory(src)
