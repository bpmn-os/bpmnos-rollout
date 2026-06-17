cmake_minimum_required(VERSION 3.26.4)

project(bpmnos-rollout)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)

# Set the output directory for the binary
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

# Use ccache (when installed) to cache compilation across rebuilds. Set before
# the FetchContent calls so it also covers the fetched engine.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  message("-- Using ccache: ${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
else()
  message("-- ccache not found; compilation will not be cached")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# BPMN-OS engine, consumed through its generated single headers
# (bpmnos-model.h / bpmnos-execution.h in _deps/bpmnos-src/lib). The engine's
# own tests are not needed here.
set(BPMNOS_TESTS OFF CACHE BOOL "Disable bpmnos tests")

FetchContent_Declare(
        bpmnos
        GIT_REPOSITORY https://github.com/bpmn-os/bpmnos-engine.git
        GIT_TAG main
)

FetchContent_MakeAvailable(bpmnos)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  SYSTEM
  ${CMAKE_BINARY_DIR}/_deps/bpmnos-src/lib
  ${CMAKE_BINARY_DIR}/_deps/cnl-src/include/
  ${CMAKE_BINARY_DIR}/_deps/json-src/
  ${CMAKE_BINARY_DIR}/_deps/strutil-src/
  ${CMAKE_BINARY_DIR}/_deps/limex-src/
)

# Standalone application
add_executable(bpmnos-rollout
  src/main.cpp
  src/Results.cpp
  src/Rollout.cpp
  src/ThreadPool.cpp
)

target_compile_options(bpmnos-rollout PRIVATE -O2)

# The engine generates its single headers (bpmnos-model.h / bpmnos-execution.h)
# via custom ALL targets; make sure they run before our sources compile.
add_dependencies(bpmnos-rollout model_header execution_header)

target_link_libraries(bpmnos-rollout
  PRIVATE
  bpmnos-execution
  bpmnos-model
  bpmn++
  pthread
  xerces-c
)

if (NDEBUG)
  target_compile_options(${PROJECT_NAME}
    PRIVATE
    -O3
    -Werror
    -pedantic-errors
    -Wpedantic
    -Wall
    -Wshadow=local
    -Wextra
    -Wconversion
    -Wsign-conversion
    -fmax-errors=1 # Remove
    -Wno-error=unused-parameter # Remove
    -Wno-error=unused-variable # Remove
    -Wno-misleading-indentation
)
else()
  target_compile_options(${PROJECT_NAME}
    PRIVATE
    -O0
    -Werror
    -pedantic-errors
    -Wpedantic
    -Wall
    -Wshadow=local
    -Wextra
    -Wconversion
    -Wsign-conversion
    -fsanitize=address,undefined,leak
    -fno-omit-frame-pointer
    -g
    -ggdb3
    -pg # Profiling
    -fmax-errors=1 # Remove
    -Wno-error=unused-parameter # Remove
    -Wno-error=unused-variable # Remove
    -Wno-misleading-indentation
  )

  add_link_options(-fsanitize=address,undefined,leak)

  target_link_options(${PROJECT_NAME}
    PRIVATE
    -fsanitize=address,undefined,leak
    -pg # Profiling
  )
endif()

