cmake_minimum_required(VERSION 4.4)

set(checker "${CMAKE_CURRENT_LIST_DIR}/check-cpp-traceability.cmake")
set(plan_path "${CMAKE_CURRENT_LIST_DIR}/../docs/specs/cpp23-implementation-plan.md")
set(spec_path "${CMAKE_CURRENT_LIST_DIR}/../docs/specs/fully-native-melearner.md")
set(temp_dir "${CMAKE_CURRENT_BINARY_DIR}/.tmp-cpp-traceability")

if(NOT EXISTS "${plan_path}" OR NOT EXISTS "${spec_path}")
  message(STATUS "C++ traceability self-test skipped: private planning sources are not present")
  return()
endif()

file(READ "${plan_path}" valid_plan)
file(READ "${spec_path}" valid_spec)
file(REMOVE_RECURSE "${temp_dir}")
file(MAKE_DIRECTORY "${temp_dir}")

function(expect_rejected name plan_var spec_var)
  set(test_plan "${temp_dir}/${name}-plan.md")
  set(test_spec "${temp_dir}/${name}-spec.md")
  file(WRITE "${test_plan}" "${${plan_var}}")
  file(WRITE "${test_spec}" "${${spec_var}}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DTRACEABILITY_PLAN=${test_plan}"
      "-DTRACEABILITY_SPEC=${test_spec}"
      -P "${checker}"
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(result EQUAL 0)
    message(FATAL_ERROR "Traceability checker accepted invalid case: ${name}")
  endif()
endfunction()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -P "${checker}"
  RESULT_VARIABLE valid_result
  OUTPUT_QUIET
  ERROR_VARIABLE valid_error
)
if(NOT valid_result EQUAL 0)
  message(FATAL_ERROR "Traceability checker rejected valid sources: ${valid_error}")
endif()

set(mutated "${valid_plan}")
string(REPLACE "| S40 | T33 | #64 |" "| S39 | T33 | #64 |" mutated "${mutated}")
expect_rejected("duplicate-story" mutated valid_spec)

set(mutated "${valid_plan}")
string(REPLACE "| T04 | #35 |" "| T04 | #999 |" mutated "${mutated}")
expect_rejected("wrong-issue" mutated valid_spec)

set(mutated "${valid_plan}")
string(REPLACE "| T02 | #33 | Generate deterministic parity fixtures | T01 |" "| T02 | #33 | Generate deterministic parity fixtures | T34 |" mutated "${mutated}")
expect_rejected("forward-blocker" mutated valid_spec)

set(mutated "${valid_plan}")
string(REPLACE "| T04 | #35 | Bootstrap the C++23 Qt application shell | T02, T03 |" "| T04 | #35 | Bootstrap the C++23 Qt application shell | T02, T02 |" mutated "${mutated}")
expect_rejected("duplicate-blocker" mutated valid_spec)

set(mutated "${valid_plan}")
string(REPLACE "| T04 | #35 | Bootstrap the C++23 Qt application shell |" "| T04 | #35 |  |" mutated "${mutated}")
expect_rejected("blank-title" mutated valid_spec)

set(mutated "${valid_plan}\n| TXX | #999 | Invalid | None |\n")
expect_rejected("extra-ticket-row" mutated valid_spec)

set(mutated_spec "${valid_spec}")
string(REPLACE "40. **S40.**" "40. **SXX.**" mutated_spec "${mutated_spec}")
expect_rejected("malformed-canonical-story" valid_plan mutated_spec)

file(REMOVE_RECURSE "${temp_dir}")
message(STATUS "C++ traceability self-test OK: valid graph accepted and seven invalid graphs rejected")
