cmake_minimum_required(VERSION 4.4)

option(
  CPP_PARITY_FULL_MATERIALIZATION
  "Create 100000 Lesson placeholders, then verify the 99802-Lesson final scan tree"
  OFF
)
option(
  CPP_PARITY_SANITIZERS
  "Compile and run the fixture tests with AddressSanitizer and UndefinedBehaviorSanitizer"
  OFF
)

set(repo_root "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_path(NORMAL_PATH repo_root)
set(source_root "${repo_root}/cpp-app/tests/fixtures")
set(work_owner "${repo_root}/.tmp/cpp-parity-v1")
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef work_id)
set(work_root "${work_owner}/${work_id}")
while(EXISTS "${work_root}")
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef work_id)
  set(work_root "${work_owner}/${work_id}")
endwhile()
set(run_root "${work_root}/runs")

function(cleanup_work_root)
  file(REMOVE_RECURSE "${work_root}")
endfunction()

function(fail_with_cleanup message_text)
  cleanup_work_root()
  message(FATAL_ERROR "${message_text}")
endfunction()

if(DEFINED ENV{CXX} AND NOT "$ENV{CXX}" STREQUAL "")
  set(cxx "$ENV{CXX}")
elseif(CMAKE_HOST_WIN32)
  find_program(cxx NAMES cl.exe cl REQUIRED)
else()
  find_program(cxx NAMES c++ g++ clang++ REQUIRED)
endif()

cmake_path(GET cxx FILENAME cxx_filename)
string(TOLOWER "${cxx_filename}" cxx_filename_lower)
if(cxx_filename_lower MATCHES "^cl(\\.exe)?$")
  set(native_msvc TRUE)
  set(executable_suffix ".exe")
  set(common_flags
    /nologo
    /std:c++23preview
    /O2
    /W4
    /WX
    /EHsc
    /permissive-
    /utf-8
    /FS
    "/Fo${work_root}/"
  )
  if(CPP_PARITY_SANITIZERS)
    fail_with_cleanup("CPP_PARITY_SANITIZERS requires GNU or Clang, not native MSVC cl")
  endif()
else()
  set(native_msvc FALSE)
  set(executable_suffix "")
  if(CMAKE_HOST_APPLE)
    set(cxx_standard_flag -std=c++2b)
  else()
    set(cxx_standard_flag -std=c++23)
  endif()
  set(common_flags
    ${cxx_standard_flag}
    -Wall
    -Wextra
    -Wpedantic
    -Werror
  )
  if(CPP_PARITY_SANITIZERS)
    list(APPEND common_flags
      -O1
      -g
      -fno-omit-frame-pointer
      -fsanitize=address,undefined
    )
  else()
    list(APPEND common_flags -O2)
  endif()
endif()

file(MAKE_DIRECTORY "${work_root}")

set(common_sources
  "${source_root}/parity_fixture.cpp"
  "${source_root}/parity_recipe_v1.cpp"
)

function(compile_fixture_executable output main_source)
  if(native_msvc)
    set(output_flag "/Fe${output}")
    set(link_flags
      /link
      /MANIFEST:EMBED
      "/MANIFESTINPUT:${source_root}/parity_fixture.manifest"
    )
  else()
    set(output_flag -o "${output}")
    set(link_flags)
  endif()
  execute_process(
    COMMAND "${cxx}"
      ${common_flags}
      ${common_sources}
      "${main_source}"
      ${output_flag}
      ${link_flags}
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error
  )
  if(NOT compile_result EQUAL 0)
    fail_with_cleanup(
      "C++ parity fixture compilation failed for ${main_source}:\n${compile_output}${compile_error}"
    )
  endif()
endfunction()

set(generator_executable "${work_root}/parity_fixture${executable_suffix}")
set(test_executable "${work_root}/parity_fixture_test${executable_suffix}")
compile_fixture_executable(
  "${generator_executable}"
  "${source_root}/parity_fixture_main.cpp"
)
compile_fixture_executable(
  "${test_executable}"
  "${source_root}/parity_fixture_test.cpp"
)

execute_process(
  COMMAND "${generator_executable}" --help
  RESULT_VARIABLE generator_result
  OUTPUT_QUIET
  ERROR_VARIABLE generator_error
)
if(NOT generator_result EQUAL 0)
  fail_with_cleanup("C++ parity fixture generator smoke failed:\n${generator_error}")
endif()

set(test_command
  "${test_executable}"
  "${repo_root}"
  "${run_root}"
)
if(CPP_PARITY_FULL_MATERIALIZATION)
  list(APPEND test_command --full-materialization)
endif()
execute_process(
  COMMAND ${test_command}
  RESULT_VARIABLE test_result
  OUTPUT_VARIABLE test_output
  ERROR_VARIABLE test_error
)
if(NOT test_result EQUAL 0)
  fail_with_cleanup("C++ parity fixture self-test failed:\n${test_output}${test_error}")
endif()

set(manifest_path "${repo_root}/fixtures/parity/fixture-manifest-v2.json")
if(NOT EXISTS "${manifest_path}")
  fail_with_cleanup("fixture-manifest-v2.json is missing")
endif()
file(READ "${manifest_path}" manifest_json)
string(JSON manifest_version ERROR_VARIABLE manifest_error GET "${manifest_json}" version)
if(manifest_error OR NOT manifest_version EQUAL 2)
  fail_with_cleanup("fixture-manifest-v2.json is not valid manifest v2 JSON: ${manifest_error}")
endif()

cleanup_work_root()
string(STRIP "${test_output}" test_output)
message(STATUS "${test_output}")
message(STATUS "C++ parity fixture self-test OK")
