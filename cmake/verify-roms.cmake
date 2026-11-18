cmake_minimum_required(VERSION 3.25)

if(NOT IS_DIRECTORY "${ROM_DIR}")
    message(FATAL_ERROR "Pass -DROM_DIR=<directory of unpacked APB rev 7 ROMs>.")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/apb-rev7.cmake")
set(problems "")
set(verified 0)

foreach(entry IN LISTS apb_rev7_roms)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 name)
    list(GET fields 1 expected_size)
    list(GET fields 2 expected_sha1)
    set(path "${ROM_DIR}/${name}")

    if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
        string(APPEND problems "\n  ${name} missing")
        continue()
    endif()

    file(SIZE "${path}" actual_size)
    if(NOT actual_size EQUAL expected_size)
        string(APPEND problems "\n  ${name} has ${actual_size} bytes, expected ${expected_size}")
        continue()
    endif()

    file(SHA1 "${path}" actual_sha1)
    if(NOT actual_sha1 STREQUAL expected_sha1)
        string(APPEND problems "\n  ${name} SHA-1 mismatch")
        continue()
    endif()

    math(EXPR verified "${verified} + 1")
endforeach()

list(LENGTH apb_rev7_roms total)
if(problems)
    message(FATAL_ERROR "APB rev 7 verification failed (${verified}/${total} ROMs matched).${problems}")
endif()
message(STATUS "APB rev 7 verified (${verified}/${total} ROMs matched by size and SHA-1).")
