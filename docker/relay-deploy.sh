#!/usr/bin/env bash
# Shared relay deploy core — used by both ci-main (auto-deploy on main) and
# deploy-relay (manual workflow_dispatch), so the two never drift.
#
# Writes docker/.env, (re)creates the relay — plus the stats stack when
# ENABLE_STATS=true — and runs a health check. Cert/DNS/GHCR-login and any
# restart-only or image-tag logic stay in the calling workflow.
#
# Inputs (env vars):
#   DOMAIN            (required) relay hostname / cert name
#   RELAY_PORT        (default 4433)
#   ADMIN_PORT        (default 8000)
#   MOQX_LOGGING      (optional) folly XLOG config; empty = baseline INFO
#   MOQX_CC, MOQX_PICO_CC
#                     (optional) congestion control overrides, see entrypoint.sh;
#                     empty = entrypoint default
#   MOQX_BBR_SKIP_PROBE_RTT (optional) mvfst bbr: skip PROBE_RTT while app-limited;
#                     empty = true
#   MOQX_QLOG_SAMPLE  (optional) fraction of new mvfst connections to qlog;
#                     empty = off
#   PULL_IMAGE        (optional) full image ref to `docker pull` + retag :latest.
#                     Empty → `docker compose pull` (compose's pinned :latest).
#   ENABLE_STATS      "true" → stats stack
#   STATS_USER, STATS_PASSWORD, GRAFANA_ADMIN_PASSWORD   (required when stats on)
#   CRASH_WATCH       "true" → install/refresh the host crash watcher (crash/)
#   CRASH_GH_TOKEN    (optional) token the watcher files GitHub issues with;
#                     empty → crash reports stay on the host
#   CRASH_SLACK_WEBHOOK_URL (optional) Slack webhook for new crash issues
set -euo pipefail
cd "$(dirname "$0")"        # docker/

RELAY_PORT="${RELAY_PORT:-4433}"
ADMIN_PORT="${ADMIN_PORT:-8000}"

# ── docker/.env ──────────────────────────────────────────────────────────────
{
  echo "DOMAIN=${DOMAIN}"
  echo "CERTBOT_EMAIL=gmarzot@openmoq.org"
  echo "MOQX_PORT=${RELAY_PORT}"
  echo "MOQX_ADMIN_PORT=${ADMIN_PORT}"
  echo "MOQX_LOGGING=${MOQX_LOGGING:-}"
  echo "MOQX_CC=${MOQX_CC:-}"
  echo "MOQX_PICO_CC=${MOQX_PICO_CC:-}"
  echo "MOQX_BBR_SKIP_PROBE_RTT=${MOQX_BBR_SKIP_PROBE_RTT:-true}"
  echo "MOQX_QLOG_SAMPLE=${MOQX_QLOG_SAMPLE:-}"
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
    # Demo/debug box: fresh namespace discovery (compose default is 10s).
    echo "MOQX_TARGETS_INTERVAL=${MOQX_TARGETS_INTERVAL:-5}"
  } >> .env
fi

# Host provisioning (kernel tuning + firewall) is done once by setup-host.sh, not
# on every deploy. Warn (don't fail) if it looks unapplied so a fresh host stands out.
if [ "$(cat /proc/sys/net/core/wmem_max 2>/dev/null || echo 0)" -lt 16777216 ]; then
  echo "::warning::Host looks unprovisioned (net.core.wmem_max < 16 MiB). Run: sudo bash docker/setup-host.sh"
fi

# ── crash watcher ────────────────────────────────────────────────────────────
# A host service, not a compose service: it keeps running through the compose
# down below, so a crash during shutdown is caught too.
install_if_changed() {  # src dst mode; succeeds only if dst was replaced
  if sudo cmp -s "$1" "$2"; then return 1; fi
  sudo install -D -m "$3" "$1" "$2" || exit 1
}
if [ "${CRASH_WATCH:-}" = "true" ]; then
  if [ "$(cat /proc/sys/kernel/core_pattern)" != "/var/coredumps/core.%e.%p.%t" ]; then
    echo "::warning::Relay core dumps are off (kernel.core_pattern). Run: sudo bash docker/setup-host.sh"
  fi
  env_file=$(mktemp)
  printf 'GH_REPO=%s\nGH_TOKEN=%s\nCRASH_HOST=%s\nSLACK_WEBHOOK_URL=%s\n' "${GITHUB_REPOSITORY:-openmoq/moqx}" "${CRASH_GH_TOKEN:-}" "$DOMAIN" "${CRASH_SLACK_WEBHOOK_URL:-}" > "$env_file"
  changed=false
  install_if_changed crash/crash-watch.py /usr/local/libexec/moqx-crash/crash-watch.py 0755 && changed=true
  install_if_changed crash/moqx-crash-watch.service /etc/systemd/system/moqx-crash-watch.service 0644 && changed=true
  install_if_changed "$env_file" /etc/moqx-crash/env 0600 && changed=true
  rm -f "$env_file"
  if $changed || ! systemctl is-active --quiet moqx-crash-watch; then
    sudo systemctl daemon-reload
    sudo systemctl enable --quiet moqx-crash-watch
    sudo systemctl restart moqx-crash-watch
  fi
  echo "==> Crash watcher: $(systemctl is-active moqx-crash-watch)"
fi

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

# ── override check ───────────────────────────────────────────────────────────
# Compare the relay's live config with the requested settings; fail on mismatch.
curl -sf "http://127.0.0.1:${ADMIN_PORT}/config" | python3 -c '
import json, os, sys
cfg = json.load(sys.stdin)
want = {
    "mvfst": (os.environ.get("MOQX_CC") or "bbr2",
              (os.environ.get("MOQX_BBR_SKIP_PROBE_RTT") or "true") == "true"),
    "picoquic": (os.environ.get("MOQX_PICO_CC") or "bbr", None),
}
ok = True
for l in cfg["listeners"]:
    if l["quic_stack"] not in want:
        continue
    name = l["name"]
    cc, skip = want[l["quic_stack"]]
    got_cc = l["quic"]["cc_algo"]
    got_skip = l["mvfst"]["bbr"]["probe_rtt_disabled_if_app_limited"]
    extra = "" if skip is None else f" probe_rtt_disabled_if_app_limited={str(got_skip).lower()}"
    print(f"==> {name}: cc_algo={got_cc}{extra}")
    if got_cc != cc or (skip is not None and got_skip != skip):
        print(f"::error::{name} is not running the requested congestion control")
        ok = False
want_qlog = float(os.environ.get("MOQX_QLOG_SAMPLE") or 0)
qlog = (cfg.get("logging") or {}).get("qlog") or {}
got_qlog = qlog.get("sample_rate", 0.0)
qdir = qlog.get("dir", "-")
print(f"==> qlog: sample_rate={got_qlog:g} dir={qdir}")
if abs(got_qlog - want_qlog) > 1e-6:
    print("::error::relay is not running the requested qlog sample rate")
    ok = False
sys.exit(0 if ok else 1)
'
