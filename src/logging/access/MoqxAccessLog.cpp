/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/MoqxAccessLog.h"

#include <algorithm>

#include <folly/coro/Collect.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Timeout.h>
#include <folly/hash/FnvHash.h>
#include <folly/hash/Hash.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

namespace {

constexpr size_t kEventCount = size_t(AccessLogEvent::LOG_DROPPED) + 1;
constexpr std::chrono::seconds kDestructorStopTimeout{2};
// The writer cancels its last writes at the deadline, so stop() waits a little longer.
constexpr std::chrono::milliseconds kStopGrace{250};

bool listed(const std::vector<std::string>& list, std::string_view value) {
  return list.empty() || std::find(list.begin(), list.end(), value) != list.end();
}

template <typename F> void forEachSink(AccessLogSinkMask mask, F&& fn) {
  while (mask) {
    auto i = size_t(__builtin_ctzll(mask));
    fn(i);
    mask &= mask - 1;
  }
}

} // namespace

double accessLogSampleKey(std::string_view key) {
  auto h = folly::hash::twang_mix64(folly::hash::fnv64_buf(key.data(), key.size()));
  return double(h >> 11) * 0x1.0p-53;
}

MoqxAccessLog::MoqxAccessLog(
    AccessLogRelayInfo relayInfo,
    std::vector<std::unique_ptr<AccessLogSink>> sinks,
    size_t queueLimit
)
    : relayInfo_(std::move(relayInfo)), sinks_(std::move(sinks)), eventSinks_(kEventCount, 0),
      lastFlush_(sinks_.size()), queueLimit_(queueLimit) {
  CHECK_LE(sinks_.size(), kMaxAccessLogSinks);
  if (!sinks_.empty()) {
    thread_.emplace("moqx-accesslog");
  }
  for (size_t i = 0; i < sinks_.size(); ++i) {
    const auto& opts = sinks_[i]->options();
    AccessLogSinkMask bit = AccessLogSinkMask(1) << i;
    for (size_t e = 0; e < kEventCount; ++e) {
      if (opts.filter.events[e]) {
        eventSinks_[e] |= bit;
      }
    }
    if (opts.filter.errorsOnly) {
      errorsOnlySinks_ |= bit;
    }
  }
}

MoqxAccessLog::~MoqxAccessLog() {
  stop(std::chrono::steady_clock::now() + kDestructorStopTimeout);
}

void MoqxAccessLog::start() {
  if (!enabled() || state_.load(std::memory_order_relaxed) != State::Created) {
    return;
  }
  std::fill(lastFlush_.begin(), lastFlush_.end(), std::chrono::steady_clock::now());
  done_ = folly::coro::co_withExecutor(thread_->getEventBase(), run()).start();
  state_.store(State::Running, std::memory_order_release);
}

void MoqxAccessLog::stop(std::chrono::steady_clock::time_point deadline) {
  if (state_.load(std::memory_order_relaxed) != State::Running) {
    return;
  }
  state_.store(State::Stopped, std::memory_order_release);
  stopDeadline_ = deadline;
  queue_.enqueue(Item{ItemKind::Stop, {}});
  auto remaining = deadline + kStopGrace - std::chrono::steady_clock::now();
  if (!std::move(done_).wait(std::chrono::ceil<std::chrono::milliseconds>(
          std::max(remaining, std::chrono::steady_clock::duration::zero())
      ))) {
    XLOG(WARN) << "access log writer did not finish before its shutdown deadline";
    return;
  }
  // Batches that arrived behind the Stop item.
  while (auto item = queue_.try_dequeue()) {
    depth_.fetch_sub(item->entries.size(), std::memory_order_relaxed);
    noteDropped(item->entries.size());
  }
}

