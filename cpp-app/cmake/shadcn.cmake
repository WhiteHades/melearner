include_guard(GLOBAL)

include(FetchContent)
include(GNUInstallDirs)

# shadcn-cpp supplies every interface component. The snapshot is pinned so a
# configure cannot silently pick up a different component set or theme, and the
# application inherits the library's own neutral theme rather than a local copy.
#
# Fetch the immutable archive for the pinned commit; URL_HASH verifies its content.
set(SHADCN_CPP_COMMIT "06eb8e1b17decef0db7e0eb9a76badf2359764ab")
set(SHADCN_CPP_SHA256
    "849383bdcc51a61fb839d3021860935131d7a2633824d329bb2d29c9b9c99bf5")

# Only the widget component library is used. The optional media target would add
# a Qt Multimedia dependency that this application does not use, because it plays
# media through its own embedded libmpv renderer.
set(SHADCN_BUILD_WIDGETS ON CACHE BOOL "Build the shadcn-cpp Qt widgets" FORCE)
set(SHADCN_BUILD_MEDIA OFF CACHE BOOL "Build the optional shadcn-cpp media player" FORCE)
set(SHADCN_BUILD_TESTS OFF CACHE BOOL "Build the shadcn-cpp tests" FORCE)
set(SHADCN_BUILD_EXAMPLES OFF CACHE BOOL "Build the shadcn-cpp gallery" FORCE)
set(SHADCN_SANITIZERS OFF CACHE BOOL "Enable the shadcn-cpp sanitizers" FORCE)

FetchContent_Declare(
  shadcn_cpp
  URL "https://codeload.github.com/WhiteHades/shadcn-cpp/tar.gz/${SHADCN_CPP_COMMIT}"
  URL_HASH SHA256=${SHADCN_CPP_SHA256}
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(shadcn_cpp)

if(NOT TARGET shadcn::widgets)
  message(FATAL_ERROR
    "The pinned shadcn-cpp snapshot did not provide the required shadcn::widgets target")
endif()

# Install the notices for the component library, its adapted upstream code,
# and its embedded Geist font with the application.
set(MELEARNER_SHADCN_LICENSE_FILES
    "${shadcn_cpp_SOURCE_DIR}/LICENSE"
    "${shadcn_cpp_SOURCE_DIR}/LICENSES/shadcn-MIT.txt"
    "${shadcn_cpp_SOURCE_DIR}/LICENSES/ui-components-MIT.txt"
    "${shadcn_cpp_SOURCE_DIR}/LICENSES/Geist-OFL.txt")

foreach(shadcn_license IN LISTS MELEARNER_SHADCN_LICENSE_FILES)
  if(NOT EXISTS "${shadcn_license}")
    message(FATAL_ERROR
      "The pinned shadcn-cpp snapshot is missing its licence file ${shadcn_license}")
  endif()
endforeach()

install(
  FILES ${MELEARNER_SHADCN_LICENSE_FILES}
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/melearner/shadcn-cpp"
  COMPONENT runtime)
