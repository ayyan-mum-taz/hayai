# Packaging an ELF into a homebrew .nro.

find_program(ELF2NRO elf2nro PATHS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(NACPTOOL nacptool PATHS "${DEVKITPRO}/tools/bin" REQUIRED)

# hayai_add_nro(<target> NAME <out> TITLE <t> AUTHOR <a> VERSION <v> [ICON <f>] [ROMFS <dir>])
function(hayai_add_nro target)
	cmake_parse_arguments(NRO "" "NAME;TITLE;AUTHOR;VERSION;ICON;ROMFS" "" ${ARGN})

	set(nacp "${CMAKE_CURRENT_BINARY_DIR}/${NRO_NAME}.nacp")
	set(nro "${CMAKE_CURRENT_BINARY_DIR}/${NRO_NAME}.nro")

	add_custom_command(OUTPUT "${nacp}"
		COMMAND "${NACPTOOL}" --create "${NRO_TITLE}" "${NRO_AUTHOR}" "${NRO_VERSION}" "${nacp}"
		VERBATIM)

	set(extra_args "")
	if(NRO_ICON)
		list(APPEND extra_args "--icon=${NRO_ICON}")
	endif()
	if(NRO_ROMFS)
		list(APPEND extra_args "--romfsdir=${NRO_ROMFS}")
	endif()

	add_custom_command(OUTPUT "${nro}"
		COMMAND "${ELF2NRO}" "$<TARGET_FILE:${target}>" "${nro}" "--nacp=${nacp}" ${extra_args}
		DEPENDS ${target} "${nacp}"
		VERBATIM)

	add_custom_target("${NRO_NAME}_nro" ALL DEPENDS "${nro}")
endfunction()
