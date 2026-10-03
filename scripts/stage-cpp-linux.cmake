cmake_minimum_required(VERSION 4.4)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "C++ Linux staging is only supported on Linux")
endif()

set(_required_variables
  MELEARNER_SOURCE_DIR
  MELEARNER_BUILD_DIR
  MELEARNER_STAGE_DIR
  MELEARNER_LEGAL_ROOT)
foreach(_variable IN LISTS _required_variables)
  if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_variable} is required")
  endif()
  if(NOT IS_ABSOLUTE "${${_variable}}")
    message(FATAL_ERROR "${_variable} must be an absolute path")
  endif()
endforeach()

if(NOT DEFINED MELEARNER_VERSION OR "${MELEARNER_VERSION}" STREQUAL "")
  set(MELEARNER_VERSION "0.1.9")
endif()
if(NOT MELEARNER_VERSION STREQUAL "0.1.9")
  message(FATAL_ERROR "melearner release version is fixed at 0.1.9, got ${MELEARNER_VERSION}")
endif()

if(NOT IS_DIRECTORY "${MELEARNER_SOURCE_DIR}")
  message(FATAL_ERROR "source directory is missing: ${MELEARNER_SOURCE_DIR}")
endif()
if(NOT IS_DIRECTORY "${MELEARNER_BUILD_DIR}")
  message(FATAL_ERROR "build directory is missing: ${MELEARNER_BUILD_DIR}")
endif()
if(NOT EXISTS "${MELEARNER_BUILD_DIR}/CMakeCache.txt")
  message(FATAL_ERROR "build directory has no CMakeCache.txt: ${MELEARNER_BUILD_DIR}")
endif()
if(NOT IS_DIRECTORY "${MELEARNER_LEGAL_ROOT}")
  message(FATAL_ERROR "legal input directory is missing: ${MELEARNER_LEGAL_ROOT}")
endif()

file(READ "${MELEARNER_SOURCE_DIR}/CMakeLists.txt" _root_cmake)
string(REGEX MATCH
  "project[ \\t\\r\\n]*\\([ \\t\\r\\n]*melearner[ \\t\\r\\n]+VERSION[ \\t\\r\\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
  _project_match "${_root_cmake}")
if(NOT _project_match OR NOT CMAKE_MATCH_1 STREQUAL "0.1.9")
  message(FATAL_ERROR "CMake project version must be 0.1.9")
endif()
file(STRINGS "${MELEARNER_BUILD_DIR}/CMakeCache.txt" _cache_version_lines
  REGEX "^CMAKE_PROJECT_VERSION:STATIC=")
if(NOT _cache_version_lines)
  message(FATAL_ERROR "configured CMake build version must be 0.1.9")
endif()
list(GET _cache_version_lines 0 _cache_version_line)
string(REGEX REPLACE "^CMAKE_PROJECT_VERSION:STATIC=" "" _cache_version
  "${_cache_version_line}")
if(NOT _cache_version STREQUAL "0.1.9")
  message(FATAL_ERROR "configured CMake build version must be 0.1.9")
endif()

file(GLOB _existing_stage_entries RELATIVE "${MELEARNER_STAGE_DIR}" "${MELEARNER_STAGE_DIR}/*")
if(_existing_stage_entries)
  message(FATAL_ERROR "stage directory must be empty: ${MELEARNER_STAGE_DIR}")
endif()
file(MAKE_DIRECTORY "${MELEARNER_STAGE_DIR}")
set(_install_prefix "${MELEARNER_STAGE_DIR}/usr")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${MELEARNER_BUILD_DIR}"
          --prefix "${_install_prefix}" --config Release
  RESULT_VARIABLE _install_result
  OUTPUT_VARIABLE _install_output
  ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR
    "CMake install failed (${_install_result}):\n${_install_output}${_install_error}")
endif()

function(_require_file _path _label)
  if(NOT EXISTS "${_path}" OR IS_DIRECTORY "${_path}" OR IS_SYMLINK "${_path}")
    message(FATAL_ERROR "missing ${_label}: ${_path}")
  endif()
  file(SIZE "${_path}" _size)
  if(_size LESS 1)
    message(FATAL_ERROR "empty ${_label}: ${_path}")
  endif()
endfunction()

set(_project_license "${MELEARNER_SOURCE_DIR}/LICENSE")
set(_third_party_notices "${MELEARNER_LEGAL_ROOT}/THIRD_PARTY_NOTICES")
set(_spdx_manifest "${MELEARNER_LEGAL_ROOT}/melearner.spdx.json")
set(_runtime_lock "${MELEARNER_LEGAL_ROOT}/runtime-lock.json")
set(_reference_profiles "${MELEARNER_LEGAL_ROOT}/reference-profiles-v1.json")
_require_file("${_project_license}" "project license")
_require_file("${_third_party_notices}" "third-party notices")
_require_file("${_spdx_manifest}" "SPDX manifest")
_require_file("${_runtime_lock}" "runtime lock")
_require_file("${_reference_profiles}" "reference profiles")

function(_require_json_object _path _label)
  file(READ "${_path}" _json_contents)
  string(JSON _json_type ERROR_VARIABLE _json_error TYPE "${_json_contents}")
  if(_json_error OR NOT _json_type STREQUAL "OBJECT")
    message(FATAL_ERROR "${_label} must be a JSON object: ${_json_error}")
  endif()
endfunction()
_require_json_object("${_spdx_manifest}" "SPDX manifest")
_require_json_object("${_runtime_lock}" "runtime lock")
_require_json_object("${_reference_profiles}" "reference profiles")

