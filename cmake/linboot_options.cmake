# Configuration options of the bootloader.
#
# Everything here is a CMake cache variable, so it can be given on the command line
# (-DLINBOOT_xxx=...) or in a preset. The values are validated, the flash layout is derived from
# them, and everything ends up in the generated linboot_config.h and linker scripts.
#
# Numbers can be given in decimal or as 0x-prefixed hexadecimal.

include(${CMAKE_CURRENT_LIST_DIR}/linboot_helpers.cmake)

#--------------------------------------------------------------------------------------------------
# Role and identity
#--------------------------------------------------------------------------------------------------

set(LINBOOT_ROLE "RESPONDER" CACHE STRING
    "LIN role of the bootloader: RESPONDER (updated by the bus commander) or COMMANDER (pulls its update from a provider responder)")
set_property(CACHE LINBOOT_ROLE PROPERTY STRINGS RESPONDER COMMANDER)
if(NOT LINBOOT_ROLE MATCHES "^(RESPONDER|COMMANDER)$")
    message(FATAL_ERROR "LINBOOT_ROLE must be RESPONDER or COMMANDER, got '${LINBOOT_ROLE}'")
endif()

set(LINBOOT_DEVICE_TYPE "" CACHE STRING
    "Manufacturer specific device type identifier (uint16), tells the updater which firmware fits this device")
if(LINBOOT_DEVICE_TYPE STREQUAL "")
    message(FATAL_ERROR "LINBOOT_DEVICE_TYPE must be set (uint16), e.g. -DLINBOOT_DEVICE_TYPE=0x0001")
endif()
linboot_check_uint(LINBOOT_DEVICE_TYPE 0 0xFFFF)

set(LINBOOT_HW_VERSION_DEFAULT "0" CACHE STRING
    "Hardware version (uint16) returned by the default linboot_hook_hardware_version(); integrators normally override the hook")
linboot_check_uint(LINBOOT_HW_VERSION_DEFAULT 0 0xFFFF)

set(LINBOOT_EXTRA_SOURCES "" CACHE STRING
    "Extra source files (list) compiled into the bootloader, e.g. board specific hook implementations")
set(LINBOOT_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories (list) for the bootloader")

#--------------------------------------------------------------------------------------------------
# LIN bus
#--------------------------------------------------------------------------------------------------

set(LINBOOT_BAUD "19200" CACHE STRING "LIN bus baud rate")
linboot_check_uint(LINBOOT_BAUD 1000 20000)

option(LINBOOT_CLASSIC_CHECKSUM "Use the classic (LIN 1.x) checksum instead of the enhanced one" OFF)

# Responder role frame IDs.
set(LINBOOT_ID_STATUS "0x30" CACHE STRING "RESPONDER: ID of the STATUS frame (published by the bootloader; its header is the heartbeat)")
set(LINBOOT_ID_CMD    "0x31" CACHE STRING "RESPONDER: ID of the CMD frame (subscribed by the bootloader)")
set(LINBOOT_ID_DATA   "0x32" CACHE STRING "Both roles: ID of the DATA frame (RESPONDER: subscribed; COMMANDER: published by the selected provider)")

# Commander role frame IDs.
set(LINBOOT_ID_QUERY "0x31" CACHE STRING "COMMANDER: ID of the QUERY frame (published by the bootloader, subscribed by all providers)")
set(LINBOOT_PROVIDER_OFFER_IDS "0x33;0x34;0x35;0x36" CACHE STRING
    "COMMANDER: IDs of the OFFER frames (list), one per possible provider; the index in the list is the provider index")

#--------------------------------------------------------------------------------------------------
# Timing and retries
#--------------------------------------------------------------------------------------------------

set(LINBOOT_BOOT_TIMEOUT_MS "2000" CACHE STRING
    "How long to look for an update after reset before booting a valid application")
linboot_check_uint(LINBOOT_BOOT_TIMEOUT_MS 1 0xFFFFFF)
set(LINBOOT_SESSION_TIMEOUT_MS "5000" CACHE STRING
    "RESPONDER: once an update session has started, how long without traffic before giving up on it")
linboot_check_uint(LINBOOT_SESSION_TIMEOUT_MS 1 0xFFFFFF)
set(LINBOOT_BOOT_DELAY_MS "100" CACHE STRING
    "RESPONDER: delay after a successful update before booting, so that the commander can read the final status")
linboot_check_uint(LINBOOT_BOOT_DELAY_MS 0 0xFFFFFF)
set(LINBOOT_MAX_ATTEMPTS "3" CACHE STRING
    "COMMANDER: how many times an update is attempted before giving up (and booting a still valid application)")
linboot_check_uint(LINBOOT_MAX_ATTEMPTS 1 255)
set(LINBOOT_POLL_RETRIES "3" CACHE STRING
    "COMMANDER: how many times a DATA frame is polled again (no response, bad checksum) before re-sending READ")
linboot_check_uint(LINBOOT_POLL_RETRIES 0 255)
set(LINBOOT_READ_RETRIES "20" CACHE STRING
    "COMMANDER: how many READ requests in a row without any progress fail the update attempt")
linboot_check_uint(LINBOOT_READ_RETRIES 1 255)

#--------------------------------------------------------------------------------------------------
# Target (port) and flash layout
#--------------------------------------------------------------------------------------------------

set(LINBOOT_PORT "stm32f0" CACHE STRING "Platform port (directory name in src/port and cmake/ports)")
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/ports/${LINBOOT_PORT}.cmake)
    message(FATAL_ERROR "Unknown LINBOOT_PORT '${LINBOOT_PORT}'")
