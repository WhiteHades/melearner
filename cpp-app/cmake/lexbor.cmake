include_guard(GLOBAL)

include(FetchContent)
include(GNUInstallDirs)

# Lexbor is used only as the HTML DOM parser. Keep the source and version
# immutable so a configure cannot silently select a different parser build.
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "Build Lexbor shared library" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "Build Lexbor static library" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "Build Lexbor examples" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "Build Lexbor tests" FORCE)
set(LEXBOR_BUILD_TESTS_CPP OFF CACHE BOOL "Build Lexbor C++ tests" FORCE)
set(LEXBOR_BUILD_UTILS OFF CACHE BOOL "Build Lexbor utilities" FORCE)
set(LEXBOR_BUILD_BENCHMARKS OFF CACHE BOOL "Build Lexbor benchmarks" FORCE)
set(LEXBOR_BUILD_WASM OFF CACHE BOOL "Build Lexbor WebAssembly" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "Install Lexbor headers" FORCE)

FetchContent_Declare(
  lexbor
  URL https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz
  URL_HASH SHA256=eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(lexbor)

if(TARGET lexbor_static AND NOT TARGET melearner::lexbor)
  set_target_properties(lexbor_static PROPERTIES AUTOMOC OFF)
  add_library(melearner::lexbor ALIAS lexbor_static)
else()
  message(FATAL_ERROR "Lexbor 3.0.0 did not provide the required static target")
endif()

set(MELEARNER_LEXBOR_LICENSE_FILE
    "${lexbor_SOURCE_DIR}/LICENSE"
    CACHE FILEPATH "Lexbor Apache-2.0 license shipped with the native app")
set(MELEARNER_LEXBOR_NOTICE_FILE
    "${lexbor_SOURCE_DIR}/NOTICE"
    CACHE FILEPATH "Lexbor NOTICE shipped with the native app")

if(NOT EXISTS "${MELEARNER_LEXBOR_LICENSE_FILE}" OR
   NOT EXISTS "${MELEARNER_LEXBOR_NOTICE_FILE}")
  message(FATAL_ERROR "Lexbor 3.0.0 LICENSE/NOTICE files are missing")
endif()

# Keep the dependency notices in the installed package. The source archive's
# Apache-2.0 license and NOTICE are the authoritative files; no copied text is
# maintained in the application tree.
install(
  FILES "${MELEARNER_LEXBOR_LICENSE_FILE}" "${MELEARNER_LEXBOR_NOTICE_FILE}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/melearner/lexbor"
  COMPONENT runtime)
