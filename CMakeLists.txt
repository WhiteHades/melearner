cmake_minimum_required(VERSION 4.4)
project(melearner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
include(CTest)
include(GNUInstallDirs)
find_package(Qt6 6.11.2 REQUIRED COMPONENTS Widgets OpenGLWidgets Network Pdf)
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
include(cpp-app/cmake/lexbor.cmake)

add_library(melearner_app STATIC
  cpp-app/src/single_instance.cpp
  cpp-app/src/main_window.cpp
  cpp-app/src/paged_list_model.cpp
  cpp-app/src/search_dialog.cpp
  cpp-app/src/notes_panel.cpp
  cpp-app/src/stats_panel.cpp
  cpp-app/src/local_files.cpp
  cpp-app/src/pdf_reader.cpp
  cpp-app/src/pdf_view.cpp
  cpp-app/src/library.cpp
  cpp-app/src/documents.cpp
  cpp-app/src/player.cpp
  cpp-app/src/mpv_video_widget.cpp
)
target_include_directories(melearner_app PUBLIC cpp-app/src)
target_link_libraries(melearner_app PUBLIC
  Qt6::Widgets Qt6::OpenGLWidgets Qt6::Network Qt6::Pdf SQLite3::SQLite3 PkgConfig::MPV PkgConfig::LIBZIP PkgConfig::MD4C melearner::lexbor)
target_compile_options(melearner_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
set_source_files_properties(cpp-app/schema/library-v1.sql PROPERTIES QT_RESOURCE_ALIAS "schema/library-v1.sql")
qt_add_resources(melearner_app assets PREFIX / FILES
  cpp-app/assets/melearner-logo.png cpp-app/schema/library-v1.sql)

add_executable(melearner cpp-app/src/main.cpp)
target_link_libraries(melearner PRIVATE melearner_app)
target_compile_definitions(melearner PRIVATE MELEARNER_VERSION="${PROJECT_VERSION}")
install(TARGETS melearner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/linux/io.github.whitehades.melearner.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES cpp-app/assets/melearner-logo.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
  RENAME io.github.whitehades.melearner.png)

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.tmp/cpp-tests")
  foreach(test IN ITEMS single_instance main_window paged_list_model library player documents search_dialog notes_panel stats_panel local_files pdf)
    add_executable(${test}_test cpp-app/tests/${test}_test.cpp)
    target_link_libraries(${test}_test PRIVATE melearner_app Qt6::Test)
    add_test(NAME ${test} COMMAND ${test}_test)
    set_tests_properties(${test} PROPERTIES ENVIRONMENT
      "QT_QPA_PLATFORM=offscreen;TMPDIR=${CMAKE_CURRENT_SOURCE_DIR}/.tmp/cpp-tests" TIMEOUT 60)
  endforeach()
  # Run on a real Wayland/X11 display. Offscreen Qt cannot create the GL widget.
  foreach(test IN ITEMS playback_render main_playback)
    add_executable(${test}_test cpp-app/tests/${test}_test.cpp)
    target_link_libraries(${test}_test PRIVATE melearner_app Qt6::Test)
    target_compile_definitions(${test}_test PRIVATE MELEARNER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
  # Explicit diagnostic run: materializes the full fixture, outside routine CTest.
  add_executable(library_load_test cpp-app/tests/library_load_test.cpp
    cpp-app/tests/fixtures/parity_fixture.cpp
    cpp-app/tests/fixtures/parity_recipe_v1.cpp)
  target_link_libraries(library_load_test PRIVATE melearner_app Qt6::Test)
endif()
