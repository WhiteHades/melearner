include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
  add_library(melearner::mpv ALIAS PkgConfig::MPV)
  return()
endif()

include(ExternalProject)
include(GNUInstallDirs)
find_program(MELEARNER_MESON meson REQUIRED)
find_program(MELEARNER_PATCH patch REQUIRED)
execute_process(COMMAND "${MELEARNER_MESON}" --version
  RESULT_VARIABLE _meson_result OUTPUT_VARIABLE _meson_version
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _meson_result EQUAL 0 OR _meson_version VERSION_LESS "1.3")
  message(FATAL_ERROR "Building the Linux player runtime requires Meson 1.3 or newer")
endif()

# mpv 0.41.0 can deadlock when probing PipeWire. Keep the official startup fix
# with the runtime, rather than depending on a distributor's backport schedule.
set(_mpv_root "${CMAKE_BINARY_DIR}/_deps/mpv")
set(_mpv_prefix "${_mpv_root}/install")
set(_mpv_source "${_mpv_root}/source")
set(_mpv_patch "${_mpv_root}/pipewire-startup.patch")
set(_mpv_patch_hash "a0a53f189740c7d320e74bcbe1434b34fc8a4e9f71cff5205b0231a17aa32ec6")
file(MAKE_DIRECTORY "${_mpv_root}" "${_mpv_prefix}/include")
if(EXISTS "${_mpv_patch}")
  file(SHA256 "${_mpv_patch}" _existing_patch_hash)
endif()
if(NOT _existing_patch_hash STREQUAL _mpv_patch_hash)
  file(DOWNLOAD
    "https://github.com/mpv-player/mpv/commit/1a78f9fa8e6903bb51c9e43eefd64456a880ed13.patch"
    "${_mpv_patch}" EXPECTED_HASH "SHA256=${_mpv_patch_hash}"
    TLS_VERIFY ON TIMEOUT 60 INACTIVITY_TIMEOUT 30 STATUS _mpv_download)
  list(GET _mpv_download 0 _mpv_download_status)
  if(NOT _mpv_download_status EQUAL 0)
    message(FATAL_ERROR "Cannot download the pinned libmpv startup fix: ${_mpv_download}")
  endif()
endif()

set(_mpv_hardware_options -Dvaapi=enabled -Dvaapi-drm=enabled
    -Dvaapi-x11=enabled -Dvaapi-wayland=enabled -Dvdpau=enabled)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  list(APPEND _mpv_hardware_options -Dcuda-hwaccel=enabled -Dcuda-interop=enabled)
endif()

# Lua supplies the osc/ytdl options that the application explicitly disables.
# Keep those options available even though no player scripts run at runtime.
ExternalProject_Add(melearner_mpv_runtime
  URL "https://codeload.github.com/mpv-player/mpv/tar.gz/refs/tags/v0.41.0"
  URL_HASH SHA256=ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
  DOWNLOAD_DIR "${_mpv_root}/download"
  DOWNLOAD_NAME mpv-0.41.0.tar.gz
  SOURCE_DIR "${_mpv_source}"
  BINARY_DIR "${_mpv_root}/build"
  INSTALL_DIR "${_mpv_prefix}"
  PATCH_COMMAND "${MELEARNER_PATCH}" --batch --forward -p1 -i "${_mpv_patch}"
  CONFIGURE_COMMAND "${MELEARNER_MESON}" setup --reconfigure <BINARY_DIR> <SOURCE_DIR>
    --prefix=<INSTALL_DIR> --libdir=lib --buildtype=release --wrap-mode=nodownload
    -Dlibmpv=true -Dcplayer=false -Dgpl=true -Dbuild-date=false
    -Dpipewire=enabled -Dpulse=enabled -Dalsa=enabled
    -Dgl=enabled -Dplain-gl=enabled -Degl=enabled -Dgl-x11=enabled
    -Dx11=enabled -Dwayland=enabled -Degl-x11=enabled -Degl-wayland=enabled
    ${_mpv_hardware_options}
    -Dlua=enabled -Djavascript=disabled -Dcplugins=disabled
    -Dcdda=disabled -Ddvdnav=disabled -Ddvbin=disabled -Dlibbluray=disabled
    -Dcaca=disabled -Dmanpage-build=disabled -Dhtml-build=disabled -Dpdf-build=disabled
  BUILD_COMMAND "${MELEARNER_MESON}" compile -C <BINARY_DIR> -j 3
  INSTALL_COMMAND "${MELEARNER_MESON}" install -C <BINARY_DIR> --no-rebuild
  INSTALL_BYPRODUCTS "${_mpv_prefix}/lib/libmpv.so.2.5.0"
  USES_TERMINAL_BUILD TRUE)

add_library(melearner::mpv SHARED IMPORTED GLOBAL)
set_target_properties(melearner::mpv PROPERTIES
  IMPORTED_LOCATION "${_mpv_prefix}/lib/libmpv.so.2.5.0"
  IMPORTED_SONAME libmpv.so.2
  INTERFACE_INCLUDE_DIRECTORIES "${_mpv_prefix}/include")
add_dependencies(melearner::mpv melearner_mpv_runtime)

# The executable's private runtime path finds this copy on source installs too.
# Keep the SONAME symlink; do not replace or depend on the host's libmpv package.
install(FILES "${_mpv_prefix}/lib/libmpv.so.2.5.0" "${_mpv_prefix}/lib/libmpv.so.2"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/melearner" COMPONENT runtime)
install(FILES "${_mpv_source}/Copyright" "${_mpv_source}/LICENSE.GPL"
    "${_mpv_source}/LICENSE.LGPL"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/melearner/mpv" COMPONENT runtime)
install(FILES "${_mpv_root}/download/mpv-0.41.0.tar.gz" "${_mpv_patch}"
    "${CMAKE_CURRENT_LIST_FILE}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/melearner/sources/mpv" COMPONENT runtime)

set(MELEARNER_MPV_LIBRARY "${_mpv_prefix}/lib/libmpv.so.2.5.0"
  CACHE INTERNAL "The patched Linux libmpv runtime" FORCE)