AccessLogSessionGate MoqxAccessLog::gateSession(
    std::string_view service,
    std::string_view peerClass,
    std::string_view sampleKey
) const {
  AccessLogSessionGate gate;
  auto key = accessLogSampleKey(sampleKey);
  for (size_t i = 0; i < sinks_.size(); ++i) {
    const auto& filter = sinks_[i]->options().filter;
    if (!listed(filter.services, service) || !listed(filter.peerClasses, peerClass)) {
      continue;
    }
    AccessLogSinkMask bit = AccessLogSinkMask(1) << i;
    gate.matched |= bit;
    if (key < filter.sessionSample) {
      gate.sampled |= bit;
    }
  }
  return gate;
}

AccessLogSinkMask
MoqxAccessLog::eventMask(const AccessLogSessionGate& gate, AccessLogEvent event, bool isError)
    const {
  auto accepting = eventSinks_[size_t(event)];
  if (isError) {
    return gate.matched & accepting;
  }
  return gate.sampled & accepting & ~errorsOnlySinks_;
}

void MoqxAccessLog::submit(std::unique_ptr<AccessLogRecord> record, AccessLogSinkMask mask) {
  if (!mask || !record || !enabled()) {
    return;
  }
  if (state_.load(std::memory_order_acquire) == State::Stopped) {
    noteDropped();
    return;
  }
  auto* evb = folly::EventBaseManager::get()->getExistingEventBase();
  if (evb && evb != thread_->getEventBase()) {
    buffers_.try_emplace(*evb, *this).add(*evb, Entry{std::move(record), mask});
    return;
  }
  std::vector<Entry> entries;
  entries.push_back(Entry{std::move(record), mask});
  enqueueBatch(std::move(entries));
}

MoqxAccessLog::ThreadBuffer::~ThreadBuffer() {
  log_.enqueueBatch(std::move(entries_));
}

void MoqxAccessLog::ThreadBuffer::add(folly::EventBase& evb, Entry entry) {
  if (!isLoopCallbackScheduled()) {
    evb.runInLoop(this);
  }
  entries_.push_back(std::move(entry));
}

void MoqxAccessLog::ThreadBuffer::runLoopCallback() noexcept {
  log_.enqueueBatch(std::exchange(entries_, {}));
}

void MoqxAccessLog::enqueueBatch(std::vector<Entry> entries) {
  if (entries.empty()) {
    return;
  }
  auto n = entries.size();
  if (state_.load(std::memory_order_acquire) == State::Stopped) {
    noteDropped(n);
    return;
  }
  // Reserves the batch's room, so concurrent producers cannot both pass the limit.
  auto depth = depth_.load(std::memory_order_relaxed);
  do {
    if (depth >= queueLimit_) {
      noteDropped(n);
      return;
    }
  } while (!depth_.compare_exchange_weak(depth, depth + n, std::memory_order_relaxed));
  queue_.enqueue(Item{ItemKind::Records, std::move(entries)});
}

void MoqxAccessLog::noteDropped(uint64_t count) {
  queueDropped_.fetch_add(count, std::memory_order_relaxed);
}

void MoqxAccessLog::reopen() {
  if (!enabled()) {
    return;
  }
  queue_.enqueue(Item{ItemKind::Reopen, {}});
}

void MoqxAccessLog::handleRecords(const std::vector<Entry>& entries) {
  auto dropped = queueDropped();
  for (const auto& entry : entries) {
    DCHECK(sinks_.size() == kMaxAccessLogSinks || (entry.mask >> sinks_.size()) == 0);
    forEachSink(entry.mask, [&](size_t i) {
      sinks_[i]->append(*entry.record, relayInfo_, dropped);
    });
  }
}

std::optional<std::chrono::steady_clock::time_point> MoqxAccessLog::nextFlush() const {
  std::optional<std::chrono::steady_clock::time_point> next;
  auto now = std::chrono::steady_clock::now();
  for (size_t i = 0; i < sinks_.size(); ++i) {
    const auto& sink = *sinks_[i];
    if (!sink.hasPending()) {
      continue;
    }
    auto due = sink.batchFull() ? now : lastFlush_[i] + sink.options().flushInterval;
    due = std::max(due, sink.retryAt());
    next = next ? std::min(*next, due) : due;
  }
  return next;
}

