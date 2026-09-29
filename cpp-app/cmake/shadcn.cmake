include_guard(GLOBAL)

include(FetchContent)
include(GNUInstallDirs)

# shadcn-cpp supplies every interface component. The snapshot is pinned so a
# configure cannot silently pick up a different component set or theme, and the
# application inherits the library's own neutral theme rather than a local copy.
#
# The archive is fetched by branch name and its SHA-256 checked against the pin,
# which is what makes the pin binding. Fetching by commit id instead reads better
# and is not available: a commit pushed moments ago has no archive yet, and a
# configure that names one fails for a reason that has nothing to do with the
# build. So the name is fixed and the content is pinned, and a commit that is not
# on the branch yet fails the hash rather than downloading something else.
set(SHADCN_CPP_COMMIT "06eb8e1b17decef0db7e0eb9a76badf2359764ab"
    CACHE STRING "shadcn-cpp source pin")
set(SHADCN_CPP_SHA256
    "c24ab31eb3107b2f948431c0144596d3061b96a887395f3d73073e6dd2cf648a"
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
  URL "https://github.com/WhiteHades/shadcn-cpp/archive/refs/heads/main.tar.gz"
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
