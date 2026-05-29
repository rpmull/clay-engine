#pragma once
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <algorithm>
#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// High-performance job system with priority queues
// Uses a single mutex for correctness with priority-based scheduling

class JobSystem {
public:
   // Priority levels for job scheduling
   enum class Priority : uint8_t {
      High = 0,    // Time-critical (physics, input)
      Normal = 1,  // Standard gameplay/rendering jobs
      Low = 2,     // Background loading, streaming
      Count = 3
   };

   explicit JobSystem(size_t threads =
      (std::max)(1u, std::thread::hardware_concurrency()))
   {
      start(threads ? threads : 1);
   }

   ~JobSystem() { stop(); }

   // Queue a job with optional priority; returns false if system is stopping.
   // Default priority is Normal for backwards compatibility.
   bool Enqueue(std::function<void()> job, Priority priority = Priority::Normal) {
      {
         std::lock_guard<std::mutex> lk(m_mutex);
         if (m_stopping) return false;
         m_queues[static_cast<size_t>(priority)].push_back(std::move(job));
         ++m_outstandingJobs;
      }
      m_cv.notify_one();
      return true;
   }

   // Get number of worker threads
   size_t GetWorkerCount() const { return m_workers.size(); }

   // Get approximate pending job count (for load balancing decisions)
   size_t GetPendingJobCount() const {
      std::lock_guard<std::mutex> lk(m_mutex);
      return m_outstandingJobs;
   }

   // Wait for all current jobs to complete (useful for synchronization points)
   // Returns early if the JobSystem is shutting down to avoid deadlocks during application shutdown
   void WaitIdle() {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_idleCv.wait(lk, [this] {
         return m_stopping || m_outstandingJobs == 0;
      });
   }

private:
   void start(size_t n) {
      m_stopping = false;
      m_activeWorkers.store(0, std::memory_order_relaxed);
      m_outstandingJobs = 0;
      m_workers.reserve(n);
      
      for (size_t i = 0; i < n; ++i) {
         m_workers.emplace_back([this] { workerLoop(); });
      }
   }

   void workerLoop() {
      for (;;) {
         std::function<void()> job;
         
         {
            std::unique_lock<std::mutex> lk(m_mutex);
            
            // Wait until there's work or we're stopping
            m_cv.wait(lk, [this] { 
               if (m_stopping) return true;
               for (size_t i = 0; i < static_cast<size_t>(Priority::Count); ++i) {
                  if (!m_queues[i].empty()) return true;
               }
               return false;
            });
            
            // Check if we should exit
            if (m_stopping) {
               // Drain remaining jobs before exit
               for (size_t i = 0; i < static_cast<size_t>(Priority::Count); ++i) {
                  if (!m_queues[i].empty()) {
                     job = std::move(m_queues[i].front());
                     m_queues[i].pop_front();
                     break;
                  }
               }
               if (!job) return; // No more jobs, exit
            } else {
               // Find highest priority non-empty queue
               for (size_t i = 0; i < static_cast<size_t>(Priority::Count); ++i) {
                  if (!m_queues[i].empty()) {
                     job = std::move(m_queues[i].front());
                     m_queues[i].pop_front();
                     break;
                  }
               }
               if (!job) continue; // Spurious wakeup
            }
         }
         
         // Execute job outside the lock
         m_activeWorkers.fetch_add(1, std::memory_order_relaxed);
         try { 
            job(); 
         }
         catch (...) {
            // Swallow exceptions - never crash the worker thread
#ifdef _MSC_VER
            // OutputDebugStringA("[JobSystem] Unhandled job exception swallowed in worker.\n");
#endif
         }
         m_activeWorkers.fetch_sub(1, std::memory_order_release);
         {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_outstandingJobs > 0) {
               --m_outstandingJobs;
               if (m_outstandingJobs == 0) {
                  m_idleCv.notify_all();
               }
            }
         }
      }
   }

   void stop() {
      {
         std::lock_guard<std::mutex> lk(m_mutex);
         m_stopping = true;
      }
      m_cv.notify_all();
      m_idleCv.notify_all();
      for (auto& t : m_workers) {
         if (t.joinable()) t.join();
      }
      m_workers.clear();
   }

   std::vector<std::thread> m_workers;
   
   // Single mutex protects all queues - simpler and correct
   mutable std::mutex m_mutex;
   std::condition_variable m_cv;
   std::condition_variable m_idleCv;
   
   // Per-priority queues for scheduling
   static constexpr size_t kQueueCount = static_cast<size_t>(Priority::Count);
   std::deque<std::function<void()>> m_queues[kQueueCount];
   
   std::atomic<size_t> m_activeWorkers{0};
   size_t m_outstandingJobs{0};
   bool m_stopping{false};
};
