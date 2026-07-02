#!/bin/sh
# Generate /etc/nginx/.htpasswd for the stats proxy's basic auth.
#
# Runs from the nginx image's /docker-entrypoint.d/ before nginx starts.
# Precedence:
#   STATS_HTPASSWD              — a verbatim "user:hash" line (pre-hashed)
#   STATS_USER + STATS_PASSWORD — hashed here with openssl apr1
#
# openssl is present in nginx:stable (Debian). Fail fast if no credentials are
# provided, since the whole point of the stats profile is gated access.
set -eu

htpasswd_file=/etc/nginx/.htpasswd

if [ -n "${STATS_HTPASSWD:-}" ]; then
    printf '%s\n' "$STATS_HTPASSWD" > "$htpasswd_file"
elif [ -n "${STATS_USER:-}" ] && [ -n "${STATS_PASSWORD:-}" ]; then
    hash=$(openssl passwd -apr1 "$STATS_PASSWORD")
    printf '%s:%s\n' "$STATS_USER" "$hash" > "$htpasswd_file"
else
    echo "40-htpasswd.sh: set STATS_USER+STATS_PASSWORD (or STATS_HTPASSWD) for the stats proxy" >&2
    exit 1
fi

# nginx workers run as the unprivileged 'nginx' user; make the file readable to
# them (owned by nginx, 0640) or fall back to world-readable if chown is denied.
if chown nginx:nginx "$htpasswd_file" 2>/dev/null; then
    chmod 640 "$htpasswd_file"
else
    chmod 644 "$htpasswd_file"
fi
echo "40-htpasswd.sh: wrote $htpasswd_file (user: ${STATS_USER:-<from STATS_HTPASSWD>})"
