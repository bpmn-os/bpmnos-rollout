#include "ThreadPool.h"

using namespace BPMNOS::Rollout;

ThreadPool::ThreadPool(unsigned int threads) {
  // 0 requests all available hardware threads; hardware_concurrency() may itself return 0 (not computable),
  // so fall back to a single worker in that case.
  unsigned int count = threads ? threads : std::thread::hardware_concurrency();
  if ( count == 0 ) {
    count = 1u;
  }
  workers.reserve(count);
  for ( unsigned int i = 0; i < count; ++i ) {
    workers.emplace_back([this]() { work(); });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard lock(mutex);
    stopping = true;
  }
  available.notify_all();
  for ( auto& thread : workers ) {
    thread.join();
  }
}

ThreadPool::QueueId ThreadPool::addQueue() {
  std::lock_guard lock(mutex);
  queues.emplace_back();
  return queues.size() - 1;
}

void ThreadPool::enqueue(QueueId id, std::function<void()> job) {
  {
    std::lock_guard lock(mutex);
    queues[id].push(std::move(job));
    ++pending;
  }
  available.notify_one();
}

void ThreadPool::clearQueue(QueueId id) {
  std::lock_guard lock(mutex);
  pending -= queues[id].size();
  std::queue<std::function<void()>> empty;
  queues[id].swap(empty);
}

void ThreadPool::work() {
  while ( true ) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex);
      available.wait(lock, [this]() { return stopping || pending > 0; });
      if ( pending == 0 ) {
        return; // stopping and no work left
      }
      // Round-robin: take the front of the next non-empty queue.
      std::size_t n = queues.size();
      for ( std::size_t i = 0; i < n; ++i ) {
        std::size_t index = (cursor + i) % n;
        if ( !queues[index].empty() ) {
          job = std::move(queues[index].front());
          queues[index].pop();
          --pending;
          cursor = (index + 1) % n;
          break;
        }
      }
    }
    if ( job ) {
      job();
    }
  }
}