folly::coro::Task<void> MoqxAccessLog::flushDue() {
  auto now = std::chrono::steady_clock::now();
  for (size_t i = 0; i < sinks_.size(); ++i) {
    auto& sink = *sinks_[i];
    if (!sink.hasPending()) {
      lastFlush_[i] = now;
      continue;
    }
    if (sink.batchFull() || now - lastFlush_[i] >= sink.options().flushInterval) {
      co_await sink.flush();
      lastFlush_[i] = now;
    }
  }
}

folly::coro::Task<void> MoqxAccessLog::visitStats(AccessLogStatsVisitor& visitor) {
  if (!enabled()) {
    co_return;
  }
  co_await folly::coro::co_withExecutor(
      thread_->getEventBase(),
      folly::coro::co_invoke([this, &visitor]() -> folly::coro::Task<void> {
        reportStats(visitor);
        co_return;
      })
  );
}

void MoqxAccessLog::reportStats(AccessLogStatsVisitor& visitor) {
  for (const auto& sink : sinks_) {
    visitor.onSink(sink->options().name, sink->stats());
  }
  double age = 0;
  if (const auto* head = queue_.try_peek(); head && !head->entries.empty()) {
    age = std::chrono::duration<double>(
              std::chrono::system_clock::now() - head->entries.front().record->time
    )
              .count();
  }
  visitor.onQueue(queueDepth(), queueDropped(), std::max(age, 0.0));
}

folly::coro::Task<void> MoqxAccessLog::untilStopDeadline(folly::coro::Task<void> task) {
  auto remaining = std::max(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          stopDeadline_ - std::chrono::steady_clock::now()
      ),
      std::chrono::milliseconds(0)
  );
  // On timeout this cancels the task and waits for it, so the batches that it writes
  // stay valid.
  co_await folly::coro::co_awaitTry(folly::coro::timeoutNoDiscard(std::move(task), remaining));
}

folly::coro::Task<void> MoqxAccessLog::closeSinks() {
  std::vector<folly::coro::Task<void>> closes;
  for (auto& sink : sinks_) {
    closes.push_back(sink->close(relayInfo_, queueDropped()));
  }
  co_await untilStopDeadline(folly::coro::collectAllRange(std::move(closes)));
}

folly::coro::Task<void> MoqxAccessLog::run() {
  while (true) {
    folly::Try<Item> item;
    if (auto due = nextFlush()) {
      auto wait = std::max(
          std::chrono::ceil<std::chrono::milliseconds>(*due - std::chrono::steady_clock::now()),
          std::chrono::milliseconds(0)
      );
      item = co_await folly::coro::co_awaitTry(queue_.co_try_dequeue_for(wait));
    } else {
      item = co_await folly::coro::co_awaitTry(queue_.dequeue());
    }
    if (item.hasValue()) {
      switch (item->kind) {
      case ItemKind::Records:
        depth_.fetch_sub(item->entries.size(), std::memory_order_relaxed);
        handleRecords(item->entries);
        break;
      case ItemKind::Reopen:
        for (auto& sink : sinks_) {
          sink->reopen();
        }
        break;
      case ItemKind::Stop:
        while (auto more = queue_.try_dequeue()) {
          if (more->kind != ItemKind::Records) {
            continue;
          }
          depth_.fetch_sub(more->entries.size(), std::memory_order_relaxed);
          if (std::chrono::steady_clock::now() < stopDeadline_) {
            handleRecords(more->entries);
          } else {
            noteDropped(more->entries.size());
          }
          if (std::any_of(sinks_.begin(), sinks_.end(), [](const auto& s) {
                return s->batchFull();
              })) {
            co_await untilStopDeadline(flushDue());
          }
        }
        co_await closeSinks();
        co_return;
      }
    }
    co_await flushDue();
  }
}

} // namespace openmoq::moqx::logging
