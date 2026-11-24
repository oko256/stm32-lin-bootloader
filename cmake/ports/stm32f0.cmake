# STM32F0 port: Cortex-M0, register level access with the CMSIS device headers.
#
# Sets the LINBOOT_PORT_* defaults for the generic options, declares the port specific options,
# and provides linboot_port_setup_target() for adding the port sources to a target.

set(LINBOOT_MCU "STM32F042x6" CACHE STRING "CMSIS device define of the MCU (e.g. STM32F042x6)")

# Memory defaults for known devices; other devices must give LINBOOT_FLASH_SIZE etc. explicitly
# (and the CMSIS device header must be available in Drivers/CMSIS/Device/ST/STM32F0xx/Include).
set(LINBOOT_PORT_FLASH_BASE "0x08000000")
set(LINBOOT_PORT_RAM_BASE "0x20000000")
set(LINBOOT_PORT_FLASH_SIZE "")
set(LINBOOT_PORT_RAM_SIZE "")
set(LINBOOT_PORT_FLASH_PAGE_SIZE "1024")
if(LINBOOT_MCU STREQUAL "STM32F042x6")
    set(LINBOOT_PORT_FLASH_SIZE "32768")
    set(LINBOOT_PORT_RAM_SIZE "6144")
elseif(LINBOOT_MCU STREQUAL "STM32F042x4")
    set(LINBOOT_PORT_FLASH_SIZE "16384")
    set(LINBOOT_PORT_RAM_SIZE "6144")
endif()

# Flash is programmed 16 bits at a time.
set(LINBOOT_PORT_FLASH_WRITE_ALIGN 2)
# Cortex-M0 has no VTOR: the bootloader copies the application's vector table (16 system + up to
# 32 IRQ vectors) to the start of the RAM and remaps the RAM to address 0. The application must not
# use this RAM.
set(LINBOOT_PORT_APP_VECTOR_RAM_SIZE "0xC0")
set(LINBOOT_PORT_CPU_FLAGS -mcpu=cortex-m0 -mthumb)

#--------------------------------------------------------------------------------------------------
# Port options
#--------------------------------------------------------------------------------------------------

set(LINBOOT_CLOCK "HSI48" CACHE STRING
    "System clock source: HSI48 (48 MHz internal), HSI (8 MHz internal) or HSE (external, used directly)")
set_property(CACHE LINBOOT_CLOCK PROPERTY STRINGS HSI48 HSI HSE)
set(LINBOOT_HSE_HZ "8000000" CACHE STRING "HSE frequency in Hz (when LINBOOT_CLOCK is HSE)")
option(LINBOOT_HSE_BYPASS "HSE is an external clock signal instead of a crystal" OFF)

set(LINBOOT_LIN_USART "1" CACHE STRING "USART used for LIN (needs LIN support, e.g. USART1 on STM32F042)")
set(LINBOOT_LIN_TX_PIN "PB6" CACHE STRING "LIN TX pin")
set(LINBOOT_LIN_RX_PIN "PB7" CACHE STRING "LIN RX pin")
set(LINBOOT_LIN_AF "0" CACHE STRING "Alternate function number of the USART on the LIN pins")
set(LINBOOT_LIN_EN_PIN "" CACHE STRING "Optional LIN transceiver enable pin, driven active while in the bootloader (empty: none)")
option(LINBOOT_LIN_EN_ACTIVE_LOW "The LIN transceiver enable pin is active low" OFF)
option(LINBOOT_STM32F0_FORCE_FLASH_BOOT
    "Program the option bytes to always boot from flash (BOOT_SEL=0, nBOOT0=1) if they aren't already; for boards with a floating BOOT0 pin"
    OFF)

if(NOT LINBOOT_CLOCK MATCHES "^(HSI48|HSI|HSE)$")
    message(FATAL_ERROR "LINBOOT_CLOCK must be HSI48, HSI or HSE, got '${LINBOOT_CLOCK}'")
endif()
linboot_check_uint(LINBOOT_HSE_HZ 4000000 32000000)
if(NOT LINBOOT_LIN_USART MATCHES "^[1-8]$")
    message(FATAL_ERROR "LINBOOT_LIN_USART must be a USART number, got '${LINBOOT_LIN_USART}'")
