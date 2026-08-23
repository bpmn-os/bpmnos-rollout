# Rollout test suite (Catch2). A `run_tests` executable that is EXCLUDE_FROM_ALL and a `tests` custom target
# that runs it from the project root, mirroring the engine.
#
# Catch2 is fetched here rather than taken from the engine, whose tests a consumer does not build.

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
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/DecisionCounter.cpp
)

rollout_compile_options(run_tests)

target_include_directories(run_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(run_tests
  PRIVATE
  bpmnos::execution
  bpmnos::model
  pthread
  Catch2::Catch2WithMain
)

set_target_properties(run_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Run from the rollout project root so the model and data paths in each test.h resolve relative to it.
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
