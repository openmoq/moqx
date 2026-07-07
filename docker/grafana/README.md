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
