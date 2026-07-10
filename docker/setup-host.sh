#!/usr/bin/env bash
# One-time host provisioning for a moqx relay host.
#
# Sets up the HOST-level prerequisites the containerized relay + stats stack rely
# on: QUIC/UDP kernel tuning and firewall rules. This is deliberately separate
# from relay-deploy.sh (the per-deploy app bring-up) — host config shouldn't be
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

# ── firewall ──────────────────────────────────────────────────────────────────
# Relay MoQ + pico + the public dashboard all live in the 4433-4533 range (kept
# in-range on purpose so the Linode cloud firewall needs no change).
ufw allow 4433:4533/udp >/dev/null
ufw allow 4433:4533/tcp >/dev/null
# The relay and node-exporter run in the host network namespace, so the bridged
# stats containers reach their admin/metrics ports via the host gateway — that
# hop traverses the host firewall. Allow only the private docker subnets
# (external access stays denied by ufw's default policy).
ufw allow from 172.16.0.0/12 to any port 8000 proto tcp >/dev/null   # relay admin
ufw allow from 172.16.0.0/12 to any port 9100 proto tcp >/dev/null   # node-exporter
echo "    firewall rules applied (4433:4533 public; docker-subnet -> :8000/:9100)"

echo "==> Done. relay-deploy.sh can now bring up the stack."
