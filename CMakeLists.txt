cmake_minimum_required(VERSION 3.22)

# LIN bus bootloader for STM32. Cross-compiled with cmake/arm-none-eabi.cmake (see the presets);
# all configuration is done with the LINBOOT_* cache variables (cmake/linboot_options.cmake).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(stm32-lin-bootloader LANGUAGES C ASM)

include(cmake/linboot_options.cmake)

option(LINBOOT_BUILD_EXAMPLES "Build the example applications (examples/)" ON)
option(LINBOOT_LTO "Build with link time optimization (smaller bootloader)" ON)
if(LINBOOT_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# The CPU flags apply to everything, including minlin.
add_compile_options(${LINBOOT_PORT_CPU_FLAGS})
add_link_options(${LINBOOT_PORT_CPU_FLAGS})

#--------------------------------------------------------------------------------------------------
# Generated configuration header and linker scripts
#--------------------------------------------------------------------------------------------------

set(LINBOOT_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

configure_file(src/linboot_config.h.in ${LINBOOT_GENERATED_DIR}/linboot_config.h)

# linboot_generate_linker_script(<output> <kind> <flash origin> <flash length> <ram origin>
#                                <ram length> <stack size>)
function(linboot_generate_linker_script output kind flash_origin flash_length ram_origin ram_length stack_size)
    set(LINKER_SCRIPT_KIND ${kind})
    set(LINKER_FLASH_ORIGIN ${flash_origin})
    set(LINKER_FLASH_LENGTH ${flash_length})
    set(LINKER_RAM_ORIGIN ${ram_origin})
    set(LINKER_RAM_LENGTH ${ram_length})
    set(LINKER_STACK_SIZE ${stack_size})
    configure_file(${PROJECT_SOURCE_DIR}/cmake/linker/cortex-m.ld.in ${output} @ONLY)
endfunction()

linboot_generate_linker_script(${LINBOOT_GENERATED_DIR}/bootloader.ld "bootloader"
    ${LINBOOT_FLASH_BASE} ${LINBOOT_BOOTLOADER_SIZE} ${LINBOOT_RAM_BASE} ${LINBOOT_RAM_SIZE} ${LINBOOT_STACK_SIZE})

# Applications get the flash after the bootloader, and the RAM except for the relocated vector
# table (on cores without VTOR). This script is also meant as a reference for integrators.
math(EXPR LINBOOT_APP_RAM_ORIGIN "${LINBOOT_RAM_BASE} + ${LINBOOT_PORT_APP_VECTOR_RAM_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR LINBOOT_APP_RAM_LENGTH "${LINBOOT_RAM_SIZE} - ${LINBOOT_PORT_APP_VECTOR_RAM_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
set(LINBOOT_APP_LINKER_SCRIPT ${LINBOOT_GENERATED_DIR}/app.ld)
linboot_generate_linker_script(${LINBOOT_APP_LINKER_SCRIPT} "application"
    ${LINBOOT_APP_ADDR} ${LINBOOT_APP_MAX_SIZE} ${LINBOOT_APP_RAM_ORIGIN} ${LINBOOT_APP_RAM_LENGTH} "0x400")

#--------------------------------------------------------------------------------------------------
# minlin
#--------------------------------------------------------------------------------------------------

set(minlin_HARDENING OFF)
set(minlin_HARDENING_GLOBALLY OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/minlin)

#--------------------------------------------------------------------------------------------------
# Bootloader
#--------------------------------------------------------------------------------------------------

# linboot_add_firmware_outputs(<target>)
#
# Generates <target>.bin and <target>.hex next to the .elf and prints the size after building.
function(linboot_add_firmware_outputs target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.bin
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.hex
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin ${CMAKE_CURRENT_BINARY_DIR}/${target}.hex
        VERBATIM
    )
endfunction()

# Warnings for our own code (not for minlin or the CMSIS headers).
set(LINBOOT_WARNING_FLAGS
    -Wall -Wextra -Wformat=2 -Wconversion -Wimplicit-fallthrough -Werror=format-security -Wshadow
    -Wcast-align -Wunused -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wvla
    -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion
    -Wstack-usage=256
)

add_executable(linboot)
target_sources(linboot PRIVATE
    src/core/linboot.c
    src/core/crc32.c
    src/core/meta.c
    src/core/image.c
    src/hooks/hooks_default.c
    ${LINBOOT_EXTRA_SOURCES}
)
if(LINBOOT_MINIMAL_LIBC)
    target_sources(linboot PRIVATE src/core/mini_libc.c)
endif()
if(LINBOOT_ROLE STREQUAL "RESPONDER")
    target_sources(linboot PRIVATE src/core/responder.c)
else()
    target_sources(linboot PRIVATE src/core/commander.c)
endif()
linboot_port_setup_target(linboot)
target_include_directories(linboot PRIVATE
    src
    src/core
    ${LINBOOT_GENERATED_DIR}
    ${LINBOOT_EXTRA_INCLUDE_DIRS}
)
target_compile_options(linboot PRIVATE ${LINBOOT_WARNING_FLAGS})
target_link_options(linboot PRIVATE
    -T ${LINBOOT_GENERATED_DIR}/bootloader.ld
    -Wl,-Map=$<TARGET_FILE_DIR:linboot>/linboot.map
)
set_target_properties(linboot PROPERTIES LINK_DEPENDS ${LINBOOT_GENERATED_DIR}/bootloader.ld)
target_link_libraries(linboot PRIVATE minlin::minlin)
linboot_add_firmware_outputs(linboot)

#--------------------------------------------------------------------------------------------------
# Examples
#--------------------------------------------------------------------------------------------------

if(LINBOOT_BUILD_EXAMPLES)
    add_subdirectory(examples/blinky)
endif()
