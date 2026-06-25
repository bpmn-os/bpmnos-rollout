#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <iostream>
#include <sstream>

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

// Silences the basic progress output the controller/dispatcher write to std::cout. For the duration of each
// test case std::cout's buffer is swapped for an in-memory one; the capture is replayed only when the test
// fails, so passing tests stay quiet while failures keep their full diagnostics (Catch's own failure output
// goes through std::cout too). std::cerr — the progress lines above — is left untouched.
class StdoutSuppressor : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        saved = std::cout.rdbuf(capture.rdbuf());
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        std::cout.rdbuf(saved);
        if (stats.totals.assertions.failed != 0) {
            std::cout << capture.str();
        }
        capture.str("");
        capture.clear();
    }

private:
    std::ostringstream capture;
    std::streambuf* saved = nullptr;
};

CATCH_REGISTER_LISTENER(StdoutSuppressor)

using namespace BPMNOS;

// Include all rollout tests here.
#include "threadpool/test.h"
#include "examples/travelling_salesperson_problem/test.h"
#include "examples/assignment_problem/test.h"
#include "examples/job_shop_scheduling_problem/test.h"
#include "examples/bin_packing_problem/test.h"
