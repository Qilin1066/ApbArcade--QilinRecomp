add_executable(apb-t11-fixtures t11_fixtures.cpp)
set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>")
set(generated "${fixtures}/loop.cpp")

add_custom_command(
    OUTPUT "${generated}"
    BYPRODUCTS "${fixtures}/loop.bin" "${fixtures}/unsupported.bin"
        "${fixtures}/overlap.bin" "${fixtures}/out-of-range.bin"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixtures}"
    COMMAND apb-t11-fixtures "${fixtures}"
    COMMAND apb-t11-aot "${fixtures}/loop.bin" 0x0200 "${generated}"
    DEPENDS apb-t11-fixtures apb-t11-aot
    VERBATIM
)
add_executable(apb-t11-probe t11_probe.cpp "${generated}")
target_link_libraries(apb-t11-probe PRIVATE apb_t11_state)

add_test(NAME t11.native COMMAND apb-t11-probe)
add_test(
    NAME t11.reject-invalid-image
    COMMAND "${CMAKE_COMMAND}" "-DTRANSLATOR=$<TARGET_FILE:apb-t11-aot>"
        "-DFIXTURE_DIR=${fixtures}" -P "${CMAKE_CURRENT_SOURCE_DIR}/reject-invalid.cmake"
)
