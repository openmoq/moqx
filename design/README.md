# Design Documents

This directory contains high-level design documents from early 2026 discussions.
Some are being actively tracked in the roadmap; others will be reworked as the
project evolves. They represent thinking-in-progress rather than finalized
specifications.

## Documents

### [configuration.md](configuration.md)
Comprehensive YAML-based configuration design with hierarchical scope (global →
listeners → services → tracks), a runtime API for hot reload, parameter
lifecycle classification (`static` / `reload:newconn` / `reload:newsub` /
`reload:immediate`), and validation tooling. See also
[configuration.example.yaml](configuration.example.yaml) for a concrete example.

### [hot-reloading.md](hot-reloading.md)
Config reload via `SIGHUP` and a runtime management API for dynamic updates
without restarts. Graceful binary restart uses eBPF-based connection steering to
hand off live connections.

### [miss-handler.md](miss-handler.md)
Hierarchical cache composition abstraction allowing caches to delegate fetches
up a chain (memory → disk → remote → origin). Open questions on filter support
and coroutine memory trade-offs.

### [black-box.md](black-box.md)
Provider abstraction (`RelayProvider` interface) for multi-provider routing with
local fallback and remote fan-out semantics. Covers concrete implementations
(`UpstreamProvider`, `MeshProvider`), bidirectional session handling, and a
migration path for namespace/track resolution across a relay fleet.

### [gummy-bear.md](gummy-bear.md)
Multi-publisher deduplication design: deduplicates overlapping object data from
competing publishers using a shared cache, with per-publisher metrics and
seamless failover. Open questions remain around streaming objects and thread
safety.

### [moxygen-vanilla-plan.md](moxygen-vanilla-plan.md)
Seven-phase plan to wrap moxygen's `Publisher`/`Subscriber` interfaces in a
C++20 standard library API with no folly exposure. Targets API modernization;
all phases are specified but not yet implemented.
