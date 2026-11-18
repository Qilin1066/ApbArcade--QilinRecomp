cmake_minimum_required(VERSION 3.25)

function(expect_failure fixture diagnostic)
    set(output "${FIXTURE_DIR}/${fixture}.cpp")
    file(REMOVE "${output}")
    execute_process(
        COMMAND "${TRANSLATOR}" "${FIXTURE_DIR}/${fixture}.bin" 0x0200 "${output}"
        RESULT_VARIABLE status
        ERROR_VARIABLE error
    )
    string(FIND "${error}" "${diagnostic}" found)
    if(NOT status STREQUAL "1" OR found EQUAL -1 OR EXISTS "${output}")
        message(FATAL_ERROR "Unexpected result for ${fixture}: ${status}\n${error}")
    endif()
endfunction()

expect_failure("unsupported" "unsupported T-11 instruction 0xFFFF at 0x0200")
expect_failure("overlap" "branch enters operand word at 0x0202")
expect_failure("out-of-range" "word at 0x01FE is outside the input image")
