# Toolchain file for the GNU Arm Embedded toolchain (arm-none-eabi-gcc).
#
# CPU-specific flags (-mcpu etc.) are not set here; they come from the port (cmake/ports/*.cmake),
# so that the same toolchain file works for all Cortex-M targets.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER_ID GNU)
set(TOOLCHAIN_PREFIX arm-none-eabi-)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
set(CMAKE_LINKER       ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size)

set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".elf")

# Compiler checks can't link a full executable without a linker script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-Wall -fdata-sections -ffunction-sections -fstack-usage -fno-common")
set(CMAKE_ASM_FLAGS_INIT "-x assembler-with-cpp -MMD -MP")

# Debug builds are optimized for size too: the bootloader must fit in its flash region also in
# Debug builds.
set(CMAKE_C_FLAGS_DEBUG   "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")

set(CMAKE_EXE_LINKER_FLAGS_INIT "--specs=nano.specs --specs=nosys.specs -nostartfiles -Wl,--gc-sections -Wl,--print-memory-usage -z noexecstack")
