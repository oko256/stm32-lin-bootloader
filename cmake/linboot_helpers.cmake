# Small helpers for validating the configuration options.

# linboot_check_uint(<var> <min> <max>)
#
# Checks that the variable named <var> holds an unsigned integer (decimal or 0x-prefixed hex) in
# the range [min, max]; fails the configuration otherwise.
function(linboot_check_uint var min max)
    set(_value "${${var}}")
    if(NOT _value MATCHES "^(0[xX][0-9a-fA-F]+|[0-9]+)$")
        message(FATAL_ERROR "${var} must be an unsigned integer, got '${_value}'")
    endif()
    math(EXPR _dec "${_value}")
    math(EXPR _min "${min}")
    math(EXPR _max "${max}")
    if(_dec LESS _min OR _dec GREATER _max)
        message(FATAL_ERROR "${var} = ${_value} is out of range [${min}, ${max}]")
    endif()
endfunction()

# linboot_check_multiple(<var> <divisor>)
#
# Checks that the value of the variable named <var> is a multiple of <divisor>.
function(linboot_check_multiple var divisor)
    math(EXPR _rem "(${${var}}) % (${divisor})")
    if(NOT _rem EQUAL 0)
        message(FATAL_ERROR "${var} = ${${var}} must be a multiple of ${divisor}")
    endif()
endfunction()

# linboot_parse_pin(<prefix> <pin>)
#
# Parses a pin name like "PB6" into <prefix>_GPIO_INDEX (0 for port A, 1 for B, ...) and
# <prefix>_PIN (6) in the parent scope.
function(linboot_parse_pin prefix pin)
    if(NOT pin MATCHES "^P([A-K])([0-9]|1[0-5])$")
        message(FATAL_ERROR "Invalid pin name '${pin}' for ${prefix}, expected e.g. PB6")
    endif()
    string(FIND "ABCDEFGHIJK" "${CMAKE_MATCH_1}" _index)
    set(${prefix}_GPIO_INDEX ${_index} PARENT_SCOPE)
    set(${prefix}_PIN ${CMAKE_MATCH_2} PARENT_SCOPE)
endfunction()
