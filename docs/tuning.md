# Host and Relay Tuning

A relay with many subscribers depends on settings outside its config: kernel
UDP buffers, process limits and the allocator. This page lists each one, the
value we recommend, and how each way of running the relay applies it.
[`scripts/host-check.sh`](/scripts/host-check.sh) reports all of them for a host
or a running relay.

## Where each setting comes from

| Setting | `moqx-run.sh` | Docker | Perf harness | PGO training |
|---|---|---|---|---|
| UDP sysctls | host's; warns if low | host's, set by `setup-host.sh` | perf VMs'; warns if low | build host's; warns if low |
| Open files | caller's limit | 1048576 (compose `ulimits`) | raised to 65536 | caller's limit |
| Core dumps | caller's limit | on (`setup-host.sh` + compose) | not set | not set |
| jemalloc | `-j` or `MOQX_JEMALLOC` | on (`MOQX_JEMALLOC=auto`) | on | on, required |
| `MALLOC_CONF` | not set | not set | not set | not set |
| mvfst socket buffer | `net.core.wmem_max` | `net.core.wmem_max` | `net.core.wmem_max` | `net.core.wmem_max` |
| IO threads | 4 | 4 | 4 | 4 |

The perf harness and PGO training both run the relay through `moqx-run.sh`.

## Kernel UDP settings

The recommended values live in
[`scripts/host/99-moqx-quic.conf`](/scripts/host/99-moqx-quic.conf):

| sysctl | Value | What it does |
|---|---|---|
| `net.core.rmem_max`, `net.core.wmem_max` | 16 MB | Largest socket buffer a process may set. The mvfst listener asks for `wmem_max`, so this is its buffer size. |
| `net.core.rmem_default`, `net.core.wmem_default` | 4 MB | Buffer size of sockets that don't set one. The relay's listener sockets set their own. |
| `net.core.netdev_max_backlog` | 65536 | Packets each CPU holds between the NIC driver and the network stack. Packets past it are dropped. |
| `net.core.optmem_max` | 1 MB | Per-socket memory for control messages and socket options. |
| `net.ipv4.udp_rmem_min` | 256 KB | Receive buffer each UDP socket keeps when UDP memory use is over `udp_mem`. Matters only under memory pressure. |

Two related settings are left at their defaults:

- `net.ipv4.udp_wmem_min` has no effect: UDP has no send-side memory accounting.
- `net.ipv4.udp_mem` caps the memory all UDP sockets on the host can hold. The
  kernel sets it at boot from RAM; `host-check.sh` shows it in MiB. Past it, each
  socket can queue only `udp_rmem_min` and further packets are dropped.

Install and apply:

```bash
sudo install -m 0644 scripts/host/99-moqx-quic.conf /etc/sysctl.d/
sudo sysctl -p /etc/sysctl.d/99-moqx-quic.conf
```

On Docker hosts, [`docker/setup-host.sh`](/docker/setup-host.sh) installs the same
file. Containers use the host's values, so set them on the host.

### Socket buffers the relay asks for

- **mvfst listener:** `udp_socket_buffer_bytes` in the listener's `mvfst` block.
  The config default is 0, which means moxygen's 1 MB. `moqx-run.sh` and the
  Docker entrypoint set it to `net.core.wmem_max`. The kernel silently caps a
  request at `wmem_max`/`rmem_max`; the relay then logs
  `UDP send buffer for fd N is X bytes (expected Y)`.
- **picoquic listener:** always 1 MB. The `mvfst` block, including
  `udp_socket_buffer_bytes`, does not apply to it.

`host-check.sh --pid <relay pid>` lists each UDP socket's buffer sizes and drop
count. Linux doubles a size the process sets, so a 16 MB request shows as
33554432.

### Drop counters

| Counter | Counts drops when |
|---|---|
| `d` in `ss -uampn` `skmem:(…)` | per socket, for any reason |
| `RcvbufErrors` in `/proc/net/snmp` | a socket's receive buffer was full |
| `MemErrors` in `/proc/net/snmp` | UDP memory use was over `udp_mem` |
| column 2 of `/proc/net/softnet_stat` | a CPU's backlog (`netdev_max_backlog`) was full |

Column 3 of `softnet_stat` (`time_squeeze`) counts how often the kernel ran out
of budget before draining a backlog. `host-check.sh` prints all of these. The
host counters count from boot, so compare them before and after a run.

## File descriptors and core dumps

