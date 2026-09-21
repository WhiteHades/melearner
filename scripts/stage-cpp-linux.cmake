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
  set(MELEARNER_VERSION "0.1.0")
endif()
if(NOT MELEARNER_VERSION STREQUAL "0.1.0")
  message(FATAL_ERROR "melearner release version is fixed at 0.1.0, got ${MELEARNER_VERSION}")
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
if(NOT _project_match OR NOT CMAKE_MATCH_1 STREQUAL "0.1.0")
  message(FATAL_ERROR "CMake project version must be 0.1.0")
endif()
file(STRINGS "${MELEARNER_BUILD_DIR}/CMakeCache.txt" _cache_version_lines
  REGEX "^CMAKE_PROJECT_VERSION:STATIC=")
if(NOT _cache_version_lines)
  message(FATAL_ERROR "configured CMake build version must be 0.1.0")
endif()
list(GET _cache_version_lines 0 _cache_version_line)
string(REGEX REPLACE "^CMAKE_PROJECT_VERSION:STATIC=" "" _cache_version
  "${_cache_version_line}")
if(NOT _cache_version STREQUAL "0.1.0")
  message(FATAL_ERROR "configured CMake build version must be 0.1.0")
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
set(_icon "${_install_prefix}/share/pixmaps/io.github.whitehades.melearner.png")
_require_file("${_binary}" "installed melearner executable")
_require_file("${_desktop}" "desktop launcher")
_require_file("${_icon}" "application icon")

find_program(_file_tool NAMES file)
find_program(_readelf_tool NAMES readelf)
find_program(_patchelf_tool NAMES patchelf)
if(NOT _file_tool OR NOT _readelf_tool OR NOT _patchelf_tool)
  message(FATAL_ERROR "file, readelf, and patchelf are required for C++ Linux staging")
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
list(FIND _desktop_lines "Exec=melearner" _desktop_exec_index)
if(_desktop_exec_index EQUAL -1)
  message(FATAL_ERROR "desktop launcher must execute melearner")
endif()
list(FIND _desktop_lines "Icon=io.github.whitehades.melearner" _desktop_icon_index)
if(_desktop_icon_index EQUAL -1)
  message(FATAL_ERROR "desktop launcher has an unexpected icon")
endif()
string(TOLOWER "${_desktop_contents}" _desktop_lower)
if(_desktop_lower MATCHES "(tauri|native-app|node|zig|rust|webview|webengine|qml|electron)")
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

set(_plugin_root "${_install_prefix}/lib/qt6/plugins")
set(_plugin_groups
  platforms
  platformthemes
  platforminputcontexts
  styles
  imageformats
  iconengines
  generic
  tls
  networkinformation
  xcbglintegrations
  wayland-shell-integration
  wayland-decoration-client
  wayland-graphics-integration-client
  egldeviceintegrations)
set(_plugin_sources)
set(_plugin_group_names)
set(_platform_plugin_count 0)
foreach(_group IN LISTS _plugin_groups)
  set(_source_group "${_qt_plugin_dir}/${_group}")
  if(NOT IS_DIRECTORY "${_source_group}")
    continue()
  endif()
  file(GLOB _source_plugins LIST_DIRECTORIES false "${_source_group}/*.so*")
  foreach(_source_plugin IN LISTS _source_plugins)
    if(NOT _source_plugin MATCHES "\\.so(\\.|$)")
      continue()
    endif()
    set(_destination_group "${_plugin_root}/${_group}")
    file(MAKE_DIRECTORY "${_destination_group}")
    file(COPY "${_source_plugin}" DESTINATION "${_destination_group}" FOLLOW_SYMLINK_CHAIN)
    list(APPEND _plugin_sources "${_source_plugin}")
    list(APPEND _plugin_group_names "${_group}")
    if(_group STREQUAL "platforms")
      math(EXPR _platform_plugin_count "${_platform_plugin_count} + 1")
    endif()
  endforeach()
endforeach()
if(_platform_plugin_count LESS 1)
  message(FATAL_ERROR "Qt has no platform plugin under ${_qt_plugin_dir}/platforms")
endif()

file(WRITE "${_install_prefix}/bin/qt.conf"
  "[Paths]\nPrefix=..\nPlugins=lib/qt6/plugins\n")

set(_runtime_dir "${_install_prefix}/lib/melearner")
file(MAKE_DIRECTORY "${_runtime_dir}")
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${_binary}"
  MODULES ${_plugin_sources}
  DIRECTORIES "${_qt_plugin_dir}" "${_runtime_dir}" "${_install_prefix}/lib"
  RESOLVED_DEPENDENCIES_VAR _resolved_dependencies
  UNRESOLVED_DEPENDENCIES_VAR _unresolved_dependencies)

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