set(_license_dir "${_install_prefix}/share/licenses/melearner")
set(_doc_dir "${_install_prefix}/share/doc/melearner")
file(MAKE_DIRECTORY "${_license_dir}" "${_doc_dir}")
file(COPY_FILE "${_project_license}" "${_license_dir}/LICENSE")
file(COPY_FILE "${_third_party_notices}" "${_doc_dir}/THIRD_PARTY_NOTICES")
file(COPY_FILE "${_spdx_manifest}" "${_doc_dir}/melearner.spdx.json")
file(COPY_FILE "${_runtime_lock}" "${_doc_dir}/runtime-lock.json")
file(COPY_FILE "${_reference_profiles}" "${_doc_dir}/reference-profiles-v1.json")

foreach(_lexbor_file IN ITEMS LICENSE NOTICE)
  _require_file(
    "${_license_dir}/lexbor/${_lexbor_file}"
    "Lexbor ${_lexbor_file}")
endforeach()

set(_binary "${_install_prefix}/bin/melearner")
set(_desktop "${_install_prefix}/share/applications/io.github.whitehades.melearner.desktop")
set(_icon "${_install_prefix}/share/icons/hicolor/512x512/apps/io.github.whitehades.melearner.png")
_require_file("${_binary}" "installed melearner executable")
_require_file("${_desktop}" "desktop launcher")
_require_file("${_icon}" "application icon")

find_program(_file_tool NAMES file)
find_program(_readelf_tool NAMES readelf)
find_program(_strip_tool NAMES strip REQUIRED)
function(_validate_patchelf _result _candidate)
  execute_process(
    COMMAND "${_candidate}" --version
    RESULT_VARIABLE _candidate_result
    OUTPUT_VARIABLE _candidate_output)
  string(REGEX MATCH "patchelf[ \\t]+([0-9]+\\.[0-9]+\\.[0-9]+)"
    _candidate_match "${_candidate_output}")
  if(NOT _candidate_result EQUAL 0 OR NOT _candidate_match
      OR CMAKE_MATCH_1 VERSION_LESS "0.19.1")
    set(${_result} FALSE PARENT_SCOPE)
  endif()
endfunction()
if(DEFINED MELEARNER_PATCHELF AND NOT "${MELEARNER_PATCHELF}" STREQUAL "")
  set(_patchelf_tool "${MELEARNER_PATCHELF}")
else()
  find_program(_patchelf_tool NAMES patchelf VALIDATOR _validate_patchelf NO_CACHE)
endif()
if(NOT _file_tool OR NOT _readelf_tool OR NOT _patchelf_tool)
  message(FATAL_ERROR "file, readelf, and patchelf are required for C++ Linux staging")
endif()
execute_process(
  COMMAND "${_patchelf_tool}" --version
  RESULT_VARIABLE _patchelf_version_result
  OUTPUT_VARIABLE _patchelf_version_output
  ERROR_VARIABLE _patchelf_version_error)
if(NOT _patchelf_version_result EQUAL 0)
  message(FATAL_ERROR
    "could not query patchelf version: ${_patchelf_version_error}")
endif()
string(REGEX MATCH "patchelf[ \\t]+([0-9]+\\.[0-9]+\\.[0-9]+)"
  _patchelf_version_match "${_patchelf_version_output}")
if(NOT _patchelf_version_match OR CMAKE_MATCH_1 VERSION_LESS "0.19.1")
  message(FATAL_ERROR
    "patchelf >= 0.19.1 is required; selected ${_patchelf_tool} reports: ${_patchelf_version_output}")
endif()

execute_process(
  COMMAND "${_file_tool}" -b "${_binary}"
  RESULT_VARIABLE _file_result
  OUTPUT_VARIABLE _file_description
  ERROR_VARIABLE _file_error)
if(NOT _file_result EQUAL 0 OR NOT _file_description MATCHES "^ELF 64-bit.*x86-64")
  message(FATAL_ERROR "melearner must be an x86_64 ELF executable: ${_file_description}${_file_error}")
endif()

file(READ "${_desktop}" _desktop_contents)
file(STRINGS "${_desktop}" _desktop_lines)
list(FIND _desktop_lines "Exec=melearner %F" _desktop_exec_index)
if(_desktop_exec_index EQUAL -1)
  message(FATAL_ERROR "desktop launcher must execute melearner with folder arguments")
endif()
list(FIND _desktop_lines "Icon=io.github.whitehades.melearner" _desktop_icon_index)
if(_desktop_icon_index EQUAL -1)
  message(FATAL_ERROR "desktop launcher has an unexpected icon")
endif()
string(TOLOWER "${_desktop_contents}" _desktop_lower)
if(_desktop_lower MATCHES "(^|[^a-z0-9])(tauri|native-app|node|nodejs|zig|rust|webview|webengine|qml|electron)([^a-z0-9]|$)")
  message(FATAL_ERROR "desktop launcher references a superseded runtime")
endif()

if(DEFINED MELEARNER_QT_PLUGIN_DIR AND NOT "${MELEARNER_QT_PLUGIN_DIR}" STREQUAL "")
  set(_qt_plugin_dir "${MELEARNER_QT_PLUGIN_DIR}")
else()
  find_program(_qtpaths_tool NAMES qtpaths6 qtpaths
    PATHS
      /usr/lib/qt6/bin
      /usr/lib/qt6/libexec
      /usr/lib/x86_64-linux-gnu/qt6/bin
      /usr/lib/x86_64-linux-gnu/qt6/libexec)
  if(NOT _qtpaths_tool)
    message(FATAL_ERROR "qtpaths6 or qtpaths is required to locate Qt plugins")
  endif()
  execute_process(
    COMMAND "${_qtpaths_tool}" --plugin-dir
    RESULT_VARIABLE _qtpaths_result
    OUTPUT_VARIABLE _qt_plugin_dir
    ERROR_VARIABLE _qtpaths_error)
  if(NOT _qtpaths_result EQUAL 0)
    message(FATAL_ERROR "could not query the Qt plugin directory: ${_qtpaths_error}")
  endif()
  string(STRIP "${_qt_plugin_dir}" _qt_plugin_dir)
