# 
# Incrementing build number (persisted counter -> generated header)
# 

set(Mosaic_BUILD_NUMBER_FILE "${CMAKE_SOURCE_DIR}/build_number.txt")
set(Mosaic_BUILD_NUMBER_HEADER "${CMAKE_BINARY_DIR}/generated/BuildNumber.h")

add_custom_target(Mosaic_build_number ALL
    COMMAND ${CMAKE_COMMAND}
        "-DBUILD_NUMBER_FILE=${Mosaic_BUILD_NUMBER_FILE}"
        "-DOUTPUT_HEADER=${Mosaic_BUILD_NUMBER_HEADER}"
        -P "${CMAKE_SOURCE_DIR}/cmake/build_number.cmake"
    BYPRODUCTS "${Mosaic_BUILD_NUMBER_HEADER}"
    COMMENT "Bumping build number"
    VERBATIM
)

add_executable(Mosaic
    main.cpp
    "${CMAKE_SOURCE_DIR}/app.rc"
    "${CMAKE_SOURCE_DIR}/assets/assets.qrc"
    app/InstanceGuard.h
    app/InstanceGuard.cpp
    app/MainWindow.h
    app/MainWindow.cpp
    app/Theme.h
    app/Theme.cpp
    app/Verification.h
    app/Verification.cpp
    components/ConflictsPage.h
    components/ConflictsPage.cpp
    components/FavoritesPage.h
    components/FavoritesPage.cpp
    components/FlowLayout.h
    components/FlowLayout.cpp
    components/ImportPage.h
    components/ImportPage.cpp
    components/MultiSourceDialog.h
    components/MultiSourceDialog.cpp
    components/SettingsDialog.h
    components/SettingsDialog.cpp
    components/SourceBrowser.h
    components/SourceBrowser.cpp
    components/TagMergePage.h
    components/TagMergePage.cpp
    components/TagSidebar.h
    components/TagSidebar.cpp
    components/ViewerWidget.h
    components/ViewerWidget.cpp
    components/MediaPlaybackController.h
    components/MediaPlaybackController.cpp    components/MediaCache.h
    components/MediaCache.cpp    components/ViewerWindow.h
    components/ViewerWindow.cpp
    media/VideoDecoder.h
    media/VideoDecoder.cpp
    media/ImageFingerprint.h
    media/ImageFingerprint.cpp
    core/AppState.h
    core/AppState.cpp
    core/Database.h
    core/Database.cpp
    core/FileLogger.h
    core/FileLogger.cpp
    core/Paths.h
    core/Paths.cpp
    core/Schema.h
    core/Schema.cpp
    feed/FeedDatabase.h
    feed/FeedDatabase.cpp
    feed/FeedRecommender.h
    feed/FeedRecommender.cpp
    feed/FeedModel.h
    feed/FeedModel.cpp
    feed/FeedWarmStart.h
    feed/FeedWarmStart.cpp
    feed/VisualFeatures.h
    feed/VisualFeatures.cpp
    feed/FeedPage.h
    feed/FeedPage.cpp
    sources/Source.h
    sources/SourceManager.h
    sources/SourceManager.cpp
    sources/MultiSource.h
    sources/MultiSource.cpp
    sources/web/HttpSource.h
    sources/web/HttpSource.cpp
    sources/web/E621Source.h
    sources/web/E621Source.cpp
    sources/web/GelbooruSource.h
    sources/web/GelbooruSource.cpp
    sources/web/R34Source.h
    sources/web/R34Source.cpp
    sources/web/Rule34XxxSource.h
    sources/web/Rule34XxxSource.cpp
    sources/local/LibrarySql.h
    sources/local/LibrarySql.cpp
    sources/local/LibraryCatalog.h
    sources/local/LibraryCatalog.cpp
    sources/local/LibraryTagService.h
    sources/local/LibraryTagService.cpp
    sources/local/DuplicateDetector.h
    sources/local/DuplicateDetector.cpp
    sources/local/LibraryImporter.h
    sources/local/LibraryImporter.cpp
    sources/local/LibraryUtils.h
    sources/local/LibraryUtils.cpp
    sources/local/LibrarySource.h
    sources/local/LibrarySource.cpp
)

# Option to build a publishable executable without a console window
option(PUBLISH_NO_CONSOLE "Remove console window for publish builds" OFF)

# The build-number header must exist before Mosaic compiles.
add_dependencies(Mosaic Mosaic_build_number)
target_include_directories(Mosaic PRIVATE "${CMAKE_BINARY_DIR}/generated")

target_link_libraries(Mosaic PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Multimedia
    ${OpenCV_LIBS}
    ffmpeg
    duckdb
)

# OpenCV/FFmpeg headers can warn; keep our code strict instead
if(MSVC)
    target_compile_options(Mosaic PRIVATE /W4 /permissive- /external:W0)
    target_compile_options(Mosaic PRIVATE $<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/GL>)
    
    # Modern CMake equivalent for link flags:
    target_link_options(Mosaic PRIVATE 
        $<$<CONFIG:Release>:/LTCG> 
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
    )
endif()


# Deploy runtime (Qt + FFmpeg/OpenCV/DuckDB) and assets
deploy_Mosaic_runtime(Mosaic)

if(PUBLISH_NO_CONSOLE)
    # When publishing, build a GUI subsystem executable (no console window).
    set_target_properties(Mosaic PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Guarantee the exact duckdb.dll matching the linked .lib ends up next to
# the executable (the generic runtime glob covers it as well).
add_custom_command(TARGET Mosaic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/libs/duckdb/duckdb.dll"
        "$<TARGET_FILE_DIR:Mosaic>/duckdb.dll"
    COMMENT "Copying duckdb.dll"
)
