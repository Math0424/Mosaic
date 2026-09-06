find_program(CLANG_FORMAT NAMES clang-format REQUIRED)

file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/src/*.hpp"
)

add_custom_target(format
    COMMAND "${CLANG_FORMAT}"
        -i
        --style=file
        ${FORMAT_SOURCES}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Formatting C++ source files"
    VERBATIM
)