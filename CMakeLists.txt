cmake_minimum_required(VERSION 3.26.4)

project(bpmnos-rollout VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only when rollout is built on its own, so that a consumer's choice always wins.
if (PROJECT_IS_TOP_LEVEL)
  if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  endif()
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Sanitizing follows the build type, and is an option of its own rather than a side effect of turning
# optimisation off, which is what the former NDEBUG option made of it.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(ROLLOUT_SANITIZE_DEFAULT ON)
else()
  set(ROLLOUT_SANITIZE_DEFAULT OFF)
endif()
option(ROLLOUT_SANITIZE "Build with the address, undefined and leak sanitizers" ${ROLLOUT_SANITIZE_DEFAULT})

option(ROLLOUT_TESTS "Build rollout tests" ${PROJECT_IS_TOP_LEVEL})

# Build output goes to the build tree rather than into the checkout.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# The engine is the only dependency rollout declares. Its headers, those of the header-only libraries the
# amalgamated headers include, and bpmn++ with Xerces all arrive as usage requirements of the exported
# targets, so none of them is named here.
set(BPMNOS_VERSION 0.1.0)
set(BPMNOS_REF "v${BPMNOS_VERSION}")

# bpmnos-greedy is the baseline every rollout result is compared against, so it is built alongside
# bpmnos-rollout. An engine resolved from a prefix brings its own, already built.
set(BPMNOS_APP ON CACHE BOOL "Build bpmnos-greedy alongside bpmnos-rollout" FORCE)

FetchContent_Declare(bpmnos
  GIT_REPOSITORY https://github.com/bpmn-os/bpmnos-engine.git
  GIT_TAG        ${BPMNOS_REF}
  FIND_PACKAGE_ARGS ${BPMNOS_VERSION}
)
FetchContent_MakeAvailable(bpmnos)

set(ROLLOUT_COMPILE_OPTIONS
  -Werror
  -pedantic-errors
  -Wpedantic
  -Wall
  -Wextra
  -Wconversion
  -Wsign-conversion
  -Wshadow=local
  -Wno-misleading-indentation
  -Wno-error=unused-parameter
  -Wno-error=unused-variable
)

if (ROLLOUT_SANITIZE)
  list(APPEND ROLLOUT_COMPILE_OPTIONS -fsanitize=address,undefined,leak -fno-omit-frame-pointer -ggdb3)
  set(ROLLOUT_LINK_OPTIONS -fsanitize=address,undefined,leak)
endif()

function(rollout_compile_options target)
  target_compile_options(${target} PRIVATE ${ROLLOUT_COMPILE_OPTIONS})
  if (ROLLOUT_LINK_OPTIONS)
    target_link_options(${target} PRIVATE ${ROLLOUT_LINK_OPTIONS})
  endif()
endfunction()

add_executable(bpmnos-rollout
  src/main.cpp
  src/Results.cpp
  src/Rollout.cpp
  src/ThreadPool.cpp
  src/DecisionCounter.cpp
)

rollout_compile_options(bpmnos-rollout)

target_include_directories(bpmnos-rollout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(bpmnos-rollout
  PRIVATE
  bpmnos::execution
  bpmnos::model
  pthread
)

if (ROLLOUT_TESTS)
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS bpmnos-rollout RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
