# Prometheus scrape config

## Per-track metrics (`moqx-track`)

`/metrics/track` requires a `namespace` parameter, so every namespace to be
graphed is a scrape target that relabelling turns into the query parameter.
Namespace values use the moq-transport encoded form — tuple elements joined by
`-`, other bytes as `.<hex>`:

    moq-test/interop   ->  moq.2dtest-interop
    conf.example.com / room 1  ->  conf.2eexample.2ecom-room.201

`limit` is a guard rather than a selector: a namespace holding more tracks than
the limit returns 400, so an over-wide scrape fails visibly instead of graphing
a series set that reshuffles between scrapes. Ceiling is
`admin.track_metrics_max_limit` (1000).

### Generating the target list

The static list needs maintaining by hand. `/state.namespace_tree` already
enumerates live namespaces, so the intended replacement is a small generator
that walks the tree, encodes each namespace, and writes a `file_sd` target
file; Prometheus reloads target files without a restart.

    file_sd_configs:
      - files: ['/etc/prometheus/targets/namespaces.json']
        refresh_interval: 30s

Namespaces churn as events start and end, and a relay may hold more than the
per-scrape ceiling, so this becomes necessary rather than convenient once the
relay carries production traffic.
