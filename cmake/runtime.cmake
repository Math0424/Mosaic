find_program(WINDEPLOYQT_EXECUTABLE windeployqt
    HINTS "${Qt6Core_DIR}/../../../bin"
          "${Qt6_DIR}/../../../bin"
)
if(NOT WINDEPLOYQT_EXECUTABLE)
    message(FATAL_ERROR "windeployqt.exe not found!")
endif()

message(STATUS "windeployqt: ${WINDEPLOYQT_EXECUTABLE}")

function(deploy_Mosaic_runtime TARGET)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(WINDEPLOYQT_CONFIG --debug)
    else()
        set(WINDEPLOYQT_CONFIG --release)
    endif()

    add_custom_command(TARGET ${TARGET} POST_BUILD
        # Qt
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            ${WINDEPLOYQT_CONFIG}
            --no-translations
            --compiler-runtime
            --verbose 1
            "$<TARGET_FILE:${TARGET}>"

        # FFmpeg / OpenCV / DuckDB
        COMMAND ${CMAKE_COMMAND}
            -DFFMPEG_ROOT="${FFMPEG_ROOT}"
            -DOpenCV_DIR="${OpenCV_DIR}"
            -DUCKDB_ROOT="${DUCKDB_ROOT}"
            -DDEST_DIR="$<TARGET_FILE_DIR:${TARGET}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"

        # Assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:${TARGET}>/assets"

        COMMENT "Deploying runtime dependencies for ${TARGET}..."
    )
endfunction()