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
#
# The engine is pinned by commit rather than by release tag, because its interfaces are still changing and
# a tag would promise a stability that is not yet being offered. Move this pin deliberately, together with
# whatever change in rollout the new revision requires.
#
# No FIND_PACKAGE_ARGS accompanies the pin. It would let an installed engine satisfying the version
# requirement be used in place of the fetch, and since the declared version does not change with every
# interface change, an installed engine could shadow the pinned revision and silently supply a different
# interface. Rollout therefore always builds against the revision named here.
set(BPMNOS_REF d049e44d0171a9f6f3da3b1ee772c5162d30574f)

# bpmnos-greedy is the baseline every rollout result is compared against, so it is built alongside
# bpmnos-rollout.
set(BPMNOS_APP ON CACHE BOOL "Build bpmnos-greedy alongside bpmnos-rollout" FORCE)

FetchContent_Declare(bpmnos
  GIT_REPOSITORY https://github.com/bpmn-os/bpmnos-engine.git
  GIT_TAG        ${BPMNOS_REF}
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
