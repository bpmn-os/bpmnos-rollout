#include "RolloutController.h"
#include "Results.h"

SCENARIO( "Job shop scheduling problem - rollout invariants", "[examples][job_shop_scheduling_problem]" ) {
  const std::string model = "tests/examples/job_shop_scheduling_problem/Job_shop_scheduling_problem.bpmn";
  const std::string csv =
    "INSTANCE_ID; NODE_ID; INITIALIZATION\n"
    "Machine1; MachineProcess; jobs := 2\n"
    "Machine2; MachineProcess; jobs := 3\n"
    "Machine3; MachineProcess; jobs := 3\n"
    "Order1; OrderProcess; machines := [\"Machine1\",\"Machine2\",\"Machine3\"]\n"
    "Order1; OrderProcess; durations := [3,2,2]\n"
    "Order2; OrderProcess; machines := [\"Machine1\",\"Machine3\",\"Machine2\"]\n"
    "Order2; OrderProcess; durations := [2,1,4]\n"
    "Order3; OrderProcess; machines := [\"Machine2\",\"Machine3\"]\n"
    "Order3; OrderProcess; durations := [4,3]\n"
  ;

  REQUIRE_NOTHROW( BPMNOS::Model::Model(model) );

  GIVEN( "Three machines and three orders" ) {
    using Results = BPMNOS::Rollout::Results;

    BPMNOS::Execution::GuidedEvaluator evaluator;

    auto greedyResults = std::make_shared<Results>();
    double greedyObj;
    {
      BPMNOS::Model::StaticDataProvider provider(model, csv);
      auto scenario = provider.createScenario(1);
      BPMNOS::Execution::Engine engine;
      BPMNOS::Execution::GreedyController controller(&evaluator);
      controller.connect(&engine);
      BPMNOS::Execution::TimeWarp timeHandler;
      timeHandler.connect(&engine);
      engine.run(scenario.get());
      greedyObj = (double)engine.getSystemState()->getWeightedObjective();
      greedyResults->add(engine.getSystemState());
    }

    WHEN( "RolloutController runs with all candidates (default config)" ) {
      BPMNOS::Model::StaticDataProvider provider(model, csv);
      auto scenario = provider.createScenario();
      BPMNOS::Execution::Engine engine;
      BPMNOS::Execution::Recorder recorder;
      BPMNOS::Rollout::RolloutController<Results> controller(&evaluator, greedyResults, { .threads = 1 });
      controller.connect(&engine);
      BPMNOS::Execution::TimeWarp timeHandler;
      timeHandler.connect(&engine);
      recorder.subscribe(&engine);
      engine.run(scenario.get());
      double rolloutObj = (double)engine.getSystemState()->getWeightedObjective();

      THEN( "No process instance fails" ) {
        REQUIRE( recorder.find(nlohmann::json{{"state", "FAILED"}}).size() == 0 );
      }
      AND_THEN( "All six process instances complete" ) {
        REQUIRE( recorder.find({{"state","COMPLETED"}}, nlohmann::json{{"nodeId",nullptr},{"event",nullptr},{"decision",nullptr}}).size() == 3 + 3 );
      }
      AND_THEN( "Rollout objective is at least as good as greedy (invariant 1)" ) {
        REQUIRE( rolloutObj >= greedyObj );
      }
    }

    WHEN( "RolloutController runs with threads=1 and threads=4" ) {
      auto run = [&](unsigned int threads) {
        BPMNOS::Model::StaticDataProvider provider(model, csv);
        auto scenario = provider.createScenario();
        BPMNOS::Execution::Engine engine;
        BPMNOS::Rollout::RolloutController<Results> controller(&evaluator, greedyResults, { .threads = threads });
        controller.connect(&engine);
        BPMNOS::Execution::TimeWarp timeHandler;
        timeHandler.connect(&engine);
        engine.run(scenario.get());
        return (double)engine.getSystemState()->getWeightedObjective();
      };

      THEN( "Objective is the same regardless of thread count (invariant 3)" ) {
        REQUIRE( run(1) == run(4) );
      }
    }
  }
}
