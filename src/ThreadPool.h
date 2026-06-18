#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <type_traits>
#include <utility>
#include <memory>
#include <cstddef>

namespace BPMNOS::Rollout {

/**
 * @brief Fixed-size thread pool with multiple FIFO queues scheduled round-robin.
 *
 * `addQueue` creates an independent FIFO queue and returns its id; jobs are submitted into a chosen
 * queue. Workers schedule round-robin: on each dequeue a shared cursor advances to the next non-empty
 * queue and takes its front, so one job from every queue runs before any queue's second (breadth-first
 * across queues); empty/exhausted queues are skipped. With one queue per group (e.g. the repetitions of
 * one decision candidate), every group is sampled once before any group twice.
 *
 * `clearQueue` drops a queue's still-queued (unstarted) jobs — the cancellation primitive. It cannot
 * stop a job already running: a long task must observe its own cancellation cooperatively (e.g. a
 * `std::stop_token` captured in the task) and return early. A job submitted to the pool must not block
 * on the future of another job in the same pool — with a bounded number of workers that can deadlock;
 * submit leaf tasks and await their futures from a non-worker thread.
 */
class ThreadPool {
public:
  using QueueId = std::size_t;

  /// Creates a pool with the given number of worker threads; 0 means use all available hardware threads.
  explicit ThreadPool(unsigned int threads);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// Registers a new, initially empty queue and returns its id.
  QueueId addQueue();

  /// Submits a job into queue `id` and returns a future for its result.
  template <typename Task>
  auto submit(QueueId id, Task&& task) -> std::future<std::invoke_result_t<Task>> {
    using Result = std::invoke_result_t<Task>;
    auto packaged = std::make_shared<std::packaged_task<Result()>>(std::forward<Task>(task));
    std::future<Result> future = packaged->get_future();
    enqueue(id, [packaged]() { (*packaged)(); });
    return future;
  }

  /// Drops all still-queued (unstarted) jobs of queue `id`; jobs already running are unaffected.
  void clearQueue(QueueId id);

private:
  void enqueue(QueueId id, std::function<void()> job);
  void work();

  std::vector<std::queue<std::function<void()>>> queues;
  std::size_t cursor = 0;    ///< round-robin cursor over the queues
  std::size_t pending = 0;   ///< total queued jobs across all queues
  std::vector<std::thread> workers;
  std::mutex mutex;
  std::condition_variable available;
  bool stopping = false;
};

} // namespace BPMNOS::Rollout
