file(MAKE_DIRECTORY "${OUT}")
file(REMOVE "${OUT}/session.qir")
execute_process(COMMAND "${NATIVE}" "${ROM_DIR}" --frames 6000 --monkey 20261120
    --record "${OUT}/session.qir" --frame-hashes "${OUT}/record.hashes" --wav "${OUT}/record.wav"
    --profile "${OUT}/record-profile.txt"
    OUTPUT_FILE "${OUT}/record.log" ERROR_FILE "${OUT}/record.log" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Recording failed; see ${OUT}/record.log")
endif()
execute_process(COMMAND "${NATIVE}" "${ROM_DIR}" --replay "${OUT}/session.qir"
    --frame-hashes "${OUT}/replay.hashes" --wav "${OUT}/replay.wav" --audio-threaded 1
    --profile "${OUT}/replay-profile.txt"
    OUTPUT_FILE "${OUT}/replay.log" ERROR_FILE "${OUT}/replay.log" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Replay failed; see ${OUT}/replay.log")
endif()
foreach(kind hashes wav)
    file(SHA256 "${OUT}/record.${kind}" original)
    file(SHA256 "${OUT}/replay.${kind}" replay)
    if(NOT original STREQUAL replay)
        message(FATAL_ERROR "Movie changed ${kind}; see ${OUT}")
    endif()
endforeach()
file(STRINGS "${OUT}/replay.hashes" frames)
list(LENGTH frames count)
if(NOT count EQUAL 6000)
    message(FATAL_ERROR "Movie replay truncated: ${count} frames")
endif()
message(STATUS "Input movie replay: all 6000 frame hashes and the WAV are identical")
