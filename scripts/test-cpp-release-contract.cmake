cmake_minimum_required(VERSION 4.4)

set(checker "${CMAKE_CURRENT_LIST_DIR}/check-cpp-release-contract.cmake")
set(temp_root "${CMAKE_CURRENT_LIST_DIR}/../.tmp")
file(MAKE_DIRECTORY "${temp_root}")
set(temp_dir "")
while(temp_dir STREQUAL "" OR EXISTS "${temp_dir}")
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
  set(temp_dir "${temp_root}/release-contract-${suffix}")
endwhile()
file(MAKE_DIRECTORY "${temp_dir}")

function(run_checker name expected_success)
  set(command "${CMAKE_COMMAND}")
  if(ARGC GREATER 2)
    list(APPEND command
      "-DRUNTIME_LOCK=${ARGV2}"
      "-DREFERENCE_PROFILES=${ARGV3}")
  endif()
  list(APPEND command -P "${checker}")

  execute_process(
    COMMAND ${command}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(expected_success)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "${name} was rejected:\n${output}${error}")
    endif()
  elseif(result EQUAL 0)
    message(FATAL_ERROR "${name} was accepted unexpectedly")
  endif()
endfunction()

set(schema_only_runtime_lock [=[
{
  "schemaVersion": 1,
  "lockId": "runtime-lock-v1"
}
]=])
set(schema_only_reference_profiles [=[
{
  "schemaVersion": 1,
  "profileSetId": "reference-profiles-v1"
}
]=])
set(schema_only_runtime_path "${temp_dir}/schema-only-runtime-lock.json")
set(schema_only_profiles_path "${temp_dir}/schema-only-reference-profiles.json")
file(WRITE "${schema_only_runtime_path}" "${schema_only_runtime_lock}")
file(WRITE "${schema_only_profiles_path}" "${schema_only_reference_profiles}")
run_checker("schema-only inputs" FALSE "${schema_only_runtime_path}" "${schema_only_profiles_path}")

set(valid_runtime_lock [=[
{
  "schemaVersion": 1,
  "lockId": "fixture-runtime-lock",
  "linux": {
    "platform": "linux",
    "architecture": "x86_64"
  }
}
]=])
set(valid_reference_profiles [=[
{
  "schemaVersion": 1,
  "profileSetId": "fixture-reference-profiles",
  "linux-x86_64": {
    "platform": "linux",
    "architecture": "x86_64"
  }
}
]=])
set(valid_runtime_path "${temp_dir}/valid-runtime-lock.json")
set(valid_profiles_path "${temp_dir}/valid-reference-profiles.json")
file(WRITE "${valid_runtime_path}" "${valid_runtime_lock}")
file(WRITE "${valid_profiles_path}" "${valid_reference_profiles}")
run_checker("Linux-only substantive inputs" TRUE "${valid_runtime_path}" "${valid_profiles_path}")

set(empty_runtime_lock [=[
{
  "schemaVersion": 1,
  "lockId": "fixture-runtime-lock",
  "linux": {}
}
]=])
set(empty_runtime_path "${temp_dir}/empty-runtime-lock.json")
file(WRITE "${empty_runtime_path}" "${empty_runtime_lock}")
run_checker("empty runtime payload" FALSE "${empty_runtime_path}" "${valid_profiles_path}")

set(empty_reference_profiles [=[
{
  "schemaVersion": 1,
  "profileSetId": "fixture-reference-profiles",
  "linux-x86_64": {}
}
]=])
set(empty_profiles_path "${temp_dir}/empty-reference-profiles.json")
file(WRITE "${empty_profiles_path}" "${empty_reference_profiles}")
run_checker("empty reference profile payload" FALSE "${valid_runtime_path}" "${empty_profiles_path}")

set(non_linux_reference_profiles [=[
{
  "schemaVersion": 1,
  "profileSetId": "fixture-reference-profiles",
  "windows-x86_64": {
    "platform": "windows",
    "architecture": "x86_64"
  }
}
]=])
set(non_linux_profiles_path "${temp_dir}/non-linux-reference-profiles.json")
file(WRITE "${non_linux_profiles_path}" "${non_linux_reference_profiles}")
run_checker("non-Linux-only reference profile" FALSE "${valid_runtime_path}" "${non_linux_profiles_path}")

set(cross_record_reference_profiles [=[
{
  "schemaVersion": 1,
  "profileSetId": "fixture-reference-profiles",
  "profiles": [
    {
      "platform": "linux",
      "architecture": "arm64"
    },
    {
      "platform": "windows",
      "architecture": "x86_64"
    }
  ]
}
]=])
set(cross_record_profiles_path "${temp_dir}/cross-record-reference-profiles.json")
file(WRITE "${cross_record_profiles_path}" "${cross_record_reference_profiles}")
run_checker("cross-record Linux and architecture values" FALSE "${valid_runtime_path}" "${cross_record_profiles_path}")

set(whitespace_runtime_lock [=[
{
  "schemaVersion": 1,
  "lockId": "   ",
  "linux": {
    "platform": "linux",
    "architecture": "x86_64"
  }
}
]=])
set(whitespace_runtime_path "${temp_dir}/whitespace-runtime-lock.json")
file(WRITE "${whitespace_runtime_path}" "${whitespace_runtime_lock}")
run_checker("whitespace runtime lock identity" FALSE "${whitespace_runtime_path}" "${valid_profiles_path}")

set(whitespace_reference_profiles [=[
{
  "schemaVersion": 1,
  "profileSetId": "   ",
  "linux-x86_64": {
    "platform": "linux",
    "architecture": "x86_64"
  }
}
]=])
set(whitespace_profiles_path "${temp_dir}/whitespace-reference-profiles.json")
file(WRITE "${whitespace_profiles_path}" "${whitespace_reference_profiles}")
run_checker("whitespace reference profile identity" FALSE "${valid_runtime_path}" "${whitespace_profiles_path}")

file(REMOVE_RECURSE "${temp_dir}")
message(STATUS "C++ release contract self-test OK: placeholders rejected and Linux-only substantive inputs accepted")
