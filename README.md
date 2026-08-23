# A rollout algorithm for BPMN-OS

This project provides a one-step lookahead [rollout algorithm](https://www.google.com/url?sa=t&source=web&rct=j&opi=89978449&url=https://web.mit.edu/dimitrib/www/Rollouts_Survey.pdf&ved=2ahUKEwjfjM6TkOOUAxV8X_EDHXsuBT4QFnoECCAQAQ&usg=AOvVaw0i0d_YnpfctznI7HiVMTyE) for BPMN-OS. 

The algorithm assesses each candidate decision by determining the final objective after assuming the decision is made and completing the simulation using a greedy algorithm.

To limit the computational effort, the following parameters can be provided:

- `candidates`: the maximum number of candidate decisions to be assessed at each decision step. If set to a positive value, only the candidate decisions with the best local evaluations are assessed. If set to zero (default), all candidates are assessed.
- `repetitions`: the number of rollouts per candidate decision. This parameter is only relevant for stochastic scenarios and the default value is 1. If set to a value above 1, the average objective value is used to decide on the chosen candidate.
- `cutoff`: limits how many decisions are rolled out before the controller falls back to the greedy algorithm for the remainder of the run. It is given as a fraction of the number of decisions made in the greedy baseline run (e.g. `0.5` rolls out the first half of the decisions and takes the rest greedily). If set to zero (default), all decisions are rolled out.
- `threads`: the number of threads to be used for parallel rollouts (default: 1). If set to zero, all available hardware threads are used.
- `bisection`: if set, a choice is assessed by bisection instead of by enumerating all of its alternatives (default). Bisection is useful for a choice over a bounded numeric value.

## Requirements

A C++23 compiler, GCC 15.2 or Clang 18.1.3 or later, CMake 3.26.4 or later, and git.

The BPMN-OS engine is the only dependency and is fetched automatically unless an installed copy satisfies
the version requirement. Everything the engine itself needs, bpmn++ and Xerces-C++ among it, arrives with
it.

## Build

This project has two preset configurations that are created in folders `build/release` and `build/debug`.
Presets are configured the first time they are needed or when running `make configure`.

| Preset | Folder | Compiled with | Used for |
| --- | --- | --- | --- |
| `release` | `build/release` | `-O3 -DNDEBUG`, assertions off | building, installing |
| `debug` | `build/debug` | unoptimised, assertions live, address/undefined/leak sanitizers | development, tests |

You can build `bpmnos-rollout` by

```sh
make # (release)
```
or
```sh
make dev # (debug)
```
with the preset indicated in parentheses. The executable is written to `build/release/bin/bpmnos-rollout`.

`bpmnos-greedy` provides the baseline each rollout is compared against and is built alongside, at
`build/release/_deps/bpmnos-build/bin/bpmnos-greedy`. An engine resolved from a prefix instead of built
here brings its own, already installed.

## Tests

To (build and) run the test suite, use
```sh
make tests # (debug)
```

Once the tests are built (and run) with `make tests`, you can use `./build/debug/run_tests
"[selected_tag]"` to run selected tests carrying the given Catch2 tag. The wrapper enters the project
folder first, the models and data being named by paths relative to it.

## Installation

To install, run
```sh
make # (release)
sudo make install
```
to copy `bin/bpmnos-rollout` into `/usr/local`, or

```sh
cmake --install build/release --prefix <target>
```
to install it into the `<target>` folder.

When the engine had to be built here, because no installed copy satisfied the requirement, it is installed
alongside `bpmnos-rollout`: the two libraries, the headers they include, and `bpmnos-greedy`. An engine
resolved from a prefix is already installed and is not installed again.