endif()

# The port declares its own options and the defaults for the memory sizes (LINBOOT_PORT_*).
include(${CMAKE_CURRENT_LIST_DIR}/ports/${LINBOOT_PORT}.cmake)

set(LINBOOT_FLASH_BASE "${LINBOOT_PORT_FLASH_BASE}" CACHE STRING "Flash base address")
set(LINBOOT_FLASH_SIZE "${LINBOOT_PORT_FLASH_SIZE}" CACHE STRING "Flash size in bytes")
set(LINBOOT_FLASH_PAGE_SIZE "${LINBOOT_PORT_FLASH_PAGE_SIZE}" CACHE STRING "Flash erase page size in bytes")
set(LINBOOT_RAM_BASE "${LINBOOT_PORT_RAM_BASE}" CACHE STRING "RAM base address")
set(LINBOOT_RAM_SIZE "${LINBOOT_PORT_RAM_SIZE}" CACHE STRING "RAM size in bytes")
foreach(i_var FLASH_BASE FLASH_SIZE FLASH_PAGE_SIZE RAM_BASE RAM_SIZE)
    if(LINBOOT_${i_var} STREQUAL "")
        message(FATAL_ERROR "LINBOOT_${i_var} must be set (no default for LINBOOT_MCU '${LINBOOT_MCU}')")
    endif()
    linboot_check_uint(LINBOOT_${i_var} 0 0xFFFFFFFF)
endforeach()

set(LINBOOT_BOOTLOADER_SIZE "0x1000" CACHE STRING
    "Flash reserved for the bootloader at the start of the flash (multiple of the page size)")
set(LINBOOT_META_ADDR "" CACHE STRING
    "Address of the flash page holding the firmware metadata (version, size, CRC); default is the last page")
set(LINBOOT_BLOCK_SIZE "" CACHE STRING
    "Size of the RAM write buffer (default: page size, or 1024 if the page is larger); must divide the page size")
option(LINBOOT_VERIFY_APP_ON_BOOT "Verify the application CRC on every boot (not just the metadata record)" ON)
set(LINBOOT_STACK_SIZE "0x400" CACHE STRING "Minimum stack size of the bootloader (checked by the linker)")
linboot_check_uint(LINBOOT_STACK_SIZE 0x100 ${LINBOOT_RAM_SIZE})
option(LINBOOT_MINIMAL_LIBC "Use minimal (small but slow) memcpy/memset/memmove instead of the C library ones" ON)

linboot_check_uint(LINBOOT_BOOTLOADER_SIZE 1 ${LINBOOT_FLASH_SIZE})
linboot_check_multiple(LINBOOT_BOOTLOADER_SIZE ${LINBOOT_FLASH_PAGE_SIZE})

# Layout math is done in decimal (if() can't compare hex strings), hex strings are made at the end.
math(EXPR _flash_base "${LINBOOT_FLASH_BASE}")
math(EXPR _flash_end "${LINBOOT_FLASH_BASE} + ${LINBOOT_FLASH_SIZE}")
math(EXPR _app_addr "${LINBOOT_FLASH_BASE} + ${LINBOOT_BOOTLOADER_SIZE}")
math(EXPR _page "${LINBOOT_FLASH_PAGE_SIZE}")
math(EXPR _page_mask "${_page} & (${_page} - 1)")
if(NOT _page_mask EQUAL 0)
    message(FATAL_ERROR "LINBOOT_FLASH_PAGE_SIZE must be a power of two")
endif()

if(LINBOOT_META_ADDR STREQUAL "")
    math(EXPR _meta_addr "${_flash_end} - ${_page}")
else()
    linboot_check_uint(LINBOOT_META_ADDR ${_app_addr} ${_flash_end})
    math(EXPR _meta_addr "${LINBOOT_META_ADDR}")
endif()
math(EXPR _meta_offset "${_meta_addr} - ${_flash_base}")
linboot_check_multiple(_meta_offset ${_page})

# The application gets everything between the bootloader and the metadata page, or if the metadata
# page is right after the bootloader, everything after the metadata page.
math(EXPR _meta_end "${_meta_addr} + ${_page}")
if(_meta_addr EQUAL _app_addr)
    set(_app_addr ${_meta_end})
    math(EXPR _app_size "${_flash_end} - ${_app_addr}")
