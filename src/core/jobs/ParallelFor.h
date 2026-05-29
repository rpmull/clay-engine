#pragma once
#include "JobSystem.h"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <algorithm>

// Use std::latch if you have C++20 <latch>; otherwise use a heap-owned cv/mutex.
#if defined(__cpp_lib_latch) && __cpp_lib_latch >= 201907L
#include <latch>
#define CM_USE_LATCH 1
#else
#define CM_USE_LATCH 0
#endif

// Optimized parallel_for with reduced allocations and better cache utilization
// Designed to match or exceed Unity/Unreal parallel job performance

// Determine optimal chunk size based on work granularity
inline size_t ComputeOptimalChunkSize(size_t total, size_t workerCount, size_t minGranularity = 64) {
   if (workerCount == 0) workerCount = 1;
   // Aim for 4x oversubscription to hide latency, but respect minimum granularity
   size_t idealChunks = workerCount * 4;
   size_t chunkSize = (total + idealChunks - 1) / idealChunks;
   return std::max(chunkSize, minGranularity);
}

template<class Fn>
inline void parallel_for(JobSystem& js,
   size_t begin, size_t end, size_t chunk,
   Fn&& fn,
   JobSystem::Priority priority = JobSystem::Priority::Normal)
{
   if (end <= begin) return;

   const size_t total = end - begin;
   
   // For small workloads, run inline to avoid job system overhead
   if (total <= chunk || chunk == 0) {
      fn(begin, total);
      return;
   }

   const size_t groups = (total + chunk - 1) / chunk;

   // Store the first exception from any slice using atomic pointer for lock-free access
   std::atomic<std::exception_ptr*> first_error{nullptr};

   // Make the callable lifetime-safe for jobs
   using FnT = std::decay_t<Fn>;
   auto fn_holder = std::make_shared<FnT>(std::forward<Fn>(fn));

#if CM_USE_LATCH
   std::latch done((std::ptrdiff_t)groups);
   auto done_ptr = &done;
#else
   // Heap-owned sync objects for thread-safe completion tracking
   struct Sync {
      std::mutex m;
      std::condition_variable cv;
      std::atomic<size_t> remaining{0};
   };
   auto sync = std::make_shared<Sync>();
   sync->remaining.store(groups, std::memory_order_relaxed);
#endif

   // Capture error pointer for cleanup
   auto first_error_ptr = &first_error;

   for (size_t s = begin; s < end; s += chunk) {
      const size_t c = std::min(chunk, end - s);

      auto jobFn = [s, c, fn_holder, first_error_ptr
#if CM_USE_LATCH
         , done_ptr
#else
         , sync
#endif
      ]() {
         try {
            (*fn_holder)(s, c);
         }
         catch (...) {
            // Try to set first error atomically
            std::exception_ptr* expected = nullptr;
            std::exception_ptr* newErr = new std::exception_ptr(std::current_exception());
            if (!first_error_ptr->compare_exchange_strong(expected, newErr, 
               std::memory_order_acq_rel, std::memory_order_relaxed)) {
               delete newErr; // Another thread already set an error
            }
         }

#if CM_USE_LATCH
         done_ptr->count_down();
#else
         if (sync->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(sync->m);
            sync->cv.notify_one();
         }
#endif
      };

      bool ok = js.Enqueue(std::move(jobFn), priority);

      // If enqueue fails (shutdown), run slice inline to guarantee progress.
      if (!ok) {
         try {
            (*fn_holder)(s, c);
         }
         catch (...) {
            std::exception_ptr* expected = nullptr;
            std::exception_ptr* newErr = new std::exception_ptr(std::current_exception());
            if (!first_error_ptr->compare_exchange_strong(expected, newErr,
               std::memory_order_acq_rel, std::memory_order_relaxed)) {
               delete newErr;
            }
         }
#if CM_USE_LATCH
         done_ptr->count_down();
#else
         if (sync->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(sync->m);
            sync->cv.notify_one();
         }
#endif
      }
   }

#if CM_USE_LATCH
   done.wait();
#else
   // Wait until remaining == 0
   std::unique_lock<std::mutex> lk(sync->m);
   sync->cv.wait(lk, [&] { return sync->remaining.load(std::memory_order_acquire) == 0; });
#endif

   // Check for and rethrow exception
   std::exception_ptr* err = first_error.load(std::memory_order_acquire);
   if (err) {
      std::exception_ptr copy = *err;
      delete err;
      std::rethrow_exception(copy);
   }
}

// Convenience overload with automatic chunk sizing
template<class Fn>
inline void parallel_for_auto(JobSystem& js,
   size_t begin, size_t end,
   Fn&& fn,
   size_t minGranularity = 64,
   JobSystem::Priority priority = JobSystem::Priority::Normal)
{
   size_t chunk = ComputeOptimalChunkSize(end - begin, js.GetWorkerCount(), minGranularity);
   parallel_for(js, begin, end, chunk, std::forward<Fn>(fn), priority);
}

// Parallel for each over a range (like std::for_each but parallel)
template<class Iterator, class Fn>
inline void parallel_for_each(JobSystem& js,
   Iterator begin, Iterator end,
   Fn&& fn,
   JobSystem::Priority priority = JobSystem::Priority::Normal)
{
   auto count = std::distance(begin, end);
   if (count <= 0) return;
   
   size_t chunk = ComputeOptimalChunkSize(count, js.GetWorkerCount(), 32);
   
   parallel_for(js, 0, static_cast<size_t>(count), chunk,
      [begin, &fn](size_t start, size_t num) {
         auto it = begin;
         std::advance(it, start);
         for (size_t i = 0; i < num; ++i, ++it) {
            fn(*it);
         }
      },
      priority);
}
