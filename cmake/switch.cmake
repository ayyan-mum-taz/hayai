# Toolchain file for Nintendo Switch (devkitA64 + libnx).
# Use with: cmake -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake

set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "Path to devkitPro")
if(NOT DEVKITPRO)
	message(FATAL_ERROR "DEVKITPRO is not set. Build inside the hayai-build container (scripts/build.sh).")
endif()

include("${DEVKITPRO}/cmake/Switch.cmake")

set(PORTLIBS "${DEVKITPRO}/portlibs/switch" CACHE PATH "devkitPro switch portlibs")
set(NSWITCH TRUE)

# Several devkitPro packages ship only .pc files, so keep pkg-config usable.
set(ENV{PKG_CONFIG_PATH} "${PORTLIBS}/lib/pkgconfig")

set(CMAKE_USE_SYSTEM_ENVIRONMENT_PATH OFF)
