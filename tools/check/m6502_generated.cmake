# Regenerates MAME's 6502 sources with MAME's own generator and checks the
# vendored copies in third_party/mame-m6502/cpu/m6502 are what it makes.
# Run by ctest as qilin_mame_m6502_generated: cmake -DPYTHON= -DSOURCE= -DOUT= -P this file.

file(MAKE_DIRECTORY "${OUT}")
foreach(pair "s;m6502.hxx" "d;m6502d.hxx")
    list(GET pair 0 mode)
    list(GET pair 1 name)
    execute_process(
        COMMAND "${PYTHON}" "${SOURCE}/m6502make.py" ${mode} m6502 "${SOURCE}/om6502.lst" "${SOURCE}/dm6502.lst" "${OUT}/${name}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "m6502make.py failed (${mode}): ${result}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol "${OUT}/${name}" "${SOURCE}/cpu/m6502/${name}"
        RESULT_VARIABLE differs)
    if(NOT differs EQUAL 0)
        message(FATAL_ERROR "third_party/mame-m6502/cpu/m6502/${name} is not what m6502make.py generates from the vendored .lst files; regenerate it (see third_party/README.md)")
    endif()
endforeach()
message(STATUS "MAME's generated 6502 sources are current")
