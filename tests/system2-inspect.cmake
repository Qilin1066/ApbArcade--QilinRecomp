cmake_minimum_required(VERSION 3.25)

set(map "${FIXTURE_DIR}/system2.map")
set(reject_output "${FIXTURE_DIR}/system2-rejected.cpp")

execute_process(
    COMMAND "${TRANSLATOR}" --inspect-system2 "${map}" "${FIXTURE_DIR}" 0x8200
    RESULT_VARIABLE status
    OUTPUT_VARIABLE report
    ERROR_VARIABLE error
)
if(NOT status STREQUAL "0" OR NOT report MATCHES "Analyzed 57 reachable T-11 locations")
    message(FATAL_ERROR "Unexpected System 2 inspection result\n${report}\n${error}")
endif()

function(expect_failure layout entry diagnostic)
    file(REMOVE "${reject_output}")
    execute_process(
        COMMAND "${TRANSLATOR}" --system2 "${layout}" "${FIXTURE_DIR}" "${entry}" "${reject_output}"
        RESULT_VARIABLE status
        ERROR_VARIABLE error
    )
    string(FIND "${error}" "${diagnostic}" found)
    if(NOT status STREQUAL "1" OR found EQUAL -1 OR EXISTS "${reject_output}")
        message(FATAL_ERROR "Unexpected System 2 diagnostic for ${entry}\n${error}")
    endif()
endfunction()

expect_failure("${map}" 0x8240 "bank selector write to 0x1400 at 0x8240 (banks 00/00) needs an immediate source")
expect_failure("${map}" 0x8244 "unsupported System 2 word write at 0x1500 from 0x8244 (banks 00/00)")
expect_failure("${map}" 0x824a "unsupported System 2 word read at 0x1700 from 0x824A (banks 00/00)")
expect_failure("${map}" 0x8250 "unsupported System 2 byte write at 0x1801 from 0x8250 (banks 00/00)")
expect_failure("${map}" 0x8254 "unsupported System 2 word read at 0x1800 from 0x8254 (banks 00/00)")
expect_failure("${map}" 0x825a "unsupported System 2 byte write at 0x1601 from 0x825A (banks 00/00)")

set(fixed "008200 | fixed ROM-even.bin | fixed ROM-odd.bin\n")
set(invalid "${FIXTURE_DIR}/invalid-system2.map")
file(WRITE "${invalid}" "${fixed}00f000|constant-even.bin|constant-odd.bin\n010000|bank0-even.bin|bank0-odd.bin\n")
expect_failure("${invalid}" 0x8200 "missing ROM at 0x400A (banks 01/00) (physical offset 0x1200A)")

file(WRITE "${invalid}" "${fixed}008200|constant-even.bin|constant-odd.bin\n")
expect_failure("${invalid}" 0x8200 "overlapping ROM pairs at physical offset 0x08200")

file(WRITE "${invalid}" "008200|fixed ROM-even.bin|constant-odd.bin\n")
expect_failure("${invalid}" 0x8200 "mismatched byte lane sizes")

file(WRITE "${invalid}" "090000|constant-even.bin|constant-odd.bin\n")
expect_failure("${invalid}" 0x8200 "invalid offset or file name in ROM map line 1")

file(SHA256 "${map}" original)
execute_process(
    COMMAND "${TRANSLATOR}" --system2 "${map}" "${FIXTURE_DIR}" 0x8200 "${map}"
    RESULT_VARIABLE status
    ERROR_VARIABLE error
)
file(SHA256 "${map}" after)
if(NOT status STREQUAL "1" OR NOT error MATCHES "input and output must be different files" OR NOT original STREQUAL after)
    message(FATAL_ERROR "ROM map was not protected from an overlapping output path")
endif()
