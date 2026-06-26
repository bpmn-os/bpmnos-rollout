# A rollout algorithm for BPMN-OS

This project provides a one-step lookahead [rollout algorithm](https://www.google.com/url?sa=t&source=web&rct=j&opi=89978449&url=https://web.mit.edu/dimitrib/www/Rollouts_Survey.pdf&ved=2ahUKEwjfjM6TkOOUAxV8X_EDHXsuBT4QFnoECCAQAQ&usg=AOvVaw0i0d_YnpfctznI7HiVMTyE) for BPMN-OS. 

The algorithm assesses each candidate decision by determining the final objective after assuming the decision is made and completing the simulation using a greedy algorithm.

To limit the computational effort, the following parameters can be provided:

- `candidates`: the maximum number of candidate decisions to be assessed at each decision step. If set to a positive value, only the candidate decisions with the best local evaluations are assessed. If set to zero (default), all candidates are assessed.
- `repetitions`: the number of rollouts per candidate decision. This parameter is only relevant for stochastic scenarios and the default value is 1. If set to a value above 1, the average objective value is used to decide on the chosen candidate.
- `cutoff`: limits how many decisions are rolled out before the controller falls back to the greedy algorithm for the remainder of the run. It is given as a fraction of the number of decisions made in the greedy baseline run (e.g. `0.5` rolls out the first half of the decisions and takes the rest greedily). If set to zero (default), all decisions are rolled out.
- `threads`: the number of threads to be used for parallel rollouts (default: 1). If set to zero, all available hardware threads are used.
- `bisection`: if set, a choice is assessed by bisection instead of by enumerating all of its alternatives (default). Bisection is useful for a choice over a bounded numeric value.