endif()
if(NOT IS_DIRECTORY "${_qt_plugin_dir}")
  message(FATAL_ERROR "Qt plugin directory is missing: ${_qt_plugin_dir}")
endif()

if(DEFINED MELEARNER_QTPATHS AND NOT "${MELEARNER_QTPATHS}" STREQUAL "")
  set(_qtpaths_tool "${MELEARNER_QTPATHS}")
else()
  find_program(_qtpaths_tool NAMES qtpaths6 qtpaths
    PATHS /usr/lib/qt6/bin /usr/lib/x86_64-linux-gnu/qt6/bin)
endif()
if(NOT _qtpaths_tool)
  message(FATAL_ERROR "qtpaths6 or qtpaths is required to locate Qt WebEngine runtime files")
endif()
execute_process(COMMAND "${_qtpaths_tool}" --query
  RESULT_VARIABLE _qtpaths_query_result OUTPUT_VARIABLE _qtpaths_query
  ERROR_VARIABLE _qtpaths_query_error)
if(NOT _qtpaths_query_result EQUAL 0)
  message(FATAL_ERROR "could not query Qt installation paths: ${_qtpaths_query_error}")
endif()
string(REPLACE "\r" "" _qtpaths_query "${_qtpaths_query}")
string(REPLACE "\n" ";" _qtpaths_lines "${_qtpaths_query}")
foreach(_qt_path_key IN ITEMS QT_INSTALL_PREFIX QT_INSTALL_LIBEXECS QT_INSTALL_DATA QT_INSTALL_TRANSLATIONS)
  set(_qt_path_value "")
  foreach(_qtpaths_line IN LISTS _qtpaths_lines)
    if(_qtpaths_line MATCHES "^${_qt_path_key}:(.+)$")
      set(_qt_path_value "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()
  if(_qt_path_value STREQUAL "")
    message(FATAL_ERROR "qtpaths did not report ${_qt_path_key}")
  endif()
  set("_${_qt_path_key}" "${_qt_path_value}")
endforeach()
set(_webengine_helper "${_QT_INSTALL_LIBEXECS}/QtWebEngineProcess")
set(_webengine_resource_dir "${_QT_INSTALL_DATA}/resources")
set(_webengine_locale_dir "${_QT_INSTALL_TRANSLATIONS}/qtwebengine_locales")
set(_webengine_notice_candidates
  "${_QT_INSTALL_PREFIX}/licenses/QtWebEngine/LICENSE.chromium"
  "${_QT_INSTALL_PREFIX}/share/licenses/qt6-webengine/LICENSE.chromium"
  "${_QT_INSTALL_PREFIX}/share/licenses/qtwebengine/LICENSE.chromium"
  "${_QT_INSTALL_DATA}/../licenses/qt6-webengine/LICENSE.chromium")
set(_webengine_notice "")
foreach(_candidate IN LISTS _webengine_notice_candidates)
  if(EXISTS "${_candidate}" AND NOT IS_DIRECTORY "${_candidate}" AND NOT IS_SYMLINK "${_candidate}")
    file(SIZE "${_candidate}" _candidate_size)
    if(_candidate_size GREATER 0)
      set(_webengine_notice "${_candidate}")
      break()
    endif()
  endif()
endforeach()
if(_webengine_notice STREQUAL "")
  message(FATAL_ERROR "Qt WebEngine provider notice LICENSE.chromium is missing below Qt prefix ${_QT_INSTALL_PREFIX}")
endif()
set(_webengine_doc_dir "${_doc_dir}/qtwebengine")
file(MAKE_DIRECTORY "${_webengine_doc_dir}")
file(COPY_FILE "${_webengine_notice}" "${_webengine_doc_dir}/LICENSE.chromium")
_require_file("${_webengine_doc_dir}/LICENSE.chromium" "Qt WebEngine provider notice")
foreach(_webengine_file IN ITEMS
    "${_webengine_resource_dir}/qtwebengine_resources.pak"
    "${_webengine_resource_dir}/qtwebengine_resources_100p.pak"
    "${_webengine_resource_dir}/qtwebengine_resources_200p.pak"
    "${_webengine_resource_dir}/v8_context_snapshot.bin")
  _require_file("${_webengine_file}" "Qt WebEngine resource")
endforeach()
set(_webengine_has_icudtl FALSE)
if(EXISTS "${_webengine_resource_dir}/icudtl.dat")
  _require_file("${_webengine_resource_dir}/icudtl.dat" "Qt WebEngine ICU data")
  set(_webengine_has_icudtl TRUE)
endif()
if(NOT IS_DIRECTORY "${_webengine_locale_dir}")
  message(FATAL_ERROR "Qt WebEngine locales are missing: ${_webengine_locale_dir}")
endif()
file(GLOB _webengine_locale_files "${_webengine_locale_dir}/*.pak")
if(NOT _webengine_locale_files)
  message(FATAL_ERROR "Qt WebEngine locales are empty: ${_webengine_locale_dir}")
endif()
_require_file("${_webengine_helper}" "QtWebEngineProcess helper")

set(_plugin_root "${_install_prefix}/lib/qt6/plugins")

# Keep the input hash before RPATH changes so a bundled file can be matched to
# its build or package payload without trying to reverse patchelf's ELF edits.
set_property(GLOBAL PROPERTY _runtime_origin_paths "")
function(_remember_runtime_origin _source _destination _kind)
  file(REAL_PATH "${_source}" _source_real)
  file(REAL_PATH "${_destination}" _destination_real)
  file(RELATIVE_PATH _package_path "${MELEARNER_STAGE_DIR}" "${_destination_real}")
  if(IS_ABSOLUTE "${_package_path}" OR _package_path MATCHES "^\\.\\.(/|$)")
    message(FATAL_ERROR "runtime inventory path escapes the stage: ${_destination_real}")
  endif()
  file(SHA256 "${_source_real}" _source_hash)
  file(SHA256 "${_destination_real}" _copied_hash)
  if(NOT _source_hash STREQUAL _copied_hash)
    message(FATAL_ERROR "runtime copy differs from its input: ${_package_path}")
  endif()
  string(SHA256 _key "${_package_path}")
  get_property(_known GLOBAL PROPERTY "_runtime_origin_${_key}_sha256" SET)
  if(_known)
    get_property(_previous_hash GLOBAL PROPERTY "_runtime_origin_${_key}_sha256")
    if(NOT _previous_hash STREQUAL _source_hash)
      message(FATAL_ERROR "conflicting runtime inputs target ${_package_path}")
    endif()
    return()
  endif()
  get_filename_component(_source_name "${_source_real}" NAME)
  set_property(GLOBAL APPEND PROPERTY _runtime_origin_paths "${_package_path}")
  set_property(GLOBAL PROPERTY "_runtime_origin_${_key}_sha256" "${_source_hash}")
  set_property(GLOBAL PROPERTY "_runtime_origin_${_key}_name" "${_source_name}")
  set_property(GLOBAL PROPERTY "_runtime_origin_${_key}_kind" "${_kind}")
endfunction()
_remember_runtime_origin("${_binary}" "${_binary}" "application-build")

# Keep the runtime closure tied to the features the C++ application actually
# uses.  Qt's PNG reader/writer is built into QtGui on the target Qt package;
# the remaining accepted local image formats are plugins.  Accessibility is a
# QtGui facility, not a plugin group.  The XDG portal theme is intentionally
# left to the host: QFileDialog and QDesktopServices have native fallbacks and
# this application does not link QtDBus directly.
set(_required_qt_plugins
  "platforms|libqxcb.so"
  "platforms|libqwayland.so"
  "platforminputcontexts|libcomposeplatforminputcontextplugin.so"
  "platforminputcontexts|libibusplatforminputcontextplugin.so"
  "imageformats|libqjpeg.so"
  "imageformats|libqgif.so"
  "imageformats|libqwebp.so"
  "xcbglintegrations|libqxcb-egl-integration.so"
  "xcbglintegrations|libqxcb-glx-integration.so"
  "wayland-shell-integration|libxdg-shell.so"
  "wayland-graphics-integration-client|libqt-plugin-wayland-egl.so")
set(_plugin_sources)
set(_plugin_group_names)
function(_stage_qt_plugin _source_root _destination_root _group _name
    _sources_output _groups_output)
  set(_source "${_source_root}/${_group}/${_name}")
  if(NOT EXISTS "${_source}" OR IS_DIRECTORY "${_source}")
    message(FATAL_ERROR "required Qt plugin is missing: ${_source}")
  endif()
  set(_destination_group "${_destination_root}/${_group}")
  file(MAKE_DIRECTORY "${_destination_group}")
  file(COPY "${_source}" DESTINATION "${_destination_group}" FOLLOW_SYMLINK_CHAIN)
  _remember_runtime_origin("${_source}" "${_destination_group}/${_name}" "qt-plugin-provider")
  set(_sources "${${_sources_output}}")
  list(APPEND _sources "${_source}")
  set(${_sources_output} "${_sources}" PARENT_SCOPE)
  set(_groups "${${_groups_output}}")
  list(APPEND _groups "${_group}")
  set(${_groups_output} "${_groups}" PARENT_SCOPE)
endfunction()

foreach(_plugin_spec IN LISTS _required_qt_plugins)
  string(REPLACE "|" ";" _plugin_parts "${_plugin_spec}")
  list(GET _plugin_parts 0 _group)
  list(GET _plugin_parts 1 _name)
  _stage_qt_plugin("${_qt_plugin_dir}" "${_plugin_root}" "${_group}" "${_name}"
    _plugin_sources _plugin_group_names)
endforeach()

file(WRITE "${_install_prefix}/bin/qt.conf"
  "[Paths]\nPrefix=..\nPlugins=lib/qt6/plugins\nLibraries=lib\nLibraryExecutables=libexec\nData=share/qt6\nTranslations=share/qt6/translations\n")
file(MAKE_DIRECTORY "${_install_prefix}/libexec" "${_install_prefix}/share/qt6/resources" "${_install_prefix}/share/qt6/translations/qtwebengine_locales")
file(COPY_FILE "${_webengine_helper}" "${_install_prefix}/libexec/QtWebEngineProcess")
file(CHMOD "${_install_prefix}/libexec/QtWebEngineProcess" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(WRITE "${_install_prefix}/libexec/qt.conf"
  "[Paths]\nPrefix=..\nPlugins=lib/qt6/plugins\nLibraries=lib\nLibraryExecutables=libexec\nData=share/qt6\nTranslations=share/qt6/translations\n")
foreach(_webengine_name IN ITEMS qtwebengine_resources.pak qtwebengine_resources_100p.pak qtwebengine_resources_200p.pak v8_context_snapshot.bin)
  file(COPY_FILE "${_webengine_resource_dir}/${_webengine_name}" "${_install_prefix}/share/qt6/resources/${_webengine_name}")
endforeach()
if(_webengine_has_icudtl)
  file(COPY_FILE "${_webengine_resource_dir}/icudtl.dat" "${_install_prefix}/share/qt6/resources/icudtl.dat")
endif()
file(COPY ${_webengine_locale_files} DESTINATION "${_install_prefix}/share/qt6/translations/qtwebengine_locales")
_remember_runtime_origin("${_webengine_helper}" "${_install_prefix}/libexec/QtWebEngineProcess" "webengine-helper")

set(_runtime_dir "${_install_prefix}/lib/melearner")
file(MAKE_DIRECTORY "${_runtime_dir}")
# Arch's mpv package currently records MuJS as an absolute NEEDED path.  CMake's
# resolver rejects that path before it can classify it, so exclude only this
# known host-provider spelling, then copy it into the private closure and
# normalize the staged mpv reference below.  Any other absolute NEEDED path is
# a provider defect and fails staging instead of silently escaping the bundle.
set(_absolute_mujs_needed_regex "^/.*/libmujs\\.so(\\.[^/]*)?$")
set(_runtime_pre_exclude_regexes "${_absolute_mujs_needed_regex}")

function(_require_patched_mpv_runtime _build_dir _runtime_dir _resolved)
  file(STRINGS "${_build_dir}/CMakeCache.txt" _mpv_paths
    REGEX "^MELEARNER_MPV_LIBRARY:INTERNAL=")
  list(LENGTH _mpv_paths _mpv_path_count)
  if(NOT _mpv_path_count EQUAL 1)
    message(FATAL_ERROR "build cache must contain exactly one patched libmpv path")
  endif()
  list(GET _mpv_paths 0 _mpv_path)
  string(REGEX REPLACE "^MELEARNER_MPV_LIBRARY:INTERNAL=" "" _mpv_path "${_mpv_path}")
  if(NOT IS_ABSOLUTE "${_mpv_path}")
    message(FATAL_ERROR "patched libmpv path must be absolute")
  endif()
  if(NOT EXISTS "${_mpv_path}" OR IS_DIRECTORY "${_mpv_path}")
    message(FATAL_ERROR "missing built patched libmpv: ${_mpv_path}")
  endif()
  get_filename_component(_mpv_filename "${_mpv_path}" NAME)
  set(_installed_mpv "${_runtime_dir}/${_mpv_filename}")
  if(NOT EXISTS "${_installed_mpv}" OR IS_DIRECTORY "${_installed_mpv}")
    message(FATAL_ERROR "missing installed patched libmpv: ${_installed_mpv}")
  endif()
  file(SHA256 "${_mpv_path}" _built_hash)
  file(SHA256 "${_installed_mpv}" _installed_hash)
  if(NOT _built_hash STREQUAL _installed_hash)
    message(FATAL_ERROR "installed libmpv differs from the patched build")
  endif()
  file(REAL_PATH "${_installed_mpv}" _installed_real)
  set(_found_mpv FALSE)
  foreach(_dependency IN LISTS _resolved)
    get_filename_component(_name "${_dependency}" NAME)
    if(_name MATCHES "^libmpv\\.so")
      file(REAL_PATH "${_dependency}" _resolved_real)
      if(NOT _resolved_real STREQUAL _installed_real)
        message(FATAL_ERROR "libmpv resolved outside the private runtime: ${_dependency}")
      endif()
      set(_found_mpv TRUE)
    endif()
  endforeach()
  if(NOT _found_mpv)
    message(FATAL_ERROR "libmpv is missing from the resolved runtime")
  endif()
endfunction()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${_binary}" "${_install_prefix}/libexec/QtWebEngineProcess"
  MODULES ${_plugin_sources}
  DIRECTORIES "${_qt_plugin_dir}" "${_runtime_dir}" "${_install_prefix}/lib"
  PRE_EXCLUDE_REGEXES ${_runtime_pre_exclude_regexes}
  RESOLVED_DEPENDENCIES_VAR _resolved_dependencies
  UNRESOLVED_DEPENDENCIES_VAR _unresolved_dependencies)
_require_patched_mpv_runtime("${MELEARNER_BUILD_DIR}" "${_runtime_dir}" "${_resolved_dependencies}")

set(_real_unresolved)
foreach(_dependency IN LISTS _unresolved_dependencies)
  if(NOT _dependency MATCHES "^linux-vdso")
    list(APPEND _real_unresolved "${_dependency}")
  endif()
endforeach()
if(_real_unresolved)
  string(JOIN "\n  " _unresolved_report ${_real_unresolved})
  message(FATAL_ERROR "unresolved runtime dependencies:\n  ${_unresolved_report}")
endif()

set(_absolute_private_dependencies)
set(_absolute_dependency_owners)
foreach(_dependency IN LISTS _resolved_dependencies)
  execute_process(
    COMMAND "${_readelf_tool}" -dW "${_dependency}"
    RESULT_VARIABLE _dependency_readelf_result
    OUTPUT_VARIABLE _dependency_readelf_output
    ERROR_VARIABLE _dependency_readelf_error)
  if(NOT _dependency_readelf_result EQUAL 0)
    message(FATAL_ERROR
      "readelf failed while checking NEEDED paths for ${_dependency}: ${_dependency_readelf_error}")
  endif()
  string(REGEX MATCHALL "Shared library: \\[/[^]]+\\]" _absolute_needed
    "${_dependency_readelf_output}")
  foreach(_absolute_entry IN LISTS _absolute_needed)
    string(REGEX REPLACE ".*\\[([^]]+)\\].*" "\\1" _absolute_path
      "${_absolute_entry}")
    if(NOT _absolute_path MATCHES "${_absolute_mujs_needed_regex}")
      message(FATAL_ERROR
        "unsupported absolute NEEDED path in ${_dependency}: ${_absolute_path}")
    endif()
    list(APPEND _absolute_private_dependencies "${_absolute_path}")
    list(APPEND _absolute_dependency_owners "${_dependency}|${_absolute_path}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _absolute_private_dependencies)
list(REMOVE_DUPLICATES _absolute_dependency_owners)

set(_system_boundary_names)
set(_private_dependency_names)
set(_private_dependencies)
function(_runtime_dependency_is_system_boundary _dependency _dependency_name _output)
  if(_dependency MATCHES
      "^/opt/(cuda|rocm|nvidia|amd)(/|$)"
      OR _dependency MATCHES
      "^/usr/(lib|lib64)/(cuda|rocm|nvidia|amd)(/|$)")
    if(NOT _dependency_name MATCHES
        "^(libOpenCL|libOpenGL|libGL|libEGL|libGLES|libvulkan|libcuda|libcudart|libnv|libnvidia|libamd|libamdocl)[^/]*\\.so")
      message(FATAL_ERROR
        "unlocked vendor GPU dependency cannot be copied privately: ${_dependency}")
    endif()
  endif()
  if(_dependency_name MATCHES
      "^(ld-linux[^/]*|linux-vdso[^/]*|libc|libm|libmvec|libdl|libpthread|librt|libresolv|libutil|libnss_[^/]+|libcrypt|libanl|libBrokenLocale)\\.so"
      OR _dependency_name MATCHES
      "^(libGL[^/]*|libEGL[^/]*|libGLES[^/]*|libOpenGL[^/]*|libOpenCL[^/]*|libvulkan[^/]*|libcuda[^/]*|libcudart[^/]*|libnv[^/]*|libnvidia[^/]*|libamd[^/]*|libamdocl[^/]*|libdrm[^/]*|libgbm[^/]*|libX[^/]*|libxcb[^/]*|libwayland[^/]*|libdecor[^/]*|libxkbcommon[^/]*|libasound[^/]*|libpulse[^/]*|libpipewire[^/]*|libjack[^/]*)\\.so")
    set(${_output} TRUE PARENT_SCOPE)
  else()
    set(${_output} FALSE PARENT_SCOPE)
  endif()
endfunction()

foreach(_dependency IN LISTS _resolved_dependencies)
  get_filename_component(_dependency_name "${_dependency}" NAME)
  _runtime_dependency_is_system_boundary(
    "${_dependency}" "${_dependency_name}" _dependency_is_system_boundary)
  if(_dependency_is_system_boundary)
    list(APPEND _system_boundary_names "${_dependency_name}")
    continue()
  endif()
  if(NOT EXISTS "${_dependency}")
    message(FATAL_ERROR "runtime dependency disappeared: ${_dependency}")
  endif()
  file(COPY "${_dependency}" DESTINATION "${_runtime_dir}" FOLLOW_SYMLINK_CHAIN)
  if(_dependency_name MATCHES "^libmpv\\.so")
    set(_origin_kind "mpv-build")
  else()
    set(_origin_kind "library-provider")
  endif()
  _remember_runtime_origin("${_dependency}" "${_runtime_dir}/${_dependency_name}" "${_origin_kind}")
  list(APPEND _private_dependencies "${_dependency}")
  list(APPEND _private_dependency_names "${_dependency_name}")
endforeach()
foreach(_absolute_path IN LISTS _absolute_private_dependencies)
  if(NOT EXISTS "${_absolute_path}" OR IS_DIRECTORY "${_absolute_path}")
    message(FATAL_ERROR "absolute NEEDED dependency disappeared: ${_absolute_path}")
  endif()
  get_filename_component(_absolute_name "${_absolute_path}" NAME)
  file(COPY "${_absolute_path}" DESTINATION "${_runtime_dir}" FOLLOW_SYMLINK_CHAIN)
  _remember_runtime_origin("${_absolute_path}" "${_runtime_dir}/${_absolute_name}" "library-provider")
  list(APPEND _private_dependencies "${_absolute_path}")
  list(APPEND _private_dependency_names "${_absolute_name}")
endforeach()
list(REMOVE_DUPLICATES _system_boundary_names)
list(REMOVE_DUPLICATES _private_dependency_names)

foreach(_owner_spec IN LISTS _absolute_dependency_owners)
  string(REPLACE "|" ";" _owner_parts "${_owner_spec}")
  list(GET _owner_parts 0 _owner)
  list(GET _owner_parts 1 _absolute_path)
  get_filename_component(_owner_name "${_owner}" NAME)
  get_filename_component(_absolute_name "${_absolute_path}" NAME)
  set(_staged_owner "${_runtime_dir}/${_owner_name}")
  if(NOT EXISTS "${_staged_owner}")
    message(FATAL_ERROR "staged owner for absolute NEEDED dependency is missing: ${_staged_owner}")
  endif()
  execute_process(
    COMMAND "${_patchelf_tool}" --replace-needed "${_absolute_path}" "${_absolute_name}" "${_staged_owner}"
    RESULT_VARIABLE _replace_needed_result
    OUTPUT_VARIABLE _replace_needed_output
    ERROR_VARIABLE _replace_needed_error)
  if(NOT _replace_needed_result EQUAL 0)
    message(FATAL_ERROR
      "patchelf could not normalize ${_absolute_path} in ${_staged_owner}: ${_replace_needed_output}${_replace_needed_error}")
  endif()
endforeach()

set(_has_libmpv FALSE)
set(_has_qt_pdf FALSE)
set(_has_qt_webengine FALSE)
foreach(_dependency_name IN LISTS _private_dependency_names)
  if(_dependency_name MATCHES "^libmpv\\.so")
    set(_has_libmpv TRUE)
  endif()
  if(_dependency_name MATCHES "^libQt6Pdf\\.so")
    set(_has_qt_pdf TRUE)
  endif()
  if(_dependency_name MATCHES "^libQt6WebEngineCore\\.so")
    set(_has_qt_webengine TRUE)
  endif()
endforeach()
if(NOT _has_libmpv)
  message(FATAL_ERROR "runtime closure does not contain libmpv")
endif()
if(NOT _has_qt_pdf)
  message(FATAL_ERROR "runtime closure does not contain Qt6Pdf")
endif()
if(NOT _has_qt_webengine)
  message(FATAL_ERROR "runtime closure does not contain Qt6WebEngineCore")
endif()
if(_webengine_has_icudtl)
  set(_webengine_icu_provider "bundled-data")
else()
  foreach(_icu_library IN ITEMS libicuuc.so libicui18n.so libicudata.so)
    set(_icu_found FALSE)
    foreach(_dependency_name IN LISTS _private_dependency_names)
      if(_dependency_name MATCHES "^${_icu_library}([.].*)?$")
        set(_icu_found TRUE)
      endif()
    endforeach()
    if(NOT _icu_found)
      message(FATAL_ERROR "Qt WebEngine uses system ICU but its private runtime closure is missing ${_icu_library}")
    endif()
  endforeach()
  set(_webengine_icu_provider "private-libraries")
endif()

function(_run_patchelf _path _rpath)
  # Remove only symbols not needed by the runtime. Keep codecs, resources,
  # dynamic exports and the original build artifacts for debugging.
  execute_process(
    COMMAND "${_strip_tool}" --strip-unneeded "${_path}"
    RESULT_VARIABLE _strip_result
    OUTPUT_VARIABLE _strip_output
    ERROR_VARIABLE _strip_error)
  if(NOT _strip_result EQUAL 0)
    message(FATAL_ERROR "strip failed for ${_path}: ${_strip_output}${_strip_error}")
  endif()
  execute_process(
    COMMAND "${_patchelf_tool}" --set-rpath "${_rpath}" "${_path}"
    RESULT_VARIABLE _patchelf_result
    OUTPUT_VARIABLE _patchelf_output
    ERROR_VARIABLE _patchelf_error)
  if(NOT _patchelf_result EQUAL 0)
    message(FATAL_ERROR "patchelf failed for ${_path}: ${_patchelf_output}${_patchelf_error}")
  endif()
endfunction()

function(_set_system_interpreter _path)
  # Homebrew GCC embeds its own prefix's ld.so. That path does not exist on
  # users' machines, even when all bundled libraries meet the glibc baseline.
  execute_process(
    COMMAND "${_patchelf_tool}" --set-interpreter "/lib64/ld-linux-x86-64.so.2" "${_path}"
    RESULT_VARIABLE _interpreter_result
    ERROR_VARIABLE _interpreter_error)
  if(NOT _interpreter_result EQUAL 0)
    message(FATAL_ERROR "could not set the system ELF interpreter for ${_path}: ${_interpreter_error}")
  endif()
endfunction()

_set_system_interpreter("${_binary}")
_set_system_interpreter("${_install_prefix}/libexec/QtWebEngineProcess")
_run_patchelf("${_binary}" "$ORIGIN/../lib/melearner")
_run_patchelf("${_install_prefix}/libexec/QtWebEngineProcess" "$ORIGIN/../lib/melearner")

file(GLOB _staged_plugin_files LIST_DIRECTORIES false "${_plugin_root}/*/*.so*")
foreach(_plugin IN LISTS _staged_plugin_files)
  if(_plugin MATCHES "\\.so(\\.|$)" AND NOT IS_SYMLINK "${_plugin}")
    # Plugins live in usr/lib/qt6/plugins/<group>; private libraries in usr/lib/melearner.
    _run_patchelf("${_plugin}" "$ORIGIN/../../../melearner")
  endif()
endforeach()

file(GLOB _staged_runtime_files LIST_DIRECTORIES false "${_runtime_dir}/*")
foreach(_runtime_file IN LISTS _staged_runtime_files)
  if(NOT IS_SYMLINK "${_runtime_file}")
    execute_process(
      COMMAND "${_file_tool}" -b "${_runtime_file}"
      RESULT_VARIABLE _runtime_file_result
      OUTPUT_VARIABLE _runtime_file_description)
    if(_runtime_file_result EQUAL 0 AND _runtime_file_description MATCHES "^ELF ")
      _run_patchelf("${_runtime_file}" "$ORIGIN")
    endif()
  endif()
endforeach()

function(_audit_elf _path _label)
  execute_process(
    COMMAND "${_readelf_tool}" -dW "${_path}"
    RESULT_VARIABLE _readelf_result
    OUTPUT_VARIABLE _readelf_output
    ERROR_VARIABLE _readelf_error)
  if(NOT _readelf_result EQUAL 0)
    message(FATAL_ERROR "readelf failed for ${_label}: ${_readelf_error}")
  endif()
  string(TOLOWER "${_readelf_output}" _readelf_lower)
  if(_readelf_lower MATCHES "\\((needed|soname)\\)[^\n]*\\[[^]]*(webkit|javascriptcore|webview2|cef|electron|tauri)")
    message(FATAL_ERROR "superseded browser/runtime import in ${_label}")
  endif()
  if(_readelf_output MATCHES "(RPATH|RUNPATH).*(/home/|/opt/|/usr/local/|/nix/store/)")
    message(FATAL_ERROR "host build path in ${_label} RPATH/RUNPATH")
  endif()
endfunction()

_audit_elf("${_binary}" "melearner")
_audit_elf("${_install_prefix}/libexec/QtWebEngineProcess" "QtWebEngineProcess")
foreach(_plugin IN LISTS _staged_plugin_files)
  if(_plugin MATCHES "\\.so(\\.|$)" AND NOT IS_SYMLINK "${_plugin}")
    _audit_elf("${_plugin}" "Qt plugin ${_plugin}")
  endif()
endforeach()
foreach(_runtime_file IN LISTS _staged_runtime_files)
  if(NOT IS_SYMLINK "${_runtime_file}")
    execute_process(
      COMMAND "${_file_tool}" -b "${_runtime_file}"
      RESULT_VARIABLE _runtime_file_result
      OUTPUT_VARIABLE _runtime_file_description)
    if(_runtime_file_result EQUAL 0 AND _runtime_file_description MATCHES "^ELF ")
      _audit_elf("${_runtime_file}" "private runtime ${_runtime_file}")
    endif()
  endif()
endforeach()

file(GLOB_RECURSE _staged_paths RELATIVE "${MELEARNER_STAGE_DIR}" "${MELEARNER_STAGE_DIR}/*")
foreach(_staged_path IN LISTS _staged_paths)
  if(_staged_path MATCHES "(^|/)(native-app|src-tauri|node_modules|qml|webview|electron|chromium|zig|rust)(/|$)")
    message(FATAL_ERROR "superseded runtime asset staged: ${_staged_path}")
  endif()
  if(_staged_path MATCHES "^usr/share/melearner/.*\\.(js|mjs|cjs|html|htm)$")
    message(FATAL_ERROR "browser asset staged: ${_staged_path}")
  endif()
endforeach()

function(_json_quote _output _value)
  string(REPLACE "\\" "\\\\" _escaped "${_value}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  string(REPLACE "\n" "\\n" _escaped "${_escaped}")
  string(REPLACE "\r" "\\r" _escaped "${_escaped}")
  string(REPLACE "\t" "\\t" _escaped "${_escaped}")
  set(${_output} "\"${_escaped}\"" PARENT_SCOPE)
endfunction()

function(_json_array _output)
  set(_result "[")
  set(_first TRUE)
  foreach(_item IN LISTS ARGN)
    _json_quote(_quoted "${_item}")
    if(NOT _first)
      string(APPEND _result ", ")
    endif()
    string(APPEND _result "${_quoted}")
    set(_first FALSE)
  endforeach()
  string(APPEND _result "]")
  set(${_output} "${_result}" PARENT_SCOPE)
endfunction()

_json_array(_private_json ${_private_dependency_names})
_json_array(_system_json ${_system_boundary_names})
_json_array(_plugin_groups_json ${_plugin_group_names})
set(_webengine_resource_paths
  "usr/share/qt6/resources/qtwebengine_resources.pak"
  "usr/share/qt6/resources/qtwebengine_resources_100p.pak"
  "usr/share/qt6/resources/qtwebengine_resources_200p.pak"
  "usr/share/qt6/resources/v8_context_snapshot.bin")
if(_webengine_has_icudtl)
  list(INSERT _webengine_resource_paths 3 "usr/share/qt6/resources/icudtl.dat")
endif()
_json_array(_webengine_resources_json ${_webengine_resource_paths})
get_property(_inventory_paths GLOBAL PROPERTY _runtime_origin_paths)
list(SORT _inventory_paths)
set(_binary_inventory "{\n  \"schemaVersion\": 1,\n  \"version\": \"${MELEARNER_VERSION}\",\n  \"files\": [")
set(_first_file TRUE)
foreach(_package_path IN LISTS _inventory_paths)
  string(SHA256 _key "${_package_path}")
  get_property(_source_hash GLOBAL PROPERTY "_runtime_origin_${_key}_sha256")
  get_property(_source_name GLOBAL PROPERTY "_runtime_origin_${_key}_name")
  get_property(_source_kind GLOBAL PROPERTY "_runtime_origin_${_key}_kind")
  file(SHA256 "${MELEARNER_STAGE_DIR}/${_package_path}" _staged_hash)
  file(SIZE "${MELEARNER_STAGE_DIR}/${_package_path}" _staged_size)
  _json_quote(_path_literal "${_package_path}")
  _json_quote(_name_literal "${_source_name}")
  if(NOT _first_file)
    string(APPEND _binary_inventory ",")
  endif()
  string(APPEND _binary_inventory
    "\n    {\"path\": ${_path_literal}, \"sourceName\": ${_name_literal}, \"sourceKind\": \"${_source_kind}\", \"sourceSha256\": \"${_source_hash}\", \"sha256\": \"${_staged_hash}\", \"size\": ${_staged_size}}")
  set(_first_file FALSE)
endforeach()
string(APPEND _binary_inventory "\n  ]\n}\n")
file(WRITE "${_doc_dir}/runtime-binaries.json" "${_binary_inventory}")
_require_json_object("${_doc_dir}/runtime-binaries.json" "runtime binary inventory")
file(WRITE "${_doc_dir}/runtime-stage.json"
  "{\n"
  "  \"schemaVersion\": 1,\n"
  "  \"version\": \"${MELEARNER_VERSION}\",\n"
  "  \"architecture\": \"x86_64\",\n"
  "  \"releaseQualified\": false,\n"
  "  \"qualificationReason\": \"diagnostic staging only; exact lock, profile, legal, signing, and installed acceptance checks remain external\",\n"
  "  \"softwareDecodeAcceptanceFlag\": \"--software-decoding\",\n"
  "  \"binaryInventory\": \"runtime-binaries.json\",\n"
  "  \"privateLibraries\": ${_private_json},\n"
  "  \"systemRuntimeBoundary\": ${_system_json},\n"
  "  \"qtPluginGroups\": ${_plugin_groups_json},\n"
  "  \"qtWebEngineResources\": ${_webengine_resources_json},\n"
  "  \"qtWebEngineIcuProvider\": \"${_webengine_icu_provider}\",\n"
  "  \"legalInputs\": [\"LICENSE\", \"THIRD_PARTY_NOTICES\", \"melearner.spdx.json\", \"runtime-lock.json\", \"reference-profiles-v1.json\"]\n"
  "}\n")

message(STATUS "C++ Linux diagnostic stage ready: ${MELEARNER_STAGE_DIR}")
message(STATUS "Release-qualified: false")
