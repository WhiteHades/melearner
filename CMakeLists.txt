cmake_minimum_required(VERSION 4.4)
project(melearner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
include(CTest)
include(GNUInstallDirs)
find_package(Qt6 6.11.2 REQUIRED COMPONENTS Widgets OpenGLWidgets Network Pdf WebEngineWidgets Concurrent Quick)
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
include(cpp-app/cmake/mpv.cmake)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
pkg_check_modules(MD4C_HTML REQUIRED IMPORTED_TARGET md4c-html)
pkg_check_modules(THUMBNAIL_MEDIA REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
include(cpp-app/cmake/lexbor.cmake)
include(cpp-app/cmake/shadcn.cmake)

add_library(melearner_app STATIC
  cpp-app/src/single_instance.cpp
  cpp-app/src/main_window.cpp
  cpp-app/src/course_preview.cpp
  cpp-app/src/seek_feedback.cpp
  cpp-app/src/course_rows.cpp
  cpp-app/src/thumbnail_store.cpp
  cpp-app/src/study_icons.cpp
  cpp-app/src/text_interaction.cpp
  cpp-app/src/paged_list_model.cpp
  cpp-app/src/course_outline_model.cpp
  cpp-app/src/search_dialog.cpp
  cpp-app/src/stats_panel.cpp
  cpp-app/src/local_files.cpp
  cpp-app/src/pdf_reader.cpp
  cpp-app/src/pdf_view.cpp
  cpp-app/src/library.cpp
  cpp-app/src/documents.cpp
  cpp-app/src/course_document_view.cpp
  cpp-app/src/player.cpp
  cpp-app/src/mpv_video_widget.cpp
)
target_include_directories(melearner_app PUBLIC cpp-app/src)
target_link_libraries(melearner_app PUBLIC
  Qt6::Widgets Qt6::OpenGLWidgets Qt6::Network Qt6::Pdf Qt6::WebEngineWidgets Qt6::Concurrent Qt6::Quick SQLite3::SQLite3
  melearner::mpv PkgConfig::LIBZIP PkgConfig::MD4C PkgConfig::MD4C_HTML melearner::lexbor
  shadcn::widgets PkgConfig::THUMBNAIL_MEDIA)
target_compile_options(melearner_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
set_source_files_properties(cpp-app/schema/library-v1.sql PROPERTIES QT_RESOURCE_ALIAS "schema/library-v1.sql")
qt_add_resources(melearner_app assets PREFIX / FILES
  cpp-app/assets/melearner-logo.png
  cpp-app/schema/library-v1.sql)

add_executable(melearner cpp-app/src/main.cpp)
target_link_libraries(melearner PRIVATE melearner_app)
if(APPLE)
  set_target_properties(melearner PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.whitehades.melearner"
    MACOSX_BUNDLE_BUNDLE_NAME "melearner"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(WIN32)
  set_target_properties(melearner PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_target_properties(melearner PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/melearner")
  add_executable(melearner_stage_validator cpp-app/tools/validate_stage.cpp)
  target_link_libraries(melearner_stage_validator PRIVATE Qt6::Core)
  target_compile_options(melearner_stage_validator PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
endif()
target_compile_definitions(melearner PRIVATE MELEARNER_VERSION="${PROJECT_VERSION}")
if(APPLE)
  install(TARGETS melearner BUNDLE DESTINATION .)
else()
  install(TARGETS melearner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES packaging/linux/io.github.whitehades.melearner.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  # The icon goes into the hicolor theme at the sizes a desktop shell asks for.
  # Installing one oversized copy under share/pixmaps did not put it on any icon
  # search path, so the app grid showed nothing for it.
  foreach(icon_size 16 22 24 32 48 64 96 128 192 256 512)
    install(FILES
      "cpp-app/assets/icons/hicolor/${icon_size}x${icon_size}/apps/io.github.whitehades.melearner.png"
      DESTINATION
        "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps")
  endforeach()
endif()
if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.tmp/cpp-tests")
  foreach(test IN ITEMS main_window main_playback)
    add_executable(${test}_test cpp-app/tests/${test}_test.cpp)
    target_link_libraries(${test}_test PRIVATE melearner_app Qt6::Test)
    target_compile_definitions(${test}_test PRIVATE MELEARNER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
  add_test(NAME core_workflow COMMAND main_window_test)
  set(_core_workflow_platform offscreen)
  if(APPLE)
    set(_core_workflow_platform cocoa)
  elseif(WIN32)
    set(_core_workflow_platform windows)
  endif()
  set_tests_properties(core_workflow PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=${_core_workflow_platform};LC_ALL=C;TMPDIR=${CMAKE_CURRENT_SOURCE_DIR}/.tmp/cpp-tests" TIMEOUT 60)
  # The playback E2E needs a private display; the offscreen plugin has no GL.
  # scripts/test-cpp-playback.sh runs both core flows on private X11 and audio.
endif()
