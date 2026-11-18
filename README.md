# A rollout algorithm for BPMN-OS

This project provides a one-step lookahead [rollout algorithm](https://www.google.com/url?sa=t&source=web&rct=j&opi=89978449&url=https://web.mit.edu/dimitrib/www/Rollouts_Survey.pdf&ved=2ahUKEwjfjM6TkOOUAxV8X_EDHXsuBT4QFnoECCAQAQ&usg=AOvVaw0i0d_YnpfctznI7HiVMTyE) for BPMN-OS. 

The algorithm assesses each candidate decision by determining the final objective after assuming the decision is made and completing the simulation using a greedy algorithm. With several repetitions, every candidate, the decision the greedy algorithm would take included, is assessed from the current state at every decision, on the same sampled futures, so that the candidates are compared on what has actually been disclosed rather than on what was expected when an earlier decision was assessed. With a single repetition, the decision the greedy algorithm would take is valued by the result of the greedy completion already known. This assumes that a scenario whose future is uncertain is always run with several repetitions, so that with a single one nothing disclosed since can have changed that result; a stochastic scenario must therefore not be run with a single repetition.

The choices, the entries of children of sequential ad-hoc subprocesses and the deliveries of messages compete without precedence: the candidates of all three are sorted by their local evaluations, a tie keeping that order. Where the model states a deferral for a decision point, the algorithm may also defer it, that is withhold its decisions until a reconsideration such as the arrival of a new candidate, as described in the documentation of the BPMN-OS engine. The deferral of a decision point is assessed as one further option, placed immediately after the best candidate of that decision point, if the deferral condition holds on the token of the decision point and if a reconsideration can still come. Its assessment simulates the greedy algorithm with the decision point deferred, so that the deferral is valued until it ends and the decision taken thereafter. Decision points deferred at earlier decisions stay deferred in every simulation until their own reconsideration. A deferral is taken only if its value exceeds that of every other option, acting winning a tie, so that the algorithm never waits without reason. Once a deferral is taken, the remaining options are assessed anew with the deferral in place, and the controller returns no decision only when every decision point is deferred.

To limit the computational effort, the following parameters can be provided:

- `candidates`: the maximum number of options to be assessed at each decision step. If set to a positive value, only the options first in the order above are assessed, which are the candidate decisions with the best local evaluations and the deferrals of their decision points, a deferral counting as an option, so that the same value reaches fewer candidate decisions when deferrals are offered. If set to zero (default), all options are assessed.
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
