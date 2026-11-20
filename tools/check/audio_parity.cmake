# The worker must preserve every sample, including chip writes and speech.
foreach(required NATIVE ROM_DIR OUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "audio_parity.cmake needs ${required}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUT}")
foreach(threaded 0 1)
    file(REMOVE "${OUT}/${threaded}.wav")
    execute_process(COMMAND "${NATIVE}" "${ROM_DIR}" --frames 6000
        --coin 900 --coin 930 --press 8:3100 --press 8:4000 --pedal 0 --steer 1
        --wav "${OUT}/${threaded}.wav" --audio-threaded ${threaded}
        OUTPUT_FILE "${OUT}/${threaded}.log" ERROR_FILE "${OUT}/${threaded}.log"
        RESULT_VARIABLE result)
    if(NOT "${result}" STREQUAL "0" OR NOT EXISTS "${OUT}/${threaded}.wav")
        message(FATAL_ERROR "Audio run failed: see ${OUT}/${threaded}.log")
    endif()
    file(SIZE "${OUT}/${threaded}.wav" size)
    if(size LESS 22000000)
        message(FATAL_ERROR "Audio run is truncated: ${size} bytes")
    endif()
    file(SHA256 "${OUT}/${threaded}.wav" hash_${threaded})
endforeach()
if(NOT hash_0 STREQUAL hash_1)
    message(FATAL_ERROR "Worker changed PCM: ${OUT}/0.wav and 1.wav differ")
endif()
message(STATUS "All 6000 frames of synchronous and worker audio are byte-identical")
