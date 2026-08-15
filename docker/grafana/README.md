# moqx Grafana dashboards

Dashboards are **provisioned and read-only** — they come up as the landing page
and can't be saved-over in the UI (`provider.yml` sets `allowUiUpdates: false`).
This keeps the deployed dashboards in version control. To change one, edit an
**editable copy** and export it back:

```
                 import-dashboards.sh              export-dashboards.sh
  provisioning/  ───────────────────►  "…(dev)"   ───────────────────►  provisioning/
  dashboards/*.json  (read-only)        copies       (strip -dev, archive)  dashboards/*.json
                                      (editable)
```

## Edit workflow (run on the box — needs `docker` + `python3`)

```bash
cd <checkout>/docker/grafana

./import-dashboards.sh          # creates "<name> (dev)" editable copies in a
                                # "moqx (dev)" folder — the read-only originals stay put

# ... edit the (dev) copies in Grafana and Save ...

./export-dashboards.sh --dry-run   # preview the JSON diff vs disk
./export-dashboards.sh             # write edits back to provisioning/dashboards/,
                                   # archiving the prior version under archive/
```

Commit the changed `provisioning/dashboards/*.json`. On the next clean deploy the
provisioner reloads them (read-only again), so the deployed dashboard == the file.

- `GRAFANA_CONTAINER` overrides the container name (default `moqx-grafana`).
- The scripts reach Grafana via `docker exec` (admin password read from the
  container), so no published port or basic-auth juggling.
- `moqx-overview.json` is the default home dashboard
  (`GF_DASHBOARDS_DEFAULT_HOME_DASHBOARD_PATH` in docker-compose.yml).

## Public dashboard (opt-in, read-only, internet-facing)

A **tokenized, unauthenticated, read-only** view can be exposed on a dedicated
public port (`STATS_PUBLIC_PORT`, default `4533`) — served by a separate nginx
server block that proxies **only** the Grafana public-dashboard routes; login,
admin, `/metrics`, `/prometheus`, and every other path return `403`. The private
`443` (tunnel) surface is unchanged. It's **off until you bind the port public.**

Enable it:
```bash
# 1) publish the dashboard as a public one (mints/reuses a token, prints the URL)
cd docker/grafana && ./publish-public-dashboard.sh publish

# 2) bind the public port to the internet and (re)create nginx
#    in docker/.env:  STATS_PUBLIC_BIND=0.0.0.0   (optionally STATS_PUBLIC_PORT=<obscure>)
cd .. && docker compose --profile stats up -d nginx
```
Link the printed `https://<domain>:<port>/grafana/public-dashboards/<token>` from
the repo/wiki. Viewers get read-only panels + time-range browsing, **no** write,
**no** arbitrary queries, **no** other route.

Controls:
- **Kill switch**: `./publish-public-dashboard.sh pause` (data stops serving) or
  `unpublish` (revokes the token). Or set `STATS_PUBLIC_BIND=127.0.0.1` + recreate
  nginx to pull the port entirely.
- **Ban an IP**: add `deny 1.2.3.4;` to `docker/nginx/denylist.conf`, then
  `docker exec moqx-proxy nginx -s reload`.
- **Rate/conn caps** (per IP) are in the nginx template (`limit_req` 5r/s burst 20,
  `limit_conn` 10) → excess gets `429`.
- **Auto-ban (fail2ban)**: point a jail at nginx's access log for a flood of
  `429`s and have its action append to `denylist.conf` + reload nginx.

Security posture: anonymous **org** auth stays OFF (that would expose the whole
instance); only the per-dashboard public token is exposed. Nothing in the relay
metrics is private (open test relay), so the full dashboard is published as-is.