QUIC connections share the listener's few UDP sockets, so the relay's
descriptor count grows slowly with subscribers. Set a high limit anyway:
upstream links, admin connections and logs use descriptors too.

| How the relay runs | Where the limit comes from |
|---|---|
| Shell or `moqx-run.sh` | `ulimit -n` (up to the hard limit), or `nofile` in `/etc/security/limits.conf` for login sessions |
| systemd | `LimitNOFILE=` in the unit |
| Docker compose | `ulimits.nofile`; the shipped compose sets 1048576 |
| Perf harness | `ulimit -n 65536` on each VM; logs a warning if the hard limit is lower |

To see what a running relay got: `grep 'open files' /proc/<pid>/limits`, or
`host-check.sh --pid`.

Core dumps need an unlimited core size (`ulimit -c unlimited`, or compose
`ulimits.core: -1`) and a `kernel.core_pattern` that writes a file.
`docker/setup-host.sh` points it at the relay container's `/var/coredumps`
volume and disables apport.

## Allocator

The relay runs about 10% faster with jemalloc. The binaries don't link an
allocator: jemalloc comes from the system `libjemalloc2` package via
`LD_PRELOAD`.

- `moqx-run.sh -j`, or `MOQX_JEMALLOC=auto`.
- Docker image: on by default; `MOQX_JEMALLOC=off` disables it.
- Perf harness and PGO training: always on. folly takes jemalloc-specific code
  paths when jemalloc is loaded, so the optimized `linux-amd64` build was
  trained with it. Run that build with jemalloc.

No launcher sets `MALLOC_CONF`, so jemalloc runs with its defaults. To check a
running relay: `grep libjemalloc /proc/<pid>/maps`, or `host-check.sh --pid`,
which also shows `MALLOC_CONF`.

## CPU and NIC

We have no measured recommendations here yet. `host-check.sh` records these so
two runs can be compared:

- CPU model and count, frequency governor (VMs usually have none), transparent
  huge pages, and whether irqbalance is running.
- NIC speed, MTU, and segmentation and receive offloads (needs `ethtool`).
  mvfst sends with GSO by default (`enable_gso`) and falls back to `sendmmsg`
  if the kernel lacks it. UDP GRO on receive is off by default
  (`num_gro_buffers: 1`).

Measure the network path (for example `iperf3 -u`) before treating a
throughput ceiling as the relay's.

## Relay config

The config defaults differ from what our launchers set. A config written by
hand gets the defaults; to match what we benchmark, start from
[`scripts/perf/config.bench.yaml`](/scripts/perf/config.bench.yaml) or
[`docker/config.docker.yaml`](/docker/config.docker.yaml).

| Setting | Config default | `moqx-run.sh` and Docker |
|---|---|---|
| `threads` | 1 | 4 |
| `use_relay_thread` | true | true |
| `use_local_forwarders` | false | true |
| `mvfst_bpf_steering` | true | false |
| `mvfst.max_conn_packets_sent_per_loop` | 48 | 16 |
| `mvfst.max_server_recv_packets_per_loop` | 64 | 256 |
| `mvfst.udp_socket_buffer_bytes` | 0 (1 MB) | `net.core.wmem_max` |
| `quic.cc_algo` | `bbr` | `moqx-run.sh`: `bbr`; Docker: `bbr2` on mvfst, `bbr` on picoquic |

The perf harness also passes `--no-cache --ignore-path-mtu` and uses `bbr2` on
mvfst. PGO training passes `--cache --ignore-path-mtu`. Field reference:
[config.md: Threading](/docs/config.md#threading) and
[config.md: mvfst Settings](/docs/config.md#mvfst-settings).

## Checking a host

```bash
scripts/host-check.sh                            # host report
scripts/host-check.sh --pid "$(pgrep -o -x moqx)"  # plus a running relay
scripts/host-check.sh --strict                   # exit 1 if a sysctl is low
```

- `moqx-run.sh` checks the sysctls before it starts the relay and warns if one
  is low. `moqx-run.sh --check-sysctl` prints the full report.
- For a container, run it on the host with sudo and the container's PID
  (`docker inspect -f '{{.State.Pid}}' moqx`). The socket list stays empty
  because the container has its own network namespace; use
  `sudo nsenter -t <pid> -n ss -uampn` for that.
- Every perf-test run uploads `run-*.host.txt` in its `perf-results` artifact:
  the report for both VMs, plus the relay process after the run. See
  [perf-tracking.md](/docs/perf-tracking.md).
