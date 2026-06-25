#include "RolloutController.h"
#include "Results.h"

SCENARIO( "Bin packing problem (stochastic) - rollout invariants", "[examples][bin_packing_problem]" ) {
  const std::string model = "tests/examples/bin_packing_problem/Bin_packing_problem.bpmn";
  // Stochastic sizes: when a bin inspects an item, its size is revealed as the nominal size increased by
  // 0, 10, or 20. So every rollout repetition realises different sizes. An item that no
  // longer fits the bin has to re-open it, increasing the bin count.
  const std::string csv =
    "INSTANCE_ID; NODE_ID; INITIALIZATION; DISCLOSURE; READY; COMPLETION\n"
    ";; bins := 3;;;\n"
    ";; items := 4;;;\n"
    "Bin1; BinProcess; capacity := 100;;;\n"
    "Bin1; InspectItem;;; size := min{capacity, size + 10*uniform_int(0,2)};\n"
    "Bin2; BinProcess; capacity := 100;;;\n"
    "Bin2; InspectItem;;; size := min{capacity, size + 10*uniform_int(0,2)};\n"
    "Bin3; BinProcess; capacity := 100;;;\n"
    "Bin3; InspectItem;;; size := min{capacity, size + 10*uniform_int(0,2)};\n"
    "Item1; ItemProcess; size := 60;;;\n"
    "Item2; ItemProcess; size := 50;;;\n"
    "Item3; ItemProcess; size := 40;;;\n"
    "Item4; ItemProcess; size := 30;;;\n"
  ;

  REQUIRE_NOTHROW( BPMNOS::Model::Model(model) );

  GIVEN( "Three bins and four items with stochastic sizes" ) {
    using Results = BPMNOS::Rollout::Results;
    using Controller = BPMNOS::Rollout::RolloutController<Results>;
    constexpr unsigned int repetitions = 4;

    BPMNOS::Execution::GuidedEvaluator evaluator;

    // Greedy baseline: run greedy once per repetition at seeds 1..repetitions (common random numbers with
    // the rollouts, which fork at getSeed()+index+1), so the baseline carries `repetitions` samples — the
    // >= 2 the dominance prediction bound requires.
    auto greedyResults = std::make_shared<Results>();
    {
      BPMNOS::Model::StochasticDataProvider provider(model, csv);
      for ( unsigned int scenarioId = 1; scenarioId <= repetitions; ++scenarioId ) {
        auto scenario = provider.createScenario(scenarioId);
        BPMNOS::Execution::Engine engine;
        BPMNOS::Execution::GreedyController controller(&evaluator);
        controller.connect(&engine);
        BPMNOS::Execution::TimeWarp timeHandler;
        timeHandler.connect(&engine);
        engine.run(scenario.get());
        greedyResults->add(engine.getSystemState());
      }
    }

    // Run the rollout on the base-seed scenario; report its objective and any failures.
    auto run = [&]() {
      BPMNOS::Model::StochasticDataProvider provider(model, csv);
      auto scenario = provider.createScenario();   // base seed 0; rollouts fork at 1..repetitions
      BPMNOS::Execution::Engine engine;
      BPMNOS::Execution::Recorder recorder;
      Controller controller(&evaluator, greedyResults, { .repetitions = repetitions, .threads = 1, .verbose = true });
      controller.connect(&engine);
      BPMNOS::Execution::TimeWarp timeHandler;
      timeHandler.connect(&engine);
      recorder.subscribe(&engine);
      engine.run(scenario.get());
      auto failures = recorder.find(nlohmann::json{{"state","FAILED"}}).size();
      return std::make_pair((double)engine.getSystemState()->getWeightedObjective(), failures);
    };

    WHEN( "RolloutController runs on the stochastic scenario (repetitions > 1)" ) {
      auto [objective, failures] = run();

      THEN( "no process instance fails" ) {
        REQUIRE( failures == 0 );
      }
      AND_THEN( "the objective is reproducible across identical runs" ) {
        REQUIRE( run().first == objective );
      }
    }
  }
}
