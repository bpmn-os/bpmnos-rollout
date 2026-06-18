#include <iostream>
#include <vector>
#include <string>
#include <future>
#include <mutex>
#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "RolloutController.h"
#include "Results.h"
#include "ThreadPool.h"

void print_usage() {
  std::cout << "Usage:" << std::endl;
  std::cout << "\trollout --model <model file> --data <data file> [--provider {static|expected|dynamic|stochastic}] [--evaluator {local|guided}] [--folders <folder1> <folder2> ...] [--candidates] [--repetitions] [--threads] [--timeout] [--verbose]" << std::endl;
  std::cout << "\trollout -m <model file> -d <data file> [-p <provider>] [-e <evaluator>] [-f <path1> <path2> ...] [-c] [-r] [-j] [-t] [-v]" << std::endl;
  std::cout << std::endl;
  std::cout << "\t-m, --model <model file>:             name of the BPMN model file" << std::endl;
  std::cout << "\t-d, --data <data file>:               name of the CSV file containing the instance data" << std::endl;
  std::cout << "\t-p, --provider {static|expected|dynamic|stochastic} (default: stochastic)" << std::endl;
  std::cout << "\t-e, --evaluator {local|guided} (default: guided)" << std::endl;
  std::cout << "\t-f, --folder <folder1> <folder2> ...: folders in which lookup tables can be found" << std::endl;
  std::cout << "\t-c, --candidates:                     max candidate decisions assessed per step (0 = all, default: 0)" << std::endl;
  std::cout << "\t-r, --repetitions:                    rollouts per candidate for stochastic scenarios (default: 1)" << std::endl;
  std::cout << "\t-j, --threads:                        number of parallel rollout threads, 0 = all available (default: 1)" << std::endl;
  std::cout << "\t-t, --timeout:                        time when execution is terminated" << std::endl;
  std::cout << "\t-v, --verbose:                        display the execution log" << std::endl;
  exit(1);
}

struct Arguments {
  Arguments() : verbose(false) {};
  std::string modelFile;
  std::string dataFile;
  std::string providerName = "stochastic";
  std::string evaluatorName = "guided";
  std::vector<std::string> folders;
  std::optional<BPMNOS::number> timeout;
  bool verbose;
  unsigned int candidates = 0;  // max candidate decisions assessed per contested decision (0 = all)
  unsigned int repetitions = 1; // rollouts per candidate for stochastic scenarios
  unsigned int threads = 1;     // number of parallel rollout threads
};

Arguments parse_arguments(int argc, char* argv[]) {
  Arguments args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if ((arg == "--model" || arg == "-m") && i + 1 < argc) {
      args.modelFile = argv[++i];
    }
    else if ((arg == "--data" || arg == "-d") && i + 1 < argc) {
      args.dataFile = argv[++i];
    }
    else if ((arg == "--provider" || arg == "-p") && i + 1 < argc) {
      args.providerName = argv[++i];
    }
    else if ((arg == "--evaluator" || arg == "-e") && i + 1 < argc) {
      args.evaluatorName = argv[++i];
    }
    else if ((arg == "--folders" || arg == "-f") && i + 1 < argc) {
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        args.folders.push_back(argv[++i]);
      }
    }
    else if ((arg == "--timeout" || arg == "-t") && i + 1 < argc) {
      args.timeout = BPMNOS::to_number(std::string(argv[++i]),BPMNOS::STRING);
    }
    else if ((arg == "--candidates" || arg == "-c") && i + 1 < argc) {
      args.candidates = static_cast<unsigned int>(std::stoul(argv[++i]));
    }
    else if ((arg == "--repetitions" || arg == "-r") && i + 1 < argc) {
      args.repetitions = static_cast<unsigned int>(std::stoul(argv[++i]));
    }
    else if ((arg == "--threads" || arg == "-j") && i + 1 < argc) {
      args.threads = static_cast<unsigned int>(std::stoul(argv[++i]));
    }
    else if ((arg == "--verbose" || arg == "-v")) {
      args.verbose = true;
    }
    else {
      std::cerr << "Unknown parameter: " << arg << "\n";
      print_usage();
    }
  }

  if (args.modelFile.empty() || args.dataFile.empty()) {
    std::cerr << "Error: --model and --data are required.\n";
    print_usage();
  }

  return args;
}

