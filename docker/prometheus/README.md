# Prometheus scrape config

## Per-track metrics (`moqx-track`)

`/metrics/track` reports counters per live track. It takes one namespace per
request, so each namespace is scraped separately.

Scoping per namespace is not just tidiness. `limit` is a guard rather than a
selector: a request matching more tracks than the limit returns 400 instead of
truncating, because an arbitrary subset would give Prometheus a series set that
reshuffles between scrapes. An unscoped scrape therefore fails as soon as the
relay's *total* track count passes the limit, and takes every namespace with
it, while per-namespace scrapes keep working until a *single* namespace passes
it. Ceiling is `admin.track_metrics_endpoint_max_limit` (1000).

Counting only happens when `admin.track_metrics_enabled` is true (the default);
with it false the endpoint returns 503 rather than an empty scrape that would
read as "no live tracks".

## Target generation (`ns-targets`)

Namespaces come and go with events, so the target list cannot be static.
`namespace-targets.py` walks the relay's `/state` namespace tree every 30s and
writes one target per namespace to a file Prometheus rereads without a restart.
It collects every node carrying a namespace rather than only the leaves, since
tracks can be published at any depth.

Targets are namespace values in the moq-transport safe form — `[A-Za-z0-9_]`
passes through, every other byte becomes `.<hex>`, tuple elements join with
`-`:

    moq-test/interop           ->  moq.2dtest-interop
    conf.example.com / room 1  ->  conf.2eexample.2ecom-room.201

Relabelling turns each target into the `namespace` query parameter and points
the scrape at the relay. The unencoded namespace is kept as `moqx_namespace`
for display, since the encoded form is what lands in the metric labels.

Series exist only while a track is live: they disappear when it ends and
restart from zero if it returns, so counters need `rate()`/`increase()` rather
than differences taken across a track's lifetime.
