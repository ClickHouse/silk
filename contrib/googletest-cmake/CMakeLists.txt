get_directory_property(_opts COMPILE_OPTIONS)
list(FILTER _opts EXCLUDE REGEX "^-W")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_opts}")

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../googletest
    ${CMAKE_CURRENT_BINARY_DIR}/googletest
    SYSTEM
)

foreach(_gt gtest gtest_main gmock gmock_main)
    if(TARGET ${_gt})
        # gtest's own cmake adds -Wconversion which triggers -Wcharacter-conversion
        # on the char8_t -> char32_t cast in gtest-printers.h:524 (googletest bug).
        target_compile_options(${_gt} PRIVATE -Wno-character-conversion)
        # MSan: cxx must follow googletest in the link order so that template
        # references instantiated inside googletest (e.g. regex_error) resolve.
        if(TARGET cxx)
            target_link_libraries(${_gt} PRIVATE cxx)
        endif()
    endif()
endforeach()
