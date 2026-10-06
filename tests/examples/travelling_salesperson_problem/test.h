#include "RolloutController.h"
#include "Results.h"

SCENARIO( "Travelling salesperson problem - rollout invariants", "[examples][travelling_salesperson_problem]" ) {
  const std::string modelFile = "tests/examples/travelling_salesperson_problem/Travelling_salesperson_problem.bpmn";
  const std::vector<std::string> folders = { "tests/examples/travelling_salesperson_problem" };
  const std::string csv =
    "INSTANCE_ID; NODE_ID; INITIALIZATION\n"
    "Instance1; TravellingSalesperson_Process; speed := 1\n"
    "Instance1; TravellingSalesperson_Process; origin := \"Hamburg\"\n"
    "Instance1; TravellingSalesperson_Process; locations := [\"Munich\",\"Berlin\",\"Cologne\"]\n"
  ;

  auto model = std::make_shared<const BPMNOS::Model::Model>(modelFile, folders);

  GIVEN( "A static TSP: Hamburg origin, three cities (Munich/Berlin/Cologne)" ) {
    // Greedy nearest-neighbour: Hamburg→Berlin(296)→Cologne(573)→Munich(575)→Hamburg(778) = 2222.
    // Optimal tour: Hamburg→Berlin(296)→Munich(585)→Cologne(575)→Hamburg(432) = 1888.
    using Results = BPMNOS::Rollout::Results;

    auto evaluator = std::make_shared<BPMNOS::Execution::LocalEvaluator>();

    // Greedy baseline (seed+1 per CRN convention); keeps results alive for all WHEN blocks.
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

      THEN( "Rollout objective is at least as good as greedy (invariant 1)" ) {
        REQUIRE( greedyObj  == -2222 );
        REQUIRE( rolloutObj == -1888 );
        REQUIRE( rolloutObj >= greedyObj );
      }
    }

    WHEN( "RolloutController runs with candidates=1" ) {
      auto provider = std::make_shared<BPMNOS::Execution::StaticDataProvider>(model, csv);
      auto scenario = provider->createScenario();
      BPMNOS::Execution::Engine engine(model);
      BPMNOS::Execution::Recorder recorder;
      BPMNOS::Rollout::RolloutController<Results> controller(evaluator, greedyResults, { .candidates = 1 });
      controller.connect(&engine);
      recorder.subscribe(&engine);
      engine.run(std::move(scenario));
      double rolloutObj = (double)engine.getSystemState()->getObjective();

      THEN( "Rollout objective equals greedy (invariant 2: only the greedy front is assessed)" ) {
        REQUIRE( rolloutObj == greedyObj );
      }
      AND_THEN( "Locations are visited in the same nearest-neighbour order as greedy" ) {
        auto visitLog = recorder.find(nlohmann::json{{"nodeId", "VisitLocation"}, {"state", "ENTERED"}});
        REQUIRE( visitLog.size() == 3 );
        REQUIRE( visitLog[0]["status"]["location"] == "Berlin" );
        REQUIRE( visitLog[1]["status"]["location"] == "Cologne" );
        REQUIRE( visitLog[2]["status"]["location"] == "Munich" );
      }
    }

    WHEN( "RolloutController runs with threads=1 and threads=4" ) {
      auto run = [&](unsigned int threads) {
        auto provider = std::make_shared<BPMNOS::Execution::StaticDataProvider>(model, csv);
        auto scenario = provider->createScenario();
        BPMNOS::Execution::Engine engine(model);
        BPMNOS::Rollout::RolloutController<Results> controller(evaluator, greedyResults, { .threads = threads } );
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