int main(int argc, char* argv[]) {
  if (argc < 5) {
    print_usage();
  }

  Arguments args = parse_arguments(argc, argv);

  auto createDataProvider = [&args]() -> std::unique_ptr<BPMNOS::Model::DataProvider> {
    if (args.providerName == "static") {
      return std::make_unique<BPMNOS::Model::StaticDataProvider>(args.modelFile,args.folders,args.dataFile);
    }
    else if (args.providerName == "expected") {
      return std::make_unique<BPMNOS::Model::ExpectedValueDataProvider>(args.modelFile,args.folders,args.dataFile);
    }
    else if (args.providerName == "dynamic") {
      return std::make_unique<BPMNOS::Model::DynamicDataProvider>(args.modelFile,args.folders,args.dataFile);
    }
    else if (args.providerName == "stochastic") {
      return std::make_unique<BPMNOS::Model::StochasticDataProvider>(args.modelFile,args.folders,args.dataFile);
    }
    else {
      std::cerr << "Error: unknown data provider.\n";
      print_usage();
    }
    return nullptr;
  };

  auto createEvaluator = [&args]() -> std::unique_ptr<BPMNOS::Execution::Evaluator> {
    if (args.evaluatorName == "local") {
      return std::make_unique<BPMNOS::Execution::LocalEvaluator>();
    }
    else if (args.evaluatorName == "guided") {
      return std::make_unique<BPMNOS::Execution::GuidedEvaluator>();
    }
    else {
      std::cerr << "Error: unknown evaluator.\n";
      print_usage();
    }
    return nullptr;
  };

  auto createRecorder = [&args]() -> std::unique_ptr<BPMNOS::Execution::Recorder> {
    if (args.verbose) {
      return std::make_unique<BPMNOS::Execution::Recorder>(std::cout);
    }
    else {
      return std::make_unique<BPMNOS::Execution::Recorder>();
    }
  };

  auto dataProvider = createDataProvider();
  auto scenario = dataProvider->createScenario();

  BPMNOS::Execution::Engine engine;

  auto evaluator = createEvaluator();

  // Greedy baseline: run the greedy controller once per repetition (common random numbers via the
  // scenario id) and collect each final system state's weighted objective as the baseline the rollout is
  // compared against. The repetitions run in parallel on the thread pool (one queue).
  BPMNOS::Rollout::Results greedyResults;
  {
    BPMNOS::Rollout::ThreadPool pool(args.threads);
    auto greedyQueue = pool.addQueue();
    std::mutex greedyResultsMutex;
    std::vector<std::future<void>> greedyRuns;
    greedyRuns.reserve(args.repetitions);

    for ( unsigned int scenarioId = 0; scenarioId < args.repetitions; ++scenarioId ) {
      greedyRuns.push_back( pool.submit(greedyQueue, [&, scenarioId]() {
        auto greedyScenario = dataProvider->createScenario(scenarioId);

        BPMNOS::Execution::Engine greedyEngine;
        BPMNOS::Execution::GreedyController greedyController(evaluator.get());
        greedyController.connect(&greedyEngine);
        BPMNOS::Execution::MyopicMessageTaskTerminator greedyMessageTaskTerminator;
        BPMNOS::Execution::TimeWarp greedyTimeHandler;
        greedyMessageTaskTerminator.connect(&greedyEngine);
        greedyTimeHandler.connect(&greedyEngine);

        if (args.timeout.has_value()) {
          greedyEngine.run(greedyScenario.get(), args.timeout.value());
        }
        else {
          greedyEngine.run(greedyScenario.get());
        }

        // The final system state is valid while greedyEngine is alive (here); add it under the lock.
        std::lock_guard greedyResultsLock(greedyResultsMutex);
        greedyResults.add(greedyEngine.getSystemState());
      }) );
    }

    // Wait for each run to finish (and rethrow any error it threw)
    for ( auto& greedyRun : greedyRuns ) {
      greedyRun.get();
    }
  }

  BPMNOS::Rollout::RolloutController<BPMNOS::Rollout::Results>::Config config{ args.candidates, args.repetitions, args.threads };
  BPMNOS::Rollout::RolloutController<BPMNOS::Rollout::Results> controller(evaluator.get(), greedyResults, config);
  controller.connect(&engine);

  BPMNOS::Execution::MyopicMessageTaskTerminator messageTaskTerminator;
  BPMNOS::Execution::TimeWarp timeHandler;
  messageTaskTerminator.connect(&engine);
  timeHandler.connect(&engine);

  auto recorder = createRecorder();
  recorder->subscribe(&engine);

  if (args.timeout.has_value()) {
    engine.run(scenario.get(),args.timeout.value());
  }
  else {
    engine.run(scenario.get());
  }
  BPMNOS::number objective = engine.getSystemState()->getWeightedObjective();

  std::cout << "Objective: " << objective << std::endl;

  return 0;
}
