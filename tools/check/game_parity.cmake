# Runs identical inputs on both lifted CPUs and on MAME's CPU cores using
# the shared board/video models. Compares every frame's pixel-colour hash.
# Registered by CTest only when QILIN_ROM_DIR and both lifts are supplied.
# All output stays in the build directory; no Vulkan, SDL window or Python.

foreach(required NATIVE REFERENCE ROM_DIR OUT SCENARIO)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "game_parity.cmake needs ${required}")
    endif()
endforeach()

set(frames 6000)
set(inputs)
if(SCENARIO STREQUAL "credited")
    # Coin up after the power-on tests; the default price is two coins.
    # Siren starts the game once the attract-mode screens can accept it;
    # a second press acknowledges the day briefing so driving is tested too.
    set(inputs --coin 900 --coin 930 --press 8:3100 --press 8:4000 --pedal 0 --steer 1)
elseif(SCENARIO STREQUAL "monkey")
    set(inputs --monkey 20261120)
elseif(SCENARIO STREQUAL "selftest")
    set(inputs --test-switch 1 --monkey 20261120)
elseif(NOT SCENARIO STREQUAL "attract")
    message(FATAL_ERROR "Unknown gameplay scenario: ${SCENARIO}")
endif()

file(MAKE_DIRECTORY "${OUT}")
# A failed command must not be able to reuse a successful earlier run.
file(REMOVE "${OUT}/native.hashes" "${OUT}/reference.hashes")
foreach(engine native reference)
    if(engine STREQUAL "native")
        set(program "${NATIVE}")
    else()
        set(program "${REFERENCE}")
    endif()
    execute_process(
        COMMAND "${program}" "${ROM_DIR}" --frames ${frames} ${inputs} --frame-hashes "${OUT}/${engine}.hashes"
        OUTPUT_FILE "${OUT}/${engine}.log"
        ERROR_FILE "${OUT}/${engine}.log"
        RESULT_VARIABLE result)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "${SCENARIO}: ${engine} failed (${result}); see ${OUT}/${engine}.log")
    endif()
    if(NOT EXISTS "${OUT}/${engine}.hashes")
        message(FATAL_ERROR "${SCENARIO}: ${engine} wrote no frame hashes; see ${OUT}/${engine}.log")
    endif()
    file(STRINGS "${OUT}/${engine}.hashes" ${engine}_frames)
    list(LENGTH ${engine}_frames count)
    if(NOT count EQUAL frames)
        message(FATAL_ERROR "${SCENARIO}: ${engine} wrote ${count} frames, expected ${frames}; see ${OUT}/${engine}.log")
    endif()
endforeach()

if(NOT "${native_frames}" STREQUAL "${reference_frames}")
    math(EXPR last "${frames} - 1")
    foreach(frame RANGE 0 ${last})
        list(GET native_frames ${frame} native_line)
        list(GET reference_frames ${frame} reference_line)
        if(NOT "${native_line}" STREQUAL "${reference_line}")
            message(FATAL_ERROR "${SCENARIO}: first difference at frame ${frame}: native [${native_line}], reference [${reference_line}]; logs and hashes in ${OUT}")
        endif()
    endforeach()
endif()
message(STATUS "${SCENARIO}: all ${frames} frames identical to MAME's CPU cores on the shared board model")
