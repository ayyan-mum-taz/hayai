# Third-party code, vendored. See PROVENANCE.md for upstreams and licences.

##################
# nanopb (runtime only; the generator is a host-side python script)
##################
add_library(nanopb STATIC
	nanopb/pb_common.c
	nanopb/pb_encode.c
	nanopb/pb_decode.c)
target_include_directories(nanopb PUBLIC nanopb)
target_compile_definitions(nanopb PUBLIC PB_C99_STATIC_ASSERT)
set(NANOPB_GENERATOR_PY "${CMAKE_CURRENT_SOURCE_DIR}/nanopb/generator/nanopb_generator.py")

##################
# gf-complete + jerasure (Reed-Solomon FEC for the video stream)
#
# The NEON kernels are compiled in (chiaki-ng builds these scalar). FEC
# reconstruction runs exactly when packets were just lost, on the receive
# thread, so this speedup lands entirely on the loss-recovery tail.
##################
add_library(gf_complete STATIC
	gf-complete/src/gf.c
	gf-complete/src/gf_wgen.c
	gf-complete/src/gf_w4.c
	gf-complete/src/gf_w8.c
	gf-complete/src/gf_w16.c
	gf-complete/src/gf_w32.c
	gf-complete/src/gf_w64.c
	gf-complete/src/gf_w128.c
	gf-complete/src/gf_rand.c
	gf-complete/src/gf_general.c
	gf-complete/src/gf_cpu.c
	gf-complete/src/neon/gf_w4_neon.c
	gf-complete/src/neon/gf_w8_neon.c
	gf-complete/src/neon/gf_w16_neon.c
	gf-complete/src/neon/gf_w32_neon.c
	gf-complete/src/neon/gf_w64_neon.c)
target_include_directories(gf_complete PUBLIC gf-complete/include)
target_compile_definitions(gf_complete PUBLIC ARM_NEON)

add_library(jerasure STATIC
	jerasure/src/galois.c
	jerasure/src/jerasure.c
	jerasure/src/reed_sol.c
	jerasure/src/cauchy.c
	jerasure/src/liberation.c)
target_include_directories(jerasure PUBLIC jerasure/include)
target_link_libraries(jerasure PUBLIC gf_complete)

add_subdirectory(chiaki-lib)
