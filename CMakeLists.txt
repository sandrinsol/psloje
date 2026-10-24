# VLORA TAXI: a PS1 taxi game in Vlora, Albania, for PSn00bSDK.
# Builds game.exe (+ game.elf with symbols) and a bootable disc, game.bin +
# game.cue, with the city (MAP.BIN) made from OpenStreetMap data.
cmake_minimum_required(VERSION 3.21)

# PSn00bSDK expects mipsel-none-elf; use mipsel-linux-gnu (Debian/Ubuntu's
# gcc-mipsel-linux-gnu) when that's the one installed.
if(NOT DEFINED PSN00BSDK_TARGET)
	find_program(_mips_none mipsel-none-elf-gcc)
	find_program(_mips_gnu mipsel-linux-gnu-gcc)
	if(NOT _mips_none AND _mips_gnu)
		set(PSN00BSDK_TARGET mipsel-linux-gnu CACHE STRING "Toolchain prefix")
	endif()
endif()

project(
	game
	LANGUAGES   C ASM
	VERSION     0.1.0
	DESCRIPTION "VLORA TAXI"
)

psn00bsdk_add_executable(game GPREL src/main.c src/world.c src/car.c)
target_compile_options(game PRIVATE -O2 -fno-ipa-ra)
# Debian/Ubuntu's mipsel-linux-gnu libgcc is built for MIPS32r2 with hard-float
# helpers, which the PS1 can't run. Link libmips1rt (64-bit maths and soft-float
# for MIPS I, installed by .devcontainer/install-psn00bsdk.sh) ahead of it; without
# that library, src/libgcc_mips1.c covers the integer helpers (but not floats).
if(PSN00BSDK_TARGET STREQUAL "mipsel-linux-gnu")
	set(_sdk_libs "${PSN00BSDK_LIBS}")
	if(NOT _sdk_libs)
		set(_sdk_libs "$ENV{PSN00BSDK_LIBS}")
	endif()
	set(MIPS1_RT "${_sdk_libs}/../libmips1rt.a")
	if(EXISTS "${MIPS1_RT}")
		# Ahead of everything (PSn00bSDK puts -lgcc early on the link line);
		# --gc-sections drops what isn't used.
		target_link_options(game PRIVATE "LINKER:--whole-archive,${MIPS1_RT},--no-whole-archive")
	else()
		message(WARNING "libmips1rt.a not found next to PSN00BSDK_LIBS: float/double won't work with this toolchain (run .devcontainer/install-psn00bsdk.sh)")
		target_sources(game PRIVATE src/libgcc_mips1.c)
	endif()
endif()
# City textures (tools/maketex.mjs).
psn00bsdk_target_incbin(game PRIVATE city_tim assets/city.tim)

# The city: data/vlora.osm (an OpenStreetMap export) when it's there, else the
# made-up test town. MAP_OSM / MAP_SPAWN / MAP_NAME override.
if(NOT DEFINED MAP_OSM)
	if(EXISTS "${PROJECT_SOURCE_DIR}/data/vlora.osm")
		set(MAP_OSM "${PROJECT_SOURCE_DIR}/data/vlora.osm")
		set(MAP_SPAWN "Sheshi i Flamurit")
		set(MAP_NAME "VLORA")
	else()
		set(MAP_OSM "${PROJECT_SOURCE_DIR}/tools/test/testtown.osm")
		set(MAP_SPAWN "Test Square")
		set(MAP_NAME "TEST TOWN (NOT VLORA)")
	endif()
endif()
find_program(NODE node REQUIRED)
add_custom_command(
	OUTPUT  map.bin map-report.json
	COMMAND ${NODE} "${PROJECT_SOURCE_DIR}/tools/osm2map.mjs" "${MAP_OSM}" map.bin
	        --spawn "${MAP_SPAWN}" --name "${MAP_NAME}" --report map-report.json
	DEPENDS "${MAP_OSM}" "${PROJECT_SOURCE_DIR}/tools/osm2map.mjs"
	COMMENT "Converting ${MAP_OSM} to MAP.BIN"
	VERBATIM
)
add_custom_target(map DEPENDS map.bin)

psn00bsdk_add_cd_image(
	iso      # Target name
	game     # Output file name (= game.bin + game.cue)
	iso.xml  # Path to config file
	DEPENDS game map system.cnf
)
