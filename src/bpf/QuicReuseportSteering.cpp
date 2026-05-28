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

#include <linux/filter.h>
#include <sys/socket.h>

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

static std::atomic<bool> gEnabled{true};

void quicReuseportSetEnabled(bool enabled) {
  gEnabled.store(enabled, std::memory_order_relaxed);
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

extern "C" void mvfst_hook_on_socket_create(int fd) {
  if (!openmoq::moqx::gEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &kProg, sizeof(kProg)) != 0) {
    XLOG(WARN) << "quic reuseport BPF steering: setsockopt failed: " << std::strerror(errno);
  }
}
