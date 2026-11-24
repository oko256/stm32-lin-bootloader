cmake_minimum_required(VERSION 3.28)

# Host unit tests for the bootloader core (src/core). Built with the host compiler, separately from
# the firmware itself (which is cross-compiled from the parent directory). The flash and the LIN
# bus are simulated (stubs/), and the configuration comes from stubs/linboot_config.h.

project(test_stm32-lin-bootloader LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

include(${CMAKE_CURRENT_SOURCE_DIR}/coverage.cmake)

# Sanitizers for everything.
set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
add_compile_options(${SANITIZER_FLAGS})
add_link_options(${SANITIZER_FLAGS})

include(FetchContent)
FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.7.0
)
set(FETCHCONTENT_UPDATES_DISCONNECTED_unity ON)
FetchContent_MakeAvailable(unity)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH FW_DIR)
set(SRC ${FW_DIR}/src)
set(CORE ${SRC}/core)

enable_testing()

# add_core_test(<name> <sources...>)
function(add_core_test name)
    add_executable(${name} ${ARGN} stubs/fake_flash.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${SRC} ${CORE})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Werror)
    target_link_libraries(${name} PRIVATE unity::framework)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_core_test(test_crc32 crc32.test.c ${CORE}/crc32.c)
add_core_test(test_meta meta.test.c ${CORE}/meta.c ${CORE}/crc32.c)
add_core_test(test_image image.test.c ${CORE}/image.c ${CORE}/crc32.c)
add_core_test(test_responder responder.test.c ${CORE}/responder.c ${CORE}/image.c ${CORE}/meta.c ${CORE}/crc32.c)
add_core_test(test_commander commander.test.c stubs/bus_stub.c
    ${CORE}/commander.c ${CORE}/image.c ${CORE}/meta.c ${CORE}/crc32.c)

# Simulated bootloader for the Python host tool tests (tools/linboot/tests). Without sanitizers,
# since it's loaded into the Python interpreter.
add_library(linboot_sim SHARED sim/linboot_sim.c stubs/fake_flash.c
    ${CORE}/responder.c ${CORE}/commander.c ${CORE}/image.c ${CORE}/meta.c ${CORE}/crc32.c)
target_include_directories(linboot_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${SRC} ${CORE})
target_compile_options(linboot_sim PRIVATE -fno-sanitize=all -fvisibility=hidden -Wall -Wextra -Wconversion -Werror)
target_link_options(linboot_sim PRIVATE -fno-sanitize=all)

# The host tool's tests, if Python with pytest is available.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME test_host_tool
        COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider ${FW_DIR}/tools/linboot/tests)
    set_tests_properties(test_host_tool PROPERTIES ENVIRONMENT "LINBOOT_SIM_LIB=$<TARGET_FILE:linboot_sim>")
endif()

# Only the bootloader core: the glue to minlin and the port need the hardware.
coverage_add_target(INCLUDE ${CORE})
