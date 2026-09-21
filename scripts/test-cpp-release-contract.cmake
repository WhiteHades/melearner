cmake_minimum_required(VERSION 4.4)

set(checker "${CMAKE_CURRENT_LIST_DIR}/check-cpp-release-contract.cmake")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -P "${checker}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "C++ release contract rejected canonical inputs:\n${output}${error}")
endif()

message(STATUS "C++ release contract self-test OK")
