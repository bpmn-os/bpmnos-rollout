#include "RolloutController.h"
#include "Results.h"

SCENARIO( "Assignment problem - rollout invariants", "[examples][assignment_problem]" ) {
  const std::string modelFile = "tests/examples/assignment_problem/Assignment_problem.bpmn";
  const std::vector<std::string> folders = { "tests/examples/assignment_problem" };
  // Greedy-trap 3×3 instance: greedy myopically takes the globally cheapest pair C1→S1 (cost 1), which
  // strands C2 (cheap only at S1) → greedy total 26 (C1→S1=1, C3→S3=5, C2→S2=20). The optimum is 10
  // (C1→S2=2, C2→S1=3, C3→S3=5), which the rollout finds via lookahead — so rollout strictly beats greedy.
  const std::string csv =
    "INSTANCE_ID; NODE_ID; INITIALIZATION\n"
    "Client1; ClientProcess;\n"
    "Client2; ClientProcess;\n"
    "Client3; ClientProcess;\n"
    "Server1; ServerProcess;\n"
    "Server2; ServerProcess;\n"
    "Server3; ServerProcess;\n"
  ;

  auto model = std::make_shared<const BPMNOS::Model::Model>(modelFile, folders);

  GIVEN( "Three clients and three servers" ) {
    using Results = BPMNOS::Rollout::Results;

    auto evaluator = std::make_shared<BPMNOS::Execution::LocalEvaluator>();

    auto greedyResults = std::make_shared<Results>();
    double greedyObj;
    {
      auto provider = std::make_shared<BPMNOS::Execution::StaticDataProvider>(model, csv);
      auto scenario = provider->createScenario(1);
      BPMNOS::Execution::Engine engine(model);
      BPMNOS::Execution::GreedyController controller(evaluator);
      controller.connect(&engine);
      engine.run(std::move(scenario));
      greedyObj = (double)engine.getSystemState()->getObjective();
      greedyResults->add(engine.getSystemState());
    }

    WHEN( "RolloutController runs with all candidates (default config)" ) {
      auto provider = std::make_shared<BPMNOS::Execution::StaticDataProvider>(model, csv);
      auto scenario = provider->createScenario();
      BPMNOS::Execution::Engine engine(model);
      BPMNOS::Rollout::RolloutController<Results> controller(evaluator, greedyResults, { .threads = 1 });
      controller.connect(&engine);
      engine.run(std::move(scenario));
      double rolloutObj = (double)engine.getSystemState()->getObjective();

      THEN( "Rollout strictly beats greedy: greedy trap cost 26 vs optimum 10 (invariant 1)" ) {
        REQUIRE( greedyObj == -26.0 );    // greedy trap: C1→S1=1, C3→S3=5, C2→S2=20
        REQUIRE( rolloutObj == -10.0 );   // optimum found by lookahead: C1→S2=2, C2→S1=3, C3→S3=5
        REQUIRE( rolloutObj > greedyObj );
      }
    }

    WHEN( "RolloutController runs with candidates=1" ) {
      auto provider = std::make_shared<BPMNOS::Execution::StaticDataProvider>(model, csv);
      auto scenario = provider->createScenario();
      BPMNOS::Execution::Engine engine(model);
      BPMNOS::Rollout::RolloutController<Results> controller(evaluator, greedyResults, { .candidates = 1 });
      controller.connect(&engine);
      engine.run(std::move(scenario));
      double rolloutObj = (double)engine.getSystemState()->getObjective();

      THEN( "Rollout objective equals greedy (invariant 2: only the greedy front is assessed)" ) {
        REQUIRE( rolloutObj == greedyObj );
      }
    }

    WHEN( "RolloutController runs with threads=1 and threads=4" ) {
      auto run = [&](unsigned int threads) {
        auto provider = std::make_shared<BPMNOS::Execution::StaticDataProvider>(model, csv);
        auto scenario = provider->createScenario();
        BPMNOS::Execution::Engine engine(model);
        BPMNOS::Rollout::RolloutController<Results> controller(evaluator, greedyResults, { .threads = threads });
        controller.connect(&engine);
        engine.run(std::move(scenario));
        return (double)engine.getSystemState()->getObjective();
      };

      THEN( "Objective is the same regardless of thread count (invariant 3)" ) {
        REQUIRE( run(1) == run(4) );
      }
    }
  }
}
