#include "ThreadPool.h"
#include <atomic>
#include <vector>
#include <future>

// Smoke test that the harness builds and links the rollout sources, and that the pool runs jobs and
// returns their results. (Not a controller test — those follow once the example fixtures are vendored.)
SCENARIO( "Thread pool runs submitted jobs across queues", "[threadpool]" ) {
  GIVEN( "A pool with two worker threads and two queues" ) {
    BPMNOS::Rollout::ThreadPool pool(2);
    auto first = pool.addQueue();
    auto second = pool.addQueue();

    WHEN( "A value-returning job and many side-effecting jobs are submitted" ) {
      auto answer = pool.submit(first, []{ return 6 * 7; });

      std::atomic<int> counter{0};
      std::vector<std::future<void>> jobs;
      for ( int i = 0; i < 100; ++i ) {
        jobs.push_back( pool.submit(second, [&counter]{ counter.fetch_add(1); }) );
      }

      THEN( "Each future yields its result and every side effect is observed" ) {
        REQUIRE( answer.get() == 42 );
        for ( auto& job : jobs ) {
          job.get();
        }
        REQUIRE( counter.load() == 100 );
      }
    }
  }
}

// Clearing a queue is the rollout's cancellation primitive (it drops a candidate's still-queued repetitions
// once the baseline dominates it). Verify it drops only unstarted jobs and breaks their futures.
SCENARIO( "Clearing a queue drops its unstarted jobs", "[threadpool]" ) {
  GIVEN( "A single-worker pool whose only worker is held by a gate job" ) {
    BPMNOS::Rollout::ThreadPool pool(1);
    auto queue = pool.addQueue();

    std::promise<void> started;
    std::promise<void> release;
    auto startedFuture = started.get_future();
    auto releaseFuture = release.get_future();

    // Occupy the single worker until released; it signals once it is actually running.
    auto gate = pool.submit(queue, [&]{ started.set_value(); releaseFuture.wait(); });
    startedFuture.wait();   // the worker is now busy in the gate, so any further jobs stay queued

    WHEN( "Jobs are queued behind the gate and the queue is cleared before release" ) {
      std::atomic<int> counter{0};
      std::vector<std::future<void>> jobs;
      for ( int i = 0; i < 50; ++i ) {
        jobs.push_back( pool.submit(queue, [&counter]{ counter.fetch_add(1); }) );
      }
      pool.clearQueue(queue);
      release.set_value();    // let the gate finish; the worker then finds the queue empty
      gate.get();

      THEN( "None of the queued jobs run and their futures are broken" ) {
        REQUIRE( counter.load() == 0 );
        for ( auto& job : jobs ) {
          REQUIRE_THROWS_AS( job.get(), std::future_error );
        }
      }
    }
  }
}
