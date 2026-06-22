# Rollout test suite (Catch2). Mirrors the engine's tests/ setup: a `run_tests` executable that is
# EXCLUDE_FROM_ALL and a `tests` custom target that runs it from the project root.
#
# The fetched engine's own tests are disabled (BPMNOS_TESTS OFF in the top-level CMakeLists), so Catch2 is
# fetched here. The rollout's own sources (Results/Rollout/ThreadPool) are compiled into the test binary; the
# engine is consumed through its generated single headers, exactly like the application.

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(
  catch
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(catch)

add_executable(run_tests EXCLUDE_FROM_ALL
  main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/Results.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/Rollout.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/ThreadPool.cpp
)

target_compile_options(run_tests PRIVATE -O0 -g -ggdb3)

target_link_libraries(run_tests
  PRIVATE
  bpmnos-execution
  bpmnos-model
  bpmn++
  pthread
  xerces-c
  Catch2::Catch2WithMain
)

# The engine generates its single headers (bpmnos-model.h / bpmnos-execution.h) via custom ALL targets;
# make sure they run before our sources compile.
add_dependencies(run_tests model_header execution_header)

set_target_properties(run_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Run from the rollout project root so the model/data paths in each test.h resolve relative to it.
file(WRITE "${CMAKE_BINARY_DIR}/run_tests" "#!/bin/bash\n(cd ${PROJECT_SOURCE_DIR};${CMAKE_BINARY_DIR}/tests/run_tests $@)")
execute_process(COMMAND chmod +x "${CMAKE_BINARY_DIR}/run_tests")
message("-- Run rollout tests: ./run_tests [tags]")

add_custom_target(
  tests
  COMMAND ${CMAKE_BINARY_DIR}/run_tests || exit 0
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/
  VERBATIM
)
add_dependencies(tests run_tests)
