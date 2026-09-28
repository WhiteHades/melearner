include_guard(GLOBAL)

include(FetchContent)
include(GNUInstallDirs)

# shadcn-cpp supplies every interface component. The snapshot is pinned so a
# configure cannot silently pick up a different component set or theme, and the
# application inherits the library's own neutral theme rather than a local copy.
set(SHADCN_CPP_COMMIT "4cd8fe361ebb84bb844d2c931ea98d5c1dfd018f"
    CACHE STRING "shadcn-cpp source pin")
set(SHADCN_CPP_SHA256
    "a931d6eb1e99abb758c08db49338098e43124312ce9a04c805fb082493dc2f08"
    CACHE STRING "SHA-256 of the pinned shadcn-cpp source archive")

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
  URL "https://github.com/WhiteHades/shadcn-cpp/archive/${SHADCN_CPP_COMMIT}.tar.gz"
  URL_HASH SHA256=${SHADCN_CPP_SHA256}
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(shadcn_cpp)

if(NOT TARGET shadcn::widgets)
  message(FATAL_ERROR
    "The pinned shadcn-cpp snapshot did not provide the required shadcn::widgets target")
endif()

# The component library is MIT licensed and adapts shadcn/ui, which is MIT
# licensed too. Both notices belong in the installed package, and the bundled
# Geist font carries its own OFL notice inside the library tree.
set(MELEARNER_SHADCN_LICENSE_FILES
    "${shadcn_cpp_SOURCE_DIR}/LICENSE"
    "${shadcn_cpp_SOURCE_DIR}/LICENSES/shadcn-MIT.txt"
    CACHE FILEPATH "shadcn-cpp licence files shipped with the native app")

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
