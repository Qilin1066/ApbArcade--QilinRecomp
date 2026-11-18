add_executable(apb-t11-fixtures t11_fixtures.cpp)
set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>")
set(generated "${fixtures}/loop.cpp")
set(system2_generated "${fixtures}/system2.cpp")
set(bank_switch_generated "${fixtures}/bank-switch.cpp")
set(system2_jumps "${CMAKE_CURRENT_SOURCE_DIR}/system2-jumps.map")
set(system2_pairs "fixed ROM" "unsupported-board" "constant" "bank0" "bank1" "bank1-tail" "bank2" "bank35"
    "control" "select-zero" "select-two" "jump-offsets" "switch-entry" "switch-tail" "switch-window1")
foreach(bank RANGE 0 63)
    list(APPEND system2_pairs "switch-bank${bank}")
endforeach()
set(system2_files "${fixtures}/system2.map" "${fixtures}/bank-switch.map")
foreach(pair IN LISTS system2_pairs)
    list(APPEND system2_files "${fixtures}/${pair}-even.bin" "${fixtures}/${pair}-odd.bin")
endforeach()

add_custom_command(
    OUTPUT "${generated}" "${system2_generated}" "${bank_switch_generated}"
    BYPRODUCTS "${fixtures}/loop.bin" "${fixtures}/unsupported.bin"
        "${fixtures}/overlap.bin" "${fixtures}/out-of-range.bin" ${system2_files}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixtures}"
    COMMAND apb-t11-fixtures "${fixtures}"
    COMMAND apb-t11-aot "${fixtures}/loop.bin" 0x0200 "${generated}"
    COMMAND apb-t11-aot --system2 "${fixtures}/system2.map" "${fixtures}" 0x8200 "${system2_generated}"
        --jump-tables "${system2_jumps}"
    COMMAND apb-t11-aot --system2 "${fixtures}/bank-switch.map" "${fixtures}" 0x8200 "${bank_switch_generated}"
    DEPENDS apb-t11-fixtures apb-t11-aot "${system2_jumps}"
    VERBATIM
)
add_executable(apb-t11-probe t11_probe.cpp "${generated}" "${system2_generated}")
target_link_libraries(apb-t11-probe PRIVATE apb_t11_state)
add_executable(apb-t11-bank-probe t11_bank_probe.cpp "${bank_switch_generated}")
target_link_libraries(apb-t11-bank-probe PRIVATE apb_t11_state)

add_test(NAME t11.native COMMAND apb-t11-probe)
add_test(NAME t11.system2-native COMMAND apb-t11-probe --system2)
add_test(NAME t11.system2-bank-switch COMMAND apb-t11-bank-probe)
add_test(
    NAME t11.reject-invalid-image
    COMMAND "${CMAKE_COMMAND}" "-DTRANSLATOR=$<TARGET_FILE:apb-t11-aot>"
        "-DFIXTURE_DIR=${fixtures}" -P "${CMAKE_CURRENT_SOURCE_DIR}/reject-invalid.cmake"
)
add_test(
    NAME t11.system2-inspect
    COMMAND "${CMAKE_COMMAND}" "-DTRANSLATOR=$<TARGET_FILE:apb-t11-aot>"
        "-DFIXTURE_DIR=${fixtures}" "-DJUMP_MAP=${system2_jumps}" -P "${CMAKE_CURRENT_SOURCE_DIR}/system2-inspect.cmake"
)
