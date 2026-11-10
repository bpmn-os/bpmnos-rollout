# A rollout algorithm for BPMN-OS

This project provides a one-step lookahead [rollout algorithm](https://www.google.com/url?sa=t&source=web&rct=j&opi=89978449&url=https://web.mit.edu/dimitrib/www/Rollouts_Survey.pdf&ved=2ahUKEwjfjM6TkOOUAxV8X_EDHXsuBT4QFnoECCAQAQ&usg=AOvVaw0i0d_YnpfctznI7HiVMTyE) for BPMN-OS. 

The algorithm assesses each candidate decision by determining the final objective after assuming the decision is made and completing the simulation using a greedy algorithm. Every candidate, the decision the greedy algorithm would take included, is assessed from the current state at every decision, on the same sampled futures, so that in a stochastic scenario the candidates are compared on what has actually been disclosed rather than on what was expected when an earlier decision was assessed.

To limit the computational effort, the following parameters can be provided:

- `candidates`: the maximum number of candidate decisions to be assessed at each decision step. If set to a positive value, only the candidate decisions with the best local evaluations are assessed. If set to zero (default), all candidates are assessed.
- `repetitions`: the number of rollouts per candidate decision. This parameter is only relevant for stochastic scenarios and the default value is 1. If set to a value above 1, the average objective value is used to decide on the chosen candidate.
- `cutoff`: limits how many decisions are rolled out before the controller falls back to the greedy algorithm for the remainder of the run. It is given as a fraction of the number of decisions made in the greedy baseline run (e.g. `0.5` rolls out the first half of the decisions and takes the rest greedily). If set to zero (default), all decisions are rolled out.
- `threads`: the number of threads to be used for parallel rollouts (default: 1). If set to zero, all available hardware threads are used.
- `bisect`: the least number of values a choice must allow to be assessed by bisection rather than by enumerating all of its values, both by the rollout and by the greedy algorithm, in the baseline run and in the completion of every candidate decision. Bisection is useful for a choice over a large range of numeric values. If not set (default), every choice with a step is enumerated, and a choice of a decimal attribute without a step, whose values are too many to be enumerated, is bisected.

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
alongside `bpmnos-rollout`: the two libraries and the headers they include. An engine
resolved from a prefix is already installed and is not installed again.
