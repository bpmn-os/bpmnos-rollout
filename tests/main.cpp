#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <iostream>

// Prints each test case's name and a green [pass] when it finishes, mirroring the engine's test runner.
class ProgressListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const& testInfo) override {
        std::cerr << testInfo.name << " " << std::flush;
    }

    void testCaseEnded(Catch::TestCaseStats const& testCaseStats) override {
        if (testCaseStats.totals.assertions.failed == 0) {
            std::cerr << "\033[32m[pass]\033[0m" << std::endl;
        }
    }
};

CATCH_REGISTER_LISTENER(ProgressListener)

using namespace BPMNOS;

// Include all rollout tests here.
#include "threadpool/test.h"
#include "examples/travelling_salesperson_problem/test.h"
#include "examples/assignment_problem/test.h"
#include "examples/job_shop_scheduling_problem/test.h"
#include "examples/bin_packing_problem/test.h"
