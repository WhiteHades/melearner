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

set(release_evidence_schema_path "${CMAKE_CURRENT_LIST_DIR}/../packaging/release-evidence-schema-v1.json")
file(READ "${release_evidence_schema_path}" release_evidence_schema_json)
string(JSON artifact_type ERROR_VARIABLE artifact_type_error GET
  "${release_evidence_schema_json}" properties artifacts type)
if(artifact_type_error OR NOT artifact_type STREQUAL "array")
  message(FATAL_ERROR "release evidence artifacts must be an array")
endif()

string(JSON artifact_min_items ERROR_VARIABLE artifact_min_error GET
  "${release_evidence_schema_json}" properties artifacts minItems)
string(JSON artifact_max_items ERROR_VARIABLE artifact_max_error GET
  "${release_evidence_schema_json}" properties artifacts maxItems)
string(JSON artifact_unique_items ERROR_VARIABLE artifact_unique_error GET
  "${release_evidence_schema_json}" properties artifacts uniqueItems)
if(artifact_min_error OR artifact_max_error OR artifact_unique_error
    OR NOT artifact_min_items EQUAL 2 OR NOT artifact_max_items EQUAL 2
    OR NOT artifact_unique_items)
  message(FATAL_ERROR "release evidence artifacts must require exactly two unique entries")
endif()

string(JSON artifact_item_ref ERROR_VARIABLE artifact_item_error GET
  "${release_evidence_schema_json}" properties artifacts items "$ref")
if(artifact_item_error OR NOT artifact_item_ref STREQUAL "#/$defs/artifactManifest")
  message(FATAL_ERROR "release evidence artifacts must use artifactManifest items")
endif()

set(expected_artifact_ids
  appimage-linux-x86_64
  arch-linux-x86_64)
list(LENGTH expected_artifact_ids expected_artifact_count)
string(JSON artifact_id_count ERROR_VARIABLE artifact_id_count_error LENGTH
  "${release_evidence_schema_json}" "$defs" artifactManifest properties id enum)
if(artifact_id_count_error OR NOT artifact_id_count EQUAL expected_artifact_count)
  message(FATAL_ERROR "artifactManifest must contain exactly the two Linux artifact IDs")
endif()
math(EXPR last_artifact_id "${expected_artifact_count} - 1")
foreach(index RANGE 0 ${last_artifact_id})
  list(GET expected_artifact_ids ${index} expected_id)
  string(JSON artifact_id ERROR_VARIABLE artifact_id_error GET
    "${release_evidence_schema_json}" "$defs" artifactManifest properties id enum ${index})
  if(artifact_id_error OR NOT artifact_id STREQUAL expected_id)
    message(FATAL_ERROR "artifactManifest artifact ID enum changed unexpectedly")
  endif()
endforeach()

string(JSON required_artifact_count ERROR_VARIABLE required_artifact_error LENGTH
  "${release_evidence_schema_json}" properties artifacts allOf)
if(required_artifact_error OR NOT required_artifact_count EQUAL expected_artifact_count)
  message(FATAL_ERROR "Linux release evidence must require exactly two artifact IDs")
endif()
math(EXPR last_required_artifact "${expected_artifact_count} - 1")
foreach(index RANGE 0 ${last_required_artifact})
  list(GET expected_artifact_ids ${index} expected_id)
  string(JSON required_artifact_id ERROR_VARIABLE required_artifact_id_error GET
    "${release_evidence_schema_json}" properties artifacts allOf ${index}
    contains properties id const)
  if(required_artifact_id_error OR NOT required_artifact_id STREQUAL expected_id)
    message(FATAL_ERROR "Linux release evidence artifact requirements changed unexpectedly")
  endif()
endforeach()

file(REMOVE_RECURSE "${temp_dir}")
message(STATUS "C++ release contract self-test OK: placeholders rejected and Linux-only artifact aggregate enforced")
