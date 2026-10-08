/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <forward_list>

#include <disruptorplus/spin_wait_strategy.hpp>
#include <disruptorplus/blocking_wait_strategy.hpp>
#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

namespace {
// The timer queue starts before runtime configuration is read. Its selector
// must be atomic while the config chooses the wait strategy at startup.
std::atomic<bool> timer_queue_sleep{false};
auto timer_queue_sleep_flag = rex::cvar::FlagRegistrar(
    {"timer_queue_sleep",
     rex::cvar::FlagType::Boolean,
     "Kernel",
     "Sleep until a timer deadline or a new timer instead of spinning",
     [](std::string_view value) {
       timer_queue_sleep.store(value == "true" || value == "1" || value == "yes",
                               std::memory_order_relaxed);
       return true;
     },
     []() { return timer_queue_sleep.load(std::memory_order_relaxed) ? "true" : "false"; },
     []() {},
     rex::cvar::Lifecycle::kInitOnly,
     {},
     "false",
     false});
}  // namespace

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

class TimerWaitStrategy : public dp::blocking_wait_strategy {
 public:
  using dp::blocking_wait_strategy::wait_until_published;

  template <typename Clock, typename Duration>
  dp::sequence_t wait_until_published(dp::sequence_t sequence, size_t count,
                                      const std::atomic<dp::sequence_t>* const sequences[],
                                      const std::chrono::time_point<Clock, Duration>& deadline) {
    if (timer_queue_sleep.load(std::memory_order_relaxed)) {
      return dp::blocking_wait_strategy::wait_until_published(sequence, count, sequences, deadline);
    }
    return spin_.wait_until_published(sequence, count, sequences, deadline);
  }

 private:
  dp::spin_wait_strategy spin_;
};

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    while (!stop_token.stop_requested()) {
      {
        // Consume new wait items and add them to sorted wait queue (a capped
        // wait while there is no timer, as some standard libraries mishandle
        // time_point::max()).
        dp::sequence_t available = claim_strategy_.wait_until_published(
            next_sequence, next_sequence - 1,
            wait_queue_.empty() ? clock::now() + std::chrono::seconds(1)
                                : wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            if (stats_enabled_) {
              RecordCallback(wait_item.get());
            }
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

  // Debugging (REX_TIMER_STATS): callbacks per second, split into the 1 ms
  // timestamp timer and everything else, and how late they run.
  bool stats_enabled_ = std::getenv("REX_TIMER_STATS") != nullptr;
  uint64_t stats_count_1ms_ = 0, stats_count_other_ = 0;
  double stats_late_sum_us_ = 0.0, stats_late_max_us_ = 0.0;
  clock::time_point stats_start_ = clock::now();
  void RecordCallback(WaitItem* item) {
    auto now = clock::now();
    double late_us = std::chrono::duration<double, std::micro>(now - item->due_).count();
    stats_late_sum_us_ += late_us;
    stats_late_max_us_ = std::max(stats_late_max_us_, late_us);
    if (item->interval_ == std::chrono::milliseconds(1)) {
      ++stats_count_1ms_;
    } else {
      ++stats_count_other_;
    }
    double elapsed = std::chrono::duration<double>(now - stats_start_).count();
    if (elapsed >= 5.0) {
      uint64_t total = stats_count_1ms_ + stats_count_other_;
      std::fprintf(stderr, "[timer-stats] %.0f/s 1ms timer, %.1f/s other, late avg %.0f us max %.0f us\n",
                   stats_count_1ms_ / elapsed, stats_count_other_ / elapsed,
                   total ? stats_late_sum_us_ / total : 0.0, stats_late_max_us_);
      stats_count_1ms_ = stats_count_other_ = 0;
      stats_late_sum_us_ = stats_late_max_us_ = 0.0;
      stats_start_ = now;
    }
  }

 private:
  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  // The blocking path releases the CPU between deadlines. Keep the spin
  // strategy selectable for comparisons and platforms not yet qualified.
  TimerWaitStrategy wait_strategy_;
  dp::multi_threaded_claim_strategy<TimerWaitStrategy> claim_strategy_;
  dp::sequence_barrier<TimerWaitStrategy> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
