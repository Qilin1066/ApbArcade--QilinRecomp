# `qilin identify` on a folder and a file whose names have Japanese and
# accented letters (outside most systems' ANSI code page on Windows): the
# folder must be found and read, and the names must come back intact in the
# JSON. A name read through the ANSI code page turns into "?" and isn't found.
# Usage: cmake -DQILIN=<qilin> -DOUT=<scratch folder> -P identify_unicode.cmake
set(name "qilin 保存 été Ångström")   # UTF-8 (CMake reads scripts as UTF-8)
set(folder "${OUT}/${name}")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${folder}")

function(identify input expected_code)
    execute_process(COMMAND "${QILIN}" identify "${input}" --json
        RESULT_VARIABLE code OUTPUT_VARIABLE json ERROR_VARIABLE err ENCODING UTF-8)
    if(NOT code EQUAL expected_code)
        message(FATAL_ERROR "identify ${input}: expected exit ${expected_code}, got ${code}\n${json}${err}")
    endif()
    foreach(text IN LISTS ARGN)
        string(FIND "${json}" "${text}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "identify ${input}: no \"${text}\" in\n${json}")
        endif()
    endforeach()
endfunction()

# One file of the set, with the wrong contents: the folder was read.
file(WRITE "${folder}/136051-1101.5t" "not really")
identify("${folder}" 2 "\"ok\": false" "35 of its 36 files are missing" "136051-1101.5t")

# A zipped set chosen instead of its folder: its name is shown as it is.
file(WRITE "${folder}/${name}.zip" "not a zip")
identify("${folder}/${name}.zip" 2 "\"ok\": false" "${name}.zip is a .zip file")

# A folder that isn't there, for contrast.
identify("${folder} missing" 2 "\"ok\": false" "Can't find ${folder} missing.")

file(REMOVE_RECURSE "${OUT}")
