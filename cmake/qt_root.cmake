# qt_root.cmake — locate a Qt installation the standard CMake way.
#
# Resolution order:
#   1. An explicit CMAKE_PREFIX_PATH / Qt6_DIR (environment, -D flag, or
#      CMakeUserPresets.json) is always respected.
#   2. Otherwise probe the default Qt installer locations and append any
#      match to CMAKE_PREFIX_PATH.
#
# Include this before anything that needs Qt (e.g. find_program(windeployqt)).

if(NOT CMAKE_PREFIX_PATH AND NOT Qt6_DIR)
    file(GLOB _Mosaic_qt_roots LIST_DIRECTORIES true
        "C:/Qt/6.*"
        "$ENV{USERPROFILE}/Qt/6.*"
        "$ENV{HOME}/Qt/6.*"
        "/opt/Qt/6.*"
    )
    foreach(_qt_root IN LISTS _Mosaic_qt_roots)
        get_filename_component(_qt_root "${_qt_root}" REALPATH)
        if(IS_DIRECTORY "${_qt_root}/msvc2022_64")
            list(APPEND CMAKE_PREFIX_PATH "${_qt_root}/msvc2022_64")
        elseif(EXISTS "${_qt_root}/lib/cmake/Qt6")
            list(APPEND CMAKE_PREFIX_PATH "${_qt_root}")
        endif()
    endforeach()
    if(CMAKE_PREFIX_PATH)
        list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
        message(STATUS "Auto-detected Qt at: ${CMAKE_PREFIX_PATH}")
    endif()
endif()
