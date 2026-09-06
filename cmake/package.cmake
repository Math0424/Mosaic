# ============================================================================
# Mosaic Windows packaging
# ============================================================================

if(NOT WIN32)
    message(FATAL_ERROR "Mosaic packaging is currently Windows-only")
endif()

if(NOT TARGET Mosaic)
    message(FATAL_ERROR "Target 'Mosaic' does not exist")
endif()

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

set(PACKAGE_DIR
    "${CMAKE_BINARY_DIR}/package"
)

set(PACKAGE_BIN
    "${PACKAGE_DIR}"
)

# ---------------------------------------------------------------------------
# Find windeployqt
# ---------------------------------------------------------------------------

find_program(WINDEPLOYQT
    NAMES windeployqt windeployqt.exe
    HINTS
        "${Qt6_DIR}/../../../bin"
        "${CMAKE_PREFIX_PATH}/bin"
)

if(NOT WINDEPLOYQT)
    message(FATAL_ERROR
        "windeployqt not found. "
        "Make sure Qt's bin directory is in PATH."
    )
endif()

message(STATUS "windeployqt: ${WINDEPLOYQT}")

# ---------------------------------------------------------------------------
# Package target
# ---------------------------------------------------------------------------

add_custom_target(package
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${PACKAGE_DIR}"

    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PACKAGE_DIR}"

    # -----------------------------------------------------------------------
    # Copy executable
    # -----------------------------------------------------------------------

    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:Mosaic>"
        "${PACKAGE_BIN}/"

    # -----------------------------------------------------------------------
    # Deploy Qt
    # -----------------------------------------------------------------------

    COMMAND "${WINDEPLOYQT}"
        --release
        --no-translations
        --no-system-d3d-compiler
        --no-opengl-sw
        --no-compiler-runtime
        "${PACKAGE_BIN}/$<TARGET_FILE_NAME:Mosaic>"

    # -----------------------------------------------------------------------
    # Copy FFmpeg/OpenCV/DuckDB
    # -----------------------------------------------------------------------

    # Copy runtime DLLs directly from the staged bin dir, which already has
    # everything (the /D defines don't survive into this script-mode run).
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "$<TARGET_FILE_DIR:Mosaic>"
        "${PACKAGE_BIN}/"

    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_BIN}/assets"

    # -----------------------------------------------------------------------
    # Assets
    # -----------------------------------------------------------------------

    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "${PACKAGE_DIR}/assets"

    COMMENT "Packaging Mosaic..."
    VERBATIM
)

# ---------------------------------------------------------------------------
# ZIP
# ---------------------------------------------------------------------------

add_custom_target(package-zip
    COMMAND ${CMAKE_COMMAND} -E tar
        "cfv"
        "${CMAKE_BINARY_DIR}/Mosaic-win-x64.zip"
        --format=zip
        "${PACKAGE_DIR}"

    DEPENDS package

    COMMENT "Creating Mosaic-win-x64.zip..."
    VERBATIM
)

# ---------------------------------------------------------------------------
# Windows installer (Inno Setup)
# ---------------------------------------------------------------------------

find_program(ISCC
    NAMES ISCC.exe iscc
    HINTS
        "C:/Program Files/Inno Setup 7"
        "C:/Program Files (x86)/Inno Setup 7"
        "C:/Program Files (x86)/Inno Setup 6"
        "C:/Program Files/Inno Setup 6"
)

if(NOT ISCC)
    message(WARNING
        "Inno Setup (ISCC) not found. The 'installer' target is disabled. "
        "Install Inno Setup from https://jrsoftware.org/isdl.php to enable it."
    )
else()
    set(INSTALLER_SCRIPT "${CMAKE_SOURCE_DIR}/installer/Mosaic.iss")
    set(INSTALLER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/installer")

    add_custom_target(installer
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${INSTALLER_OUTPUT_DIR}"

        COMMAND "${ISCC}"
            /DMyAppVersion=${PROJECT_VERSION}
            /DPackageDir=${PACKAGE_DIR}
            /DOutputDir=${INSTALLER_OUTPUT_DIR}
            "${INSTALLER_SCRIPT}"

        DEPENDS package

        COMMENT "Building Mosaic Windows installer (Inno Setup)..."
        VERBATIM
    )
endif()