endif()
linboot_check_uint(LINBOOT_LIN_AF 0 7)
linboot_parse_pin(LIN_TX ${LINBOOT_LIN_TX_PIN})
linboot_parse_pin(LIN_RX ${LINBOOT_LIN_RX_PIN})

if(LINBOOT_CLOCK STREQUAL "HSI48")
    set(_sysclk_hz 48000000)
elseif(LINBOOT_CLOCK STREQUAL "HSI")
    set(_sysclk_hz 8000000)
else()
    math(EXPR _sysclk_hz "${LINBOOT_HSE_HZ}")
endif()

# Port specific part of linboot_config.h.
set(LINBOOT_PORT_CONFIG "#define LINBOOT_MCU_${LINBOOT_MCU} 1
#define LINBOOT_CLOCK_${LINBOOT_CLOCK} 1
#define LINBOOT_SYSCLK_HZ ${_sysclk_hz}u
#define LINBOOT_HSE_BYPASS ${LINBOOT_HSE_BYPASS}
#define LINBOOT_LIN_USART ${LINBOOT_LIN_USART}
#define LINBOOT_LIN_AF ${LINBOOT_LIN_AF}u
#define LINBOOT_LIN_TX_GPIO_INDEX ${LIN_TX_GPIO_INDEX}u
#define LINBOOT_LIN_TX_PIN ${LIN_TX_PIN}u
#define LINBOOT_LIN_RX_GPIO_INDEX ${LIN_RX_GPIO_INDEX}u
#define LINBOOT_LIN_RX_PIN ${LIN_RX_PIN}u
#define LINBOOT_STM32F0_FORCE_FLASH_BOOT ${LINBOOT_STM32F0_FORCE_FLASH_BOOT}
")
if(NOT LINBOOT_LIN_EN_PIN STREQUAL "")
    linboot_parse_pin(LIN_EN ${LINBOOT_LIN_EN_PIN})
    string(APPEND LINBOOT_PORT_CONFIG "#define LINBOOT_LIN_EN_GPIO_INDEX ${LIN_EN_GPIO_INDEX}u
#define LINBOOT_LIN_EN_PIN ${LIN_EN_PIN}u
#define LINBOOT_LIN_EN_ACTIVE_LOW ${LINBOOT_LIN_EN_ACTIVE_LOW}
")
endif()
# CMake booleans (ON/OFF) to C (1/0).
string(REGEX REPLACE " (ON|TRUE|YES)\n" " 1\n" LINBOOT_PORT_CONFIG "${LINBOOT_PORT_CONFIG}")
string(REGEX REPLACE " (OFF|FALSE|NO)\n" " 0\n" LINBOOT_PORT_CONFIG "${LINBOOT_PORT_CONFIG}")

set(LINBOOT_PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/port/stm32f0)
cmake_path(NORMAL_PATH LINBOOT_PORT_DIR)
set(LINBOOT_CMSIS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/CMSIS)
cmake_path(NORMAL_PATH LINBOOT_CMSIS_DIR)

# linboot_port_setup_target(<target> [APP])
#
# Adds the CPU flags, the CMSIS include directories and the MCU define to the target. Without APP,
# also adds the port sources of the bootloader.
function(linboot_port_setup_target target)
    cmake_parse_arguments(arg "APP" "" "" ${ARGN})
    target_compile_options(${target} PRIVATE ${LINBOOT_PORT_CPU_FLAGS})
    target_link_options(${target} PRIVATE ${LINBOOT_PORT_CPU_FLAGS})
    target_compile_definitions(${target} PRIVATE ${LINBOOT_MCU})
    target_include_directories(${target} SYSTEM PRIVATE
        ${LINBOOT_CMSIS_DIR}/Include
        ${LINBOOT_CMSIS_DIR}/Device/ST/STM32F0xx/Include
    )
    if(NOT arg_APP)
        target_sources(${target} PRIVATE
            ${LINBOOT_PORT_DIR}/startup.c
            ${LINBOOT_PORT_DIR}/port_stm32f0.c
            ${LINBOOT_PORT_DIR}/flash.c
            ${LINBOOT_PORT_DIR}/lin_usart.c
        )
    endif()
endfunction()
