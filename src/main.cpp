#include <iostream>
#include <vector>
#include <string>
#include <future>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "RolloutController.h"
#include "Results.h"
#include "ThreadPool.h"

void print_usage() {
  std::cout << "Usage:" << std::endl;
  std::cout << "\tbpmnos-rollout --model <model file> --data <data file> [--json <json file>] [--provider {static|expected|dynamic|stochastic}] [--evaluator {local|guided}] [--folders <folder1> <folder2> ...] [--candidates] [--repetitions] [--threads] [--bsiection] [--verbose]" << std::endl;
  std::cout << "\trollout -m <model file> -d <data file> [-p <provider>] [-e <evaluator>] [-f <path1> <path2> ...] [-c] [-r] [-j] [-t] [-v]" << std::endl;
  std::cout << std::endl;
  std::cout << "\t-m, --model <model file>:             name of the BPMN model file" << std::endl;
  std::cout << "\t-d, --data <data file>:               name of the CSV file containing the instance data" << std::endl;
  std::cout << "\t-j, --json <json file>:               name of the file for the JSON output" << std::endl;
  std::cout << "\t-p, --provider {static|expected|dynamic|stochastic} (default: stochastic)" << std::endl;
  std::cout << "\t-e, --evaluator {local|guided} (default: guided)" << std::endl;
  std::cout << "\t-f, --folder <folder1> <folder2> ...: folders in which lookup tables can be found" << std::endl;
  std::cout << "\t-ca, --candidates:                     max candidate decisions assessed per step (0 = unlimited, default)" << std::endl;
  std::cout << "\t-r, --repetitions:                    rollouts per candidate for stochastic scenarios (default: 1)" << std::endl;
  std::cout << "\t-cu, --cutoff:                        max number of decisions made before switching to greedy (0 = unlimited, default)" << std::endl;
  std::cout << "\t-j, --threads:                        number of parallel rollout threads, 0 = all available (default: 1)" << std::endl;
  std::cout << "\t-b, --bisection:                      use bisection for choices" << std::endl;
  std::cout << "\t-v, --verbose:                        display the execution log" << std::endl;
  exit(1);
}

struct Arguments {
  Arguments() : verbose(false) {};
  std::string modelFile;
  std::string dataFile;
  std::string jsonFile;
  std::string providerName = "stochastic";
  std::string evaluatorName = "guided";
  std::vector<std::string> folders;
  bool bisection = false;
  bool verbose = false;
  unsigned int candidates = 0;  // max candidate decisions to be rolled out (0 = unlimited)
  unsigned int repetitions = 1; // rollouts per candidate for stochastic scenarios
  double cutoff = 0.0;          // fraction of greedy baseline decisions to be actually rolled out (0.0 = unlimited)
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
    else if ((arg == "--json" || arg == "-j") && i + 1 < argc) {
      args.jsonFile = argv[++i];
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
    else if ((arg == "--candidates" || arg == "-ca") && i + 1 < argc) {
      args.candidates = static_cast<unsigned int>(std::stoul(argv[++i]));
    }
    else if ((arg == "--repetitions" || arg == "-r") && i + 1 < argc) {
      args.repetitions = static_cast<unsigned int>(std::stoul(argv[++i]));
    }
    else if ((arg == "--cutoff" || arg == "-cu") && i + 1 < argc) {
      args.cutoff = static_cast<double>(std::stod(argv[++i]));
    }
    else if ((arg == "--threads" || arg == "-j") && i + 1 < argc) {
      args.threads = static_cast<unsigned int>(std::stoul(argv[++i]));
    }
    else if ((arg == "--bisection" || arg == "-b")) {
      args.bisection = true;
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


  auto dataProvider = createDataProvider();
  auto evaluator = createEvaluator();

  // Greedy baseline: run the greedy controller once per repetition (common random numbers via the
  // scenario id) and collect each final system state's weighted objective as the baseline the rollout is
  // compared against. The repetitions run in parallel on the thread pool (one queue).
  auto greedyResults = std::make_shared<BPMNOS::Rollout::Results>();
  // For the cutoff: count the rolled-out decisions of each greedy baseline run and keep the maximum.
  std::size_t maxDecisionCount = 0;
  {
    BPMNOS::Rollout::ThreadPool pool(args.threads);
    auto greedyQueue = pool.addQueue();
    std::mutex greedyResultsMutex;
    std::vector<std::future<void>> greedyRuns;
    greedyRuns.reserve(args.repetitions);

    for ( unsigned int scenarioId = 0; scenarioId < args.repetitions; ++scenarioId ) {
      greedyRuns.push_back( pool.submit(greedyQueue, [&, scenarioId]() {
        // createScenario(s) seeds the scenario at provider.seed + s. Offset by +1 so the greedy baseline
        // samples provider.seed+1 .. provider.seed+repetitions — the same futures the rollout forks use
        // (getSeed()+index+1), giving common random numbers between the baseline and the rollouts, and
        // keeping both off the base seed that the live run will realize.
        auto greedyScenario = dataProvider->createScenario(scenarioId + 1);

        BPMNOS::Rollout::DecisionCounter greedyDecisionCounter;   // declared before the engine so it outlives it
        BPMNOS::Execution::Engine greedyEngine;
        BPMNOS::Execution::GreedyController greedyController(evaluator.get());
        greedyController.connect(&greedyEngine);
        greedyDecisionCounter.connect(&greedyEngine);   // count the baseline's rolled-out decisions for the cutoff
        BPMNOS::Execution::TimeWarp greedyTimeHandler;
        greedyTimeHandler.connect(&greedyEngine);
        greedyEngine.run(greedyScenario.get());

        // The final system state is valid while greedyEngine is alive (here); add it under the lock.
        std::lock_guard greedyResultsLock(greedyResultsMutex);
        greedyResults->add(greedyEngine.getSystemState());
        maxDecisionCount = std::max(maxDecisionCount, greedyDecisionCounter.count());
      }) );
    }

    // Wait for each run to finish (and rethrow any error it threw)
    for ( auto& greedyRun : greedyRuns ) {
      greedyRun.get();
    }
  }
  
  auto scenario = dataProvider->createScenario();
  BPMNOS::Execution::Engine engine;
  auto cutoff = (unsigned int)std::ceil(args.cutoff * (double)maxDecisionCount);
  BPMNOS::Rollout::RolloutController<BPMNOS::Rollout::Results>::Config config{ args.candidates, args.repetitions, cutoff, args.threads, args.bisection, args.verbose };
  BPMNOS::Rollout::RolloutController<BPMNOS::Rollout::Results> controller(evaluator.get(), greedyResults, config);
  controller.connect(&engine);

  BPMNOS::Execution::TimeWarp timeHandler;
  timeHandler.connect(&engine);

  std::ofstream jsonStream;
  std::unique_ptr<BPMNOS::Execution::Recorder> recorder;
  if (!args.jsonFile.empty()) {
    jsonStream.open(args.jsonFile);
    if (!jsonStream.is_open()) {
      std::cerr << "Error: unable to open JSON output file: " << args.jsonFile << "\n";
      return 1;
    }
    recorder = std::make_unique<BPMNOS::Execution::Recorder>(BPMNOS::Execution::Recorder::Config{ .stream = jsonStream, .tagged = true });
    recorder->subscribe(&engine);
  }

  engine.run(scenario.get());

  auto objective = (float)engine.getSystemState()->getWeightedObjective();
  std::cout << "Objective (maximization): " << objective << std::endl;
  std::cout << "Objective (minimization): " << -objective  << std::endl;

  return 0;
}
