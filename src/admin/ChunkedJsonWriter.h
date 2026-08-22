/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Function.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

#include <algorithm>
#include <memory>

#include "admin/JsonWriter.h"

namespace openmoq::moqx::admin {

// A JsonWriter that accumulates into its own queue and hands off a chunk once
// that queue crosses a threshold, so a response goes out as it is produced.
//
// Handing off never blocks, which is what lets the relay-state walk -- a
// synchronous FunctionRef traversal over maps that must not mutate underneath
// it -- run start to finish without ever yielding its executor.
//
// Comma and nesting state live in the JsonWriter rather than the queue, so a
// flush is invisible in the output: chunk boundaries can fall anywhere.
class ChunkedJsonWriter {
public:
  // Returns false once the consumer is gone or has given up, after which the
  // caller should stop walking.
  using Sink = folly::Function<bool(std::unique_ptr<folly::IOBuf>)>;

  static constexpr size_t kDefaultThreshold = 16 * 1024;

  explicit ChunkedJsonWriter(Sink sink, size_t threshold = kDefaultThreshold)
      : sink_(std::move(sink)), threshold_(threshold) {}

  JsonWriter& json() { return w_; }

  // For bytes outside the JSON value, i.e. the trailing newline; bypasses the
  // writer's comma state.
  void raw(std::string_view s) { app_.push(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

  bool alive() const { return alive_; }

  // Call at item boundaries during a walk.
  bool maybeFlush() {
    if (queue_.chainLength() >= threshold_) {
      return flush();
    }
    return alive_;
  }

  bool flush() {
    if (queue_.empty()) {
      return alive_;
    }
    auto chunk = queue_.move();
    app_.reset(&queue_, growthFor(threshold_));
    // A walk that cannot be stopped mid-section keeps writing after the sink is
    // gone; drop those bytes rather than accumulate them to no end.
    if (alive_) {
      alive_ = sink_(std::move(chunk));
    }
    return alive_;
  }

private:
  // Roughly one allocation per chunk: the queue is emptied every time it
  // crosses the threshold, so growing in smaller steps only buys proxygen a
  // longer buffer chain to write.
  static size_t growthFor(size_t threshold) {
    return std::clamp(threshold, size_t{1024}, size_t{64 * 1024});
  }

  Sink sink_;
  size_t threshold_;
  folly::IOBufQueue queue_{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender app_{&queue_, growthFor(threshold_)};
  JsonWriter w_{app_};
  bool alive_{true};
};

} // namespace openmoq::moqx::admin
