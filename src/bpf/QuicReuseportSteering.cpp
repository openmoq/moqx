/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Overrides the weak mvfst_hook_on_socket_create symbol to attach a classic
// BPF reuseport steering filter to each QUIC worker socket.
//
// For short-header (1-RTT) packets the filter extracts workerId from the
// mvfst default V1 connection ID encoding:
//   CID byte 2 (UDP offset 11): bits 7:2 → upper 6 bits of workerId
//   CID byte 3 (UDP offset 12): bits 7:6 → lower 2 bits of workerId
//   workerId = (CID[2] << 2) | (CID[3] >> 6)
// The kernel selects socket[workerId % num_sockets], matching mvfst's own
// routing for non-initial packets.
//
// For long-header packets (initial/handshake, client-chosen CIDs) the filter
// returns the UDP source port, distributing new connections across workers.

#include "bpf/QuicReuseportSteering.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

#include <linux/filter.h>
#include <sys/socket.h>

#include <folly/Indestructible.h>
#include <folly/SocketAddress.h>
#include <folly/ThreadLocal.h>
#include <folly/container/F14Set.h>
#include <folly/logging/xlog.h>
#include <folly/net/NetworkSocket.h>

namespace openmoq::moqx {

// SO_ATTACH_REUSEPORT_CBPF is a property of the kernel reuseport group, not the
// fd: re-attaching frees the group's single prog while sibling RX softirqs may
// still run it. The hook fires per worker listener fd at bind() and per accepted
// connection (site 2), so attach exactly once per group keyed by address:port.
class ReuseportSteering {
public:
  void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

  // The body of the mvfst_hook_on_socket_create weak-symbol override.
  void maybeAttach(int fd);

private:
  std::atomic<bool> enabled_{true};

  // Shared group set, guarded by mutex_. Touched only on the cold first-touch
  // slow path.
  std::mutex mutex_;
  folly::F14FastSet<folly::SocketAddress> attachedGroups_;

  // Lock-free, syscall-free fast path: fds this thread already resolved and
  // confirmed attached. folly::ThreadLocal (not the native keyword, which can't
  // be a non-static member) keeps the state instance-owned; safe because the
  // Indestructible enclosing object never destructs, so neither does this.
  folly::ThreadLocal<folly::F14FastSet<int>> tlSeenFds_;
};

// folly::Indestructible (constructs in place, never destructs) is required, not
// a plain static: a late hook call from an IO thread draining during shutdown
// (or terminateClientSession/site-2 work after stop(), see commit 8abbc20)
// could otherwise touch an already-destroyed instance.
static folly::Indestructible<ReuseportSteering> gSteering;

void quicReuseportSetEnabled(bool enabled) {
  gSteering->setEnabled(enabled);
}

} // namespace openmoq::moqx

// UDP header is 8 bytes. QUIC packet begins immediately after.
// Short-header layout (offsets relative to start of UDP header):
//   [8]  QUIC first byte — bit 7 = 0 for short header
//   [9]  CID byte 0
//   [10] CID byte 1
//   [11] CID byte 2  (upper 6 bits of workerId)
//   [12] CID byte 3  (lower 2 bits of workerId in bits 7:6)

// clang-format off
static const struct sock_filter kFilter[] = {
    // Load QUIC first byte.
    BPF_STMT(BPF_LD | BPF_B | BPF_ABS,        8),
    // Isolate the long-header bit (MSB).
    BPF_STMT(BPF_ALU | BPF_AND | BPF_K,    0x80),
    // A == 0 → short header: skip 2 to CID extraction.
    // A != 0 → long header: fall through to source-port hash.
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,    0x00, 2, 0),
    // Long-header path: spread by UDP source port.
    BPF_STMT(BPF_LD | BPF_H | BPF_ABS,        0),
    BPF_STMT(BPF_RET | BPF_A,                 0),
    // Short-header path: reconstruct workerId.
    BPF_STMT(BPF_LD  | BPF_B | BPF_ABS,      11),  // A = CID[2]
    BPF_STMT(BPF_ALU | BPF_LSH | BPF_K,       2),  // A <<= 2
    BPF_STMT(BPF_MISC | BPF_TAX,              0),   // X = A
    BPF_STMT(BPF_LD  | BPF_B | BPF_ABS,      12),  // A = CID[3]
    BPF_STMT(BPF_ALU | BPF_RSH | BPF_K,       6),  // A >>= 6
    BPF_STMT(BPF_ALU | BPF_OR  | BPF_X,       0),  // A |= X  → workerId
    BPF_STMT(BPF_RET | BPF_A,                 0),
};
// clang-format on

static const struct sock_fprog kProg = {
    .len = static_cast<unsigned short>(sizeof(kFilter) / sizeof(kFilter[0])),
    .filter = const_cast<struct sock_filter*>(kFilter),
};

void openmoq::moqx::ReuseportSteering::maybeAttach(int fd) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }

  // Lock-free fast path: this thread already resolved this fd and the group was
  // attached then. We never detach, so the skip is always correct.
  if (tlSeenFds_->count(fd)) {
    return;
  }

  // Slow path (first time this thread sees this fd). Recover the group key
  // outside the lock — getsockname reads only per-fd kernel state.
  folly::SocketAddress addr;
  try {
    addr.setFromLocalAddress(folly::NetworkSocket::fromFd(fd));
  } catch (const std::exception& ex) {
    // Transient / unbound fd: log and return WITHOUT caching, so a later hook
    // call retries.
    XLOG_EVERY_N(WARN, 1000) << "quic reuseport BPF steering: getsockname failed: " << ex.what();
    return;
  }

  {
    std::lock_guard<std::mutex> g(mutex_);
    if (!attachedGroups_.count(addr)) {
      // Hold the lock across check → setsockopt → insert so concurrent worker
      // threads binding the same group at startup serialize and exactly one
      // attaches.
      if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &kProg, sizeof(kProg)) != 0) {
        // Return WITHOUT caching: a later hook retries a rare transient
        // listener failure, and client sockets (where the option legitimately
        // fails) re-run the rate-limited slow path per connection.
        XLOG_EVERY_N(WARN, 1000) << "quic reuseport BPF steering: setsockopt failed: "
                                 << std::strerror(errno);
        return;
      }
      attachedGroups_.insert(addr);
      XLOG(INFO) << "quic reuseport BPF steering: attached for group " << addr.describe();
    }
  }

  // Cache only after the group is known-attached.
  tlSeenFds_->insert(fd);
}

extern "C" void mvfst_hook_on_socket_create(int fd) {
  openmoq::moqx::gSteering->maybeAttach(fd);
}