elseif(_meta_end EQUAL _flash_end)
    math(EXPR _app_size "${_meta_addr} - ${_app_addr}")
else()
    message(FATAL_ERROR "LINBOOT_META_ADDR must be the page right after the bootloader or the last page of the flash")
endif()
if(_app_size LESS_EQUAL 0)
    message(FATAL_ERROR "No flash left for the application")
endif()

math(EXPR LINBOOT_FLASH_END "${_flash_end}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR LINBOOT_APP_ADDR "${_app_addr}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR LINBOOT_APP_MAX_SIZE "${_app_size}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR LINBOOT_META_ADDR_RESOLVED "${_meta_addr}" OUTPUT_FORMAT HEXADECIMAL)

if(LINBOOT_BLOCK_SIZE STREQUAL "")
    if(_page GREATER 1024)
        set(_block_size 1024)
    else()
        set(_block_size ${_page})
    endif()
else()
    linboot_check_uint(LINBOOT_BLOCK_SIZE 8 ${LINBOOT_FLASH_PAGE_SIZE})
    math(EXPR _block_size "${LINBOOT_BLOCK_SIZE}")
endif()
math(EXPR _rem "${LINBOOT_FLASH_PAGE_SIZE} % ${_block_size}")
math(EXPR _rem2 "${_block_size} % ${LINBOOT_PORT_FLASH_WRITE_ALIGN}")
if(NOT _rem EQUAL 0 OR NOT _rem2 EQUAL 0)
    message(FATAL_ERROR "LINBOOT_BLOCK_SIZE must divide the page size and be a multiple of the flash write size")
endif()
set(LINBOOT_BLOCK_SIZE_RESOLVED ${_block_size})

#--------------------------------------------------------------------------------------------------
# Frame ID validation
#--------------------------------------------------------------------------------------------------

if(LINBOOT_ROLE STREQUAL "RESPONDER")
    set(_ids ${LINBOOT_ID_STATUS} ${LINBOOT_ID_CMD} ${LINBOOT_ID_DATA})
else()
    set(_ids ${LINBOOT_ID_QUERY} ${LINBOOT_ID_DATA} ${LINBOOT_PROVIDER_OFFER_IDS})
endif()
list(LENGTH LINBOOT_PROVIDER_OFFER_IDS LINBOOT_PROVIDER_COUNT)
if(LINBOOT_PROVIDER_COUNT EQUAL 0 OR LINBOOT_PROVIDER_COUNT GREATER 16)
    message(FATAL_ERROR "LINBOOT_PROVIDER_OFFER_IDS must have 1 to 16 IDs")
endif()
list(JOIN LINBOOT_PROVIDER_OFFER_IDS ", " LINBOOT_PROVIDER_OFFER_IDS_C)
# minlin frame table entries for the OFFER frames (subscribed by the COMMANDER role).
set(LINBOOT_PROVIDER_OFFER_FRAMES_C "")
foreach(i_id ${LINBOOT_PROVIDER_OFFER_IDS})
    string(APPEND LINBOOT_PROVIDER_OFFER_FRAMES_C "LINBOOT_FRAME(${i_id}, MINLIN_SUBSCRIBE), ")
endforeach()
set(_seen "")
foreach(i_id ${_ids})
    # 0x3C..0x3F are reserved for diagnostics and future use.
    linboot_check_uint(i_id 0 0x3B)
    math(EXPR _id_dec "${i_id}")
    if(_id_dec IN_LIST _seen)
        message(FATAL_ERROR "LIN frame ID ${i_id} is used more than once")
    endif()
    list(APPEND _seen ${_id_dec})
endforeach()

#--------------------------------------------------------------------------------------------------
# Booleans for linboot_config.h
#--------------------------------------------------------------------------------------------------

foreach(i_var CLASSIC_CHECKSUM VERIFY_APP_ON_BOOT)
    if(LINBOOT_${i_var})
        set(LINBOOT_${i_var}_C 1)
    else()
        set(LINBOOT_${i_var}_C 0)
    endif()
endforeach()

#--------------------------------------------------------------------------------------------------
# Summary
#--------------------------------------------------------------------------------------------------

message(STATUS "linboot: role ${LINBOOT_ROLE}, device type ${LINBOOT_DEVICE_TYPE}, ${LINBOOT_BAUD} baud, port ${LINBOOT_PORT} (${LINBOOT_MCU})")
message(STATUS "linboot: bootloader ${LINBOOT_FLASH_BASE} (${LINBOOT_BOOTLOADER_SIZE} bytes), "
    "app ${LINBOOT_APP_ADDR} (max ${LINBOOT_APP_MAX_SIZE} bytes), metadata ${LINBOOT_META_ADDR_RESOLVED}")