set(_system_boundary_names)
set(_private_dependency_names)
set(_private_dependencies)
foreach(_dependency IN LISTS _resolved_dependencies)
  get_filename_component(_dependency_name "${_dependency}" NAME)
  if(_dependency_name MATCHES
      "^(ld-linux[^/]*|linux-vdso[^/]*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libnss_[^/]+|libcrypt|libanl|libBrokenLocale)\\.so"
      OR _dependency_name MATCHES
      "^(libGL[^/]*|libEGL[^/]*|libGLES[^/]*|libOpenGL[^/]*|libdrm[^/]*|libgbm[^/]*|libX[^/]*|libxcb[^/]*|libwayland[^/]*|libdecor[^/]*|libxkbcommon[^/]*|libasound[^/]*|libpulse[^/]*|libpipewire[^/]*|libjack[^/]*)\\.so")
    list(APPEND _system_boundary_names "${_dependency_name}")
    continue()
  endif()
  if(NOT EXISTS "${_dependency}")
    message(FATAL_ERROR "runtime dependency disappeared: ${_dependency}")
  endif()
  file(COPY "${_dependency}" DESTINATION "${_runtime_dir}" FOLLOW_SYMLINK_CHAIN)
  list(APPEND _private_dependencies "${_dependency}")
  list(APPEND _private_dependency_names "${_dependency_name}")
endforeach()
list(REMOVE_DUPLICATES _system_boundary_names)
list(REMOVE_DUPLICATES _private_dependency_names)

set(_has_libmpv FALSE)
set(_has_qt_pdf FALSE)
foreach(_dependency_name IN LISTS _private_dependency_names)
  if(_dependency_name MATCHES "^libmpv\\.so")
    set(_has_libmpv TRUE)
  endif()
  if(_dependency_name MATCHES "^libQt6Pdf\\.so")
    set(_has_qt_pdf TRUE)
  endif()
endforeach()
if(NOT _has_libmpv)
  message(FATAL_ERROR "runtime closure does not contain libmpv")
endif()
if(NOT _has_qt_pdf)
  message(FATAL_ERROR "runtime closure does not contain Qt6Pdf")
endif()

function(_run_patchelf _path _rpath)
  execute_process(
    COMMAND "${_patchelf_tool}" --set-rpath "${_rpath}" "${_path}"
    RESULT_VARIABLE _patchelf_result
    OUTPUT_VARIABLE _patchelf_output
    ERROR_VARIABLE _patchelf_error)
  if(NOT _patchelf_result EQUAL 0)
    message(FATAL_ERROR "patchelf failed for ${_path}: ${_patchelf_output}${_patchelf_error}")
  endif()
endfunction()

_run_patchelf("${_binary}" "$ORIGIN/../lib/melearner")

file(GLOB _staged_plugin_files LIST_DIRECTORIES false "${_plugin_root}/*/*.so*")
foreach(_plugin IN LISTS _staged_plugin_files)
  if(_plugin MATCHES "\\.so(\\.|$)" AND NOT IS_SYMLINK "${_plugin}")
    _run_patchelf("${_plugin}" "$ORIGIN/../../melearner")
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
  if(_readelf_lower MATCHES "(webkit|javascriptcore|webview2|cef|electron|tauri|qwebengine|qt6qml|qt6quick)")
    message(FATAL_ERROR "superseded browser/runtime import in ${_label}")
  endif()
  if(_readelf_output MATCHES "(RPATH|RUNPATH).*(/home/|/opt/|/usr/local/|/nix/store/)")
    message(FATAL_ERROR "host build path in ${_label} RPATH/RUNPATH")
  endif()
endfunction()

_audit_elf("${_binary}" "melearner")
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
  if(_staged_path MATCHES "(^|/)(native-app|src-tauri|node_modules|qml|webengine|webview|electron|chromium|zig|rust)(/|$)")
    message(FATAL_ERROR "superseded runtime asset staged: ${_staged_path}")
  endif()
  if(_staged_path MATCHES "^usr/share/melearner/.*\\.(js|mjs|cjs|html|htm)$")
    message(FATAL_ERROR "browser asset staged: ${_staged_path}")
  endif()
endforeach()

function(_json_array _output)
  set(_result "[")
  set(_first TRUE)
  foreach(_item IN LISTS ARGN)
    string(REPLACE "\\" "\\\\" _escaped "${_item}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    if(NOT _first)
      string(APPEND _result ", ")
    endif()
    string(APPEND _result "\"${_escaped}\"")
    set(_first FALSE)
  endforeach()
  string(APPEND _result "]")
  set(${_output} "${_result}" PARENT_SCOPE)
endfunction()

_json_array(_private_json ${_private_dependency_names})
_json_array(_system_json ${_system_boundary_names})
_json_array(_plugin_groups_json ${_plugin_group_names})
file(WRITE "${_doc_dir}/runtime-stage.json"
  "{\n"
  "  \"schemaVersion\": 1,\n"
  "  \"version\": \"${MELEARNER_VERSION}\",\n"
  "  \"architecture\": \"x86_64\",\n"
  "  \"releaseQualified\": false,\n"
  "  \"qualificationReason\": \"diagnostic staging only; exact lock, profile, legal, signing, and installed acceptance checks remain external\",\n"
  "  \"softwareDecodeAcceptanceFlag\": \"--software-decoding\",\n"
  "  \"privateLibraries\": ${_private_json},\n"
  "  \"systemRuntimeBoundary\": ${_system_json},\n"
  "  \"qtPluginGroups\": ${_plugin_groups_json},\n"
  "  \"legalInputs\": [\"LICENSE\", \"THIRD_PARTY_NOTICES\", \"melearner.spdx.json\", \"runtime-lock.json\", \"reference-profiles-v1.json\"]\n"
  "}\n")

message(STATUS "C++ Linux diagnostic stage ready: ${MELEARNER_STAGE_DIR}")
message(STATUS "Release-qualified: false")
