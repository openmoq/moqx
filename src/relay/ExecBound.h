/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/ExecutorCheck.h"

#include <folly/Executor.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>

#include <utility>

namespace openmoq::moqx {

// Owns a shared_ptr whose pointee must be released on exec, such as a handle that keeps a session
// alive. Releasing it anywhere else posts the release to exec, which must still be running.
template <typename T> class ExecBound {
public:
  ExecBound() = default;
  ExecBound(T value, folly::Executor* exec) : value_(std::move(value)), exec_(exec) {
    XCHECK(!value_ || exec_);
  }

  ExecBound(ExecBound&& other) noexcept
      : value_(std::move(other.value_)), exec_(std::exchange(other.exec_, nullptr)) {}

  ExecBound& operator=(ExecBound&& other) noexcept {
    if (this != &other) {
      release();
      value_ = std::move(other.value_);
      exec_ = std::exchange(other.exec_, nullptr);
    }
    return *this;
  }

  ExecBound(const ExecBound&) = delete;
  ExecBound& operator=(const ExecBound&) = delete;

  ~ExecBound() { release(); }

  // Runs fn(value) on exec: inline when already there, otherwise posted. fn gets its own copy, so
  // it may release this ExecBound.
  template <typename Fn> void post(Fn fn) const {
    if (!value_) {
      return;
    }
    if (isOnExecutor(exec_)) {
      T value = value_;
      fn(std::as_const(value));
      return;
    }
    add(std::move(fn));
  }

  // Queues fn(value) on exec, even from exec, so fn does not run inside the caller.
  template <typename Fn> void add(Fn fn) const {
    if (value_) {
      exec_->add([value = value_, fn = std::move(fn)]() mutable { fn(std::as_const(value)); });
    }
  }

  // Returns a task that runs fn(value), which returns a Task, on exec. The task's frame can be
  // destroyed on the awaiting thread, so its copy is an ExecBound too.
  template <typename Fn> auto run(Fn fn) const {
    XCHECK(value_);
    return folly::coro::co_withExecutor(
        folly::getKeepAliveToken(exec_),
        folly::coro::co_invoke([bound = ExecBound(value_, exec_), fn = std::move(fn)]() mutable {
          return fn(std::as_const(bound.value_));
        })
    );
  }

  // Starts run(fn) without waiting for it.
  template <typename Fn> void launch(Fn fn) const {
    if (value_) {
      run(std::move(fn)).start();
    }
  }

  explicit operator bool() const { return static_cast<bool>(value_); }

  void reset() { release(); }

private:
  void release() {
    if (!value_) {
      return;
    }
    if (isOnExecutor(exec_)) {
      value_.reset();
    } else {
      exec_->add([value = std::move(value_)]() mutable {});
      value_ = T{};
    }
  }

  T value_;
  folly::Executor* exec_{nullptr};
};

} // namespace openmoq::moqx
