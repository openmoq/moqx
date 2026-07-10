#!/usr/bin/env bash
# Shared relay deploy core — used by both ci-main (auto-deploy on main) and
# deploy-relay (manual workflow_dispatch), so the two never drift.
#
# Writes docker/.env, (re)creates the relay — plus the stats stack + public
# read-only dashboard when ENABLE_STATS=true — runs a health check, and
# publishes the public dashboard. Cert/DNS/GHCR-login and any restart-only or
# image-tag logic stay in the calling workflow.
#
# Inputs (env vars):
#   DOMAIN            (required) relay hostname / cert name
#   RELAY_PORT        (default 4433)
#   ADMIN_PORT        (default 8000)
#   MOQX_LOGGING      (optional) folly XLOG config; empty = baseline INFO
#   PULL_IMAGE        (optional) full image ref to `docker pull` + retag :latest.
#                     Empty → `docker compose pull` (compose's pinned :latest).
#   ENABLE_STATS      "true" → stats stack + public dashboard
#   STATS_USER, STATS_PASSWORD, GRAFANA_ADMIN_PASSWORD   (required when stats on)
#   STATS_PUBLIC_PORT (default 4533)
set -euo pipefail
cd "$(dirname "$0")"        # docker/

RELAY_PORT="${RELAY_PORT:-4433}"
ADMIN_PORT="${ADMIN_PORT:-8000}"
PUB_PORT="${STATS_PUBLIC_PORT:-4533}"

# ── docker/.env ──────────────────────────────────────────────────────────────
{
  echo "DOMAIN=${DOMAIN}"
  echo "CERTBOT_EMAIL=gmarzot@openmoq.org"
  echo "MOQX_PORT=${RELAY_PORT}"
  echo "MOQX_ADMIN_PORT=${ADMIN_PORT}"
  echo "MOQX_LOGGING=${MOQX_LOGGING:-}"
  echo "MOQX_CPUS=$(nproc)"
  echo "MOQX_THREADS=$(nproc)"
} > .env

PROFILE_ARGS=""
if [ "${ENABLE_STATS:-}" = "true" ]; then
  PROFILE_ARGS="--profile stats"
  {
    echo "STATS_USER=${STATS_USER}"
    echo "STATS_PASSWORD=${STATS_PASSWORD}"
    echo "GRAFANA_ADMIN_PASSWORD=${GRAFANA_ADMIN_PASSWORD}"
    # CI runner has the disk; let the 365d time limit bound history at 3s/5s.
    echo "PROMETHEUS_RETENTION_SIZE=20GB"
    # Stats implies the public read-only dashboard: bind its port public.
    echo "STATS_PUBLIC_BIND=0.0.0.0"
  } >> .env
fi

# ── host network tuning for the QUIC/UDP relay ───────────────────────────────
# The relay sizes its UDP socket buffer to net.core.wmem_max (entrypoint.sh),
# and the stock 208 KB default throttles a high-fanout QUIC relay. Raise UDP
# buffers + device backlog to values suited to a multi-gigabit relay. net.core.*
# are host-global (not namespaced), so this must be set on the host — the relay
# container reads the host value. Persisted so a reboot keeps it; the relay picks
# up the larger buffer on its next (re)start.
sudo tee /etc/sysctl.d/99-moqx-quic.conf >/dev/null <<'SYSCTL' || true
# moqx QUIC/UDP relay tuning — see docker/relay-deploy.sh
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576
net.core.netdev_max_backlog = 10000
net.core.optmem_max = 65536
SYSCTL
sudo sysctl -q --system >/dev/null 2>&1 || true

# ── pull + (re)create ────────────────────────────────────────────────────────
if [ -n "${PULL_IMAGE:-}" ]; then
  docker pull "$PULL_IMAGE"
  # compose pins :latest — make the pulled tag the local :latest.
  [ "${PULL_IMAGE##*:}" != "latest" ] && docker tag "$PULL_IMAGE" "${PULL_IMAGE%%:*}:latest"
else
  docker compose pull
fi

echo "==> (Re)creating relay${PROFILE_ARGS:+ + stats stack} on ${DOMAIN}:${RELAY_PORT}..."
docker compose ${PROFILE_ARGS} down --remove-orphans 2>/dev/null || true
docker rm -f moqx logmon 2>/dev/null || true
docker compose ${PROFILE_ARGS} up -d

# ── health check ─────────────────────────────────────────────────────────────
for i in $(seq 1 30); do
  curl -sf "http://127.0.0.1:${ADMIN_PORT}/info" >/dev/null 2>&1 && break
  sleep 1
  if [ "$i" -eq 30 ]; then
    echo "::error::Admin endpoint did not respond within 30s"
    docker compose logs moqx
    exit 1
  fi
done
echo "==> Relay running: $(curl -sf http://127.0.0.1:${ADMIN_PORT}/info)"

# ── public read-only dashboard (stats only) ──────────────────────────────────
if [ "${ENABLE_STATS:-}" = "true" ]; then
  echo "==> Publishing public read-only dashboard on :${PUB_PORT}..."
  # PUB_PORT (default 4533) sits in the already-open 4433-4533 range (host ufw +
  # Linode cloud firewall), so no per-port ufw rule is needed — deliberately kept
  # in-range to avoid a firewall change. If you move it out of range, open it.
  # node-exporter runs in the host netns (to see the real NIC) and Prometheus
  # reaches it via the host gateway, so that hop now traverses the host firewall.
  # Allow only the private docker subnets to host :9100 (external stays denied).
  sudo ufw allow from 172.16.0.0/12 to any port 9100 proto tcp >/dev/null 2>&1 || true
  # The relay also runs in the host netns now, so bridged prometheus/json-exporter/
  # nginx reach its admin :8000 via the host gateway — allow only docker subnets.
  sudo ufw allow from 172.16.0.0/12 to any port 8000 proto tcp >/dev/null 2>&1 || true
  for i in $(seq 1 30); do
    docker exec moqx-grafana curl -sf -o /dev/null http://localhost:3000/api/health 2>/dev/null && break
    sleep 1
  done
  ./grafana/publish-public-dashboard.sh publish || echo "::warning::public dashboard publish failed"
fi
