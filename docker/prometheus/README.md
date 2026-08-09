# Prometheus scrape config

## Per-track metrics (`moqx-track`)

`/metrics/track` takes optional `service`, `namespace`, `track` and `limit`
parameters; with none of them it reports every live track, so a single scrape
covers the relay.

`limit` is a guard rather than a selector: more live tracks than the limit
returns 400, so an over-wide scrape fails visibly instead of graphing a series
set that reshuffles between scrapes. Ceiling is
`admin.track_metrics_endpoint_max_limit` (1000).

Past that ceiling the scrape splits by namespace prefix, one job per prefix,
with `/state.namespace_tree` generating the target list into a `file_sd` file:

    file_sd_configs:
      - files: ['/etc/prometheus/targets/namespaces.json']
        refresh_interval: 30s

Label values use the moq-transport encoded form — tuple elements joined by
`-`, other bytes as `.<hex>`:

    moq-test/interop           ->  moq.2dtest-interop
    conf.example.com / room 1  ->  conf.2eexample.2ecom-room.201

Series exist only for live tracks: they disappear when a track ends and restart
from zero if it returns, so counters need `increase()`/`rate()` rather than
raw deltas across a track's lifetime.

Counting is installed only when `admin.track_metrics_enabled` is true (default);
with it false the endpoint returns 503.
