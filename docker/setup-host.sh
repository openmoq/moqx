#!/usr/bin/env bash
# One-time host provisioning for a moqx relay host.
#
# Sets up the HOST-level prerequisites the containerized relay + stats stack rely
# on: QUIC/UDP kernel tuning, relay core dumps, and firewall rules. This is
# deliberately separate from relay-deploy.sh (the per-deploy app bring-up) — host config shouldn't be
# re-applied on every redeploy. Run it ONCE per host (re-running is safe/idempotent);
# relay-deploy.sh only *verifies* these are present and warns if not.
#
# Assumes ufw is already active with SSH (22) allowed — this script only ADDS
# rules, it never enables ufw or changes its default policy (no lockout risk).
#
#   usage:  sudo bash docker/setup-host.sh
set -euo pipefail

echo "==> moqx host provisioning"

# ── QUIC/UDP kernel tuning ────────────────────────────────────────────────────
# The relay sizes its UDP socket buffer to net.core.wmem_max (see entrypoint.sh),
# and the stock 208 KB default throttles a high-fanout QUIC relay. net.core.* are
# host-global (not namespaced), so they must be set on the host; the container
# reads the host value. Persisted so a reboot keeps them.
tee /etc/sysctl.d/99-moqx-quic.conf >/dev/null <<'SYSCTL'
# moqx QUIC/UDP relay tuning — managed by docker/setup-host.sh
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576
net.core.netdev_max_backlog = 10000
net.core.optmem_max = 65536
SYSCTL
# Apply only our drop-in (not --system, which reloads every drop-in and would
# surface unrelated errors from other files on the host).
sysctl -q -p /etc/sysctl.d/99-moqx-quic.conf
echo "    kernel tuning applied (rmem_max/wmem_max=16MiB, backlog=10000, optmem=65536)"

# ── relay core dumps ──────────────────────────────────────────────────────────
# A file core_pattern resolves in the crashing process's mount namespace: relay
# cores land in its /var/coredumps volume for crash/crash-watch.py. Host
# processes have no /var/coredumps (don't create one) and dump nothing. apport
# rewrites the pattern at boot and drops container crashes, so it is disabled.
# apport is a sysv service here: `disable --now` would not stop it.
systemctl stop apport.service >/dev/null 2>&1 || true
systemctl disable apport.service >/dev/null 2>&1 || true
tee /etc/sysctl.d/99-moqx-core.conf >/dev/null <<'SYSCTL'
# moqx relay core dumps — managed by docker/setup-host.sh
kernel.core_pattern = /var/coredumps/core.%e.%p.%t
SYSCTL
sysctl -q -p /etc/sysctl.d/99-moqx-core.conf
echo "    relay core dumps enabled (core_pattern -> the relay's /var/coredumps volume)"

# ── firewall ──────────────────────────────────────────────────────────────────
# Relay MoQ (mvfst) + pico listeners. The stats surface stays on localhost.
ufw allow 4433:4434/udp >/dev/null
# Remove the wider 4433:4533 rules if present.
ufw delete allow 4433:4533/udp >/dev/null 2>&1 || true
ufw delete allow 4433:4533/tcp >/dev/null 2>&1 || true
# node-exporter runs in the host network namespace (to read the real NIC), so the
# bridged Prometheus reaches it via the host gateway — that hop traverses the host
# firewall. Allow only the private docker subnets (external stays denied by ufw's
# default policy). The relay is bridge-networked, so its admin needs no such rule.
ufw allow from 172.16.0.0/12 to any port 9100 proto tcp >/dev/null   # node-exporter
echo "    firewall rules applied (4433:4434/udp public; docker-subnet -> :8000/:9100)"

echo "==> Done. relay-deploy.sh can now bring up the stack."
