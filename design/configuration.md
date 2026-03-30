# Configuration & Hot-Reloading Design

## TODO

- Single MOQT implementation per listner?
  - Different ALPNs and SNI could have different moqt handlers.
- In "panel-to-audience" model (i.e. few real-time participatns, many low-latency viewers), how to define different tunables for the panel as oposite to the audience?

## Overview

The relay needs both file-based configuration for base deployment and a runtime API for dynamic updates. Configuration must be hierarchical, mapping naturally to MOQT's connection model (authority, path, namespace).

### Requirements References

- [Relay Requirements 2.2](https://github.com/OpenMOQ/relay-requirements/blob/main/relay-requirements.md): "configurable via environment variables and/or a configuration file (e.g., YAML, TOML). Configuration should be reloadable without requiring a server restart where possible."
- [Relay Requirements 3.3.2](https://github.com/OpenMOQ/relay-requirements/blob/main/relay-requirements.md): Control Plane API for management and orchestration.
- [Relay Requirements 3.6](https://github.com/OpenMOQ/relay-requirements/blob/main/relay-requirements.md): Congestion control configurable at minimum per service.

### Goals

1. **File-based configuration** as the primary interface for base deployment.
2. **Runtime API** for dynamic updates in orchestrated/CDN environments.
3. **Hierarchical structure** that maps to MOQT connection model.
4. **Clear reloadability semantics**: which parameters are static, which are hot-reloadable, and how reload affects existing connections/subscriptions.
5. **Graceful binary restart** for deployments requiring process replacement.

## Prior Art

| Project     | Config Format         | Hierarchy                                                      | Dynamic Reload                                  | Runtime API                                              | Binary Restart                                        |
| ----------- | --------------------- | -------------------------------------------------------------- | ----------------------------------------------- | -------------------------------------------------------- | ----------------------------------------------------- |
| **nginx**   | Custom DSL            | `http` → `server` → `location`                                 | `SIGHUP`: new workers, old drain                | nginx+ only (commercial)                                 | New workers inherit listen sockets; old workers drain |
| **Envoy**   | YAML/JSON             | `static` (bootstrap) + `dynamic` (xDS)                         | xDS API: listeners, clusters, routes, endpoints | Full xDS; also admin API                                 | Hot restart via shared memory between old/new         |
| **HAProxy** | Custom DSL            | `global` → `defaults` → `frontend` → `backend`                 | `-sf` soft reload; new process inherits sockets | Runtime API for limited changes (server weights, health) | Socket inheritance via Unix domain sockets            |
| **Caddy**   | JSON + Caddyfile      | Sites → routes → handlers                                      | Full config reload via API, zero-downtime       | First-class JSON API for all config                      | Socket inheritance via `SO_REUSEPORT`                 |
| **Traefik** | YAML/TOML + providers | `static` (entrypoints, global) + `dynamic` (routers, services) | File provider watches for changes               | Kubernetes/Docker/file providers                         | Relies on orchestrator (no built-in)                  |

### Key Takeaways

- **Envoy's static/dynamic split** is the strongest model for our use case: some things (listeners, thread count) are inherently static; routes, upstreams, and policies are inherently dynamic.
- **Custom DSLs** (nginx, HAProxy) are powerful but create a learning barrier and maintenance burden. Both projects have paid the cost of building and maintaining parsers, and users frequently cite config syntax as a pain point.
- **All mature proxies** distinguish between "needs new process" and "can update in-place" parameters.

## Configuration Format

### Decision: YAML

**Chosen**: YAML as the file-based configuration format.

**Rationale:**

- Widely used in infrastructure tooling. Operators already know it.
- Supported by mature C++ parsing libraries ([yaml-cpp](https://github.com/jbeder/yaml-cpp), [rapidyaml](https://github.com/biojppm/rapidyaml)).

**YAML pitfalls** (e.g., the "Norway problem" where `NO` is parsed as boolean): mitigated by always deserializing into strongly-typed C++ structs. The relay never interprets raw YAML scalars as implicit types. Values are always parsed as the expected type defined in the config schema, or rejected. This is consistent with how Envoy and Kubernetes handle YAML.

For the API we can use the same format, require JSON serialization or go with something completly else (e.g. JSON Patch for updating).

## Configuration Hierarchy

The hierarchy should map to the MOQT connection model.

```
global              ─── process-wide settings (worker_threads, log_level, telemetry)
├── listeners[]     ─── named listener definitions
│   ├── udp/tcp     ─── transport type (exactly one)
│   │   ├── socket  ─── address, port, ipv6_only
│   │   └── transport ─── implementation-specific config (e.g. mvfst, proxygen)
│   │       └── protocols, tunables
│   ├── moqt_implementation ─── MOQT library to use (e.g. moxygen, libquicr)
│   └── tls                 ─── TLS cert/key provider config
├── service_defaults ─── defaults inherited by all services
└── services[]       ─── matched by MOQT authority + path (maps to CLIENT_SETUP), defines MOQT scope
    ├── listeners    ─── optional: restrict which listeners serve this service
    ├── provider     ─── upstream/origin configuration (type + config)
    └── tracks[]     ─── matched by namespace and track matchers (maps to SUBSCRIBE/PUBLISH)
```

### Matching Model

A client connects to a **listener** (transport), then sends `CLIENT_SETUP` with **authority** and **path**. These are matched against **services**.

Each service defines one or more `match` entries. Each entry pairs an **authority** matcher with a **path** matcher:

```yaml
services:
  example:
    match:
      - authority: {exact: "live.example.com"}
        path: {exact: "/moq-relay"}
      - authority: {wildcard: "*.live.example.com"}
        path: {prefix: "/live/"}
      - authority: {any: true}
        path: {prefix: "/"}
```

**Authority matching priority** (highest to lowest):

1. **Exact**: `{exact: "live.example.com"}` — O(1) hash lookup.
2. **Wildcard**: `{wildcard: "*.example.com"}` — matches single-label subdomains only (e.g. `foo.example.com`), not the bare domain or multi-label subdomains. O(1) suffix lookup.
3. **Any**: `{any: true}` — catch-all fallback.

Regex authority matching is planned but not yet implemented.

**Path matching priority** (within matched authority tier):

1. **Exact**: `{exact: "/moq-relay"}` — O(1) hash lookup.
2. **Prefix**: `{prefix: "/live/"}` — longest prefix wins. Uses simple string prefix matching (not segment-aware).

`{prefix: "/"}` matches any valid MOQT path and serves as the catch-all pattern.

> **Specificity rule**: When multiple prefix rules match within the same priority category, the more specific prefix wins. "More specific" means a longer tuple (more fields) or a domain with more segments (more dots), not longer in terms of bytes.

> **Performance note**: Regex matching is evaluated only when exact and prefix matches fail. Regex is appropriate for service-level matching (per-connection), which happens once at session setup. Track-level regex (in namespace/track matchers) should be used sparingly and is explicitly not evaluated per-object — only at subscription setup time.

> **TODO: WebTransport endpoint gate limitation**
>
> How authority and path reach `ServiceMatcher` differs by transport:
>
> | Transport | Authority source | Path source | Pre-gate |
> |-----------|-----------------|-------------|----------|
> | **WebTransport** | HTTP `Host` header | HTTP URL path | `isAcceptedEndpoint()` exact match — 404 if path not in set |
> | **Raw QUIC** | `SetupKey::AUTHORITY` in CLIENT_SETUP | `SetupKey::PATH` in CLIENT_SETUP | None |
>
> Moxygen's `isAcceptedEndpoint()` check runs *before* the WebTransport upgrade completes. The listener's `endpoint` field is registered as the only accepted path. If a WT client connects with an HTTP path that doesn't match a registered endpoint, moxygen returns 404 before MOQT ever starts — `ServiceMatcher` never sees the connection.
>
> This means multi-service path matchers (e.g. `{exact: "/moq-relay"}` and `{prefix: "/live/"}`) only work for WT connections if every matched path is also registered as a listener endpoint via `MoQServerBase::addEndpoint()`. Currently moqx passes only a single `endpoint` from the listener config.
>
> Additionally, moxygen's conflict detection (`MoQSession::onClientSetup()`): if WT already set authority/path from HTTP and CLIENT_SETUP also includes them, the session gets a PROTOCOL_VIOLATION.
>
> **Fix needed upstream**: make moxygen's endpoint check prefix-aware, or auto-populate endpoints from service path matchers at startup. Until then, multi-service path routing is effectively limited to raw QUIC, or to WT deployments where all services use the same path (the listener endpoint).

### Inheritance

Configuration parameters are inherited down the hierarchy. A more specific level overrides its parent. This follows the nginx model where `server` inherits from `http`, and `location` overrides `server`.

The `service_defaults` block provides defaults inherited by all services. Individual services can override any of these values.

### Include Directives

Includes are supported **only at the service level**: a `services_include` directive loads service definitions from external files. This keeps the main config file manageable while preventing fragmentation of the hierarchy at arbitrary levels.

```yaml
services_include:
  - /etc/moqx/services.d/*.yaml # Each file defines one or more services
```

Each included file must contain complete service definitions. Partial overrides or nested includes are not supported.

## Configuration Schema

### Parameter Lifecycle Classification

Every configuration parameter belongs to one of four lifecycle categories. The config schema documentation (generated from the C++ struct definitions) must annotate each parameter with its lifecycle.

| Lifecycle            | Meaning                                                                                                  | Examples                                                                              |
| -------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| **Static**           | Requires process restart. Cannot be changed at runtime.                                                  | `worker_threads`, `listeners[].udp.socket.address`, `listeners[].udp.socket.port`     |
| **Reload:NewConn**   | Applied on config reload. Affects new connections/sessions only. Existing sessions keep old values.      | `services[].auth`, `services[].provider`, TLS cert/key, service matching rules        |
| **Reload:NewSub**    | Applied on config reload. Affects new subscriptions/tracks only. Existing subscriptions keep old values. | `tracks[].max_max_cache_duration_ms`, routing rules, authorization rules              |
| **Reload:Immediate** | Applied on config reload. Takes effect on all existing connections/subscriptions.                        | `log_level`, `slow_subscriber_queue_max_bytes`, cache eviction policy, metrics config |

## Design Decisions

### YAML Over Custom DSL

A custom DSL (like nginx's config language) would be more concise for our domain but:

- Requires building and maintaining a parser, error reporting, syntax highlighting, linting tools.
- Creates a learning curve for every new operator.
- nginx's DSL is frequently cited as a source of misconfiguration (e.g., [trailing slash behavior](https://www.nginx.com/resources/wiki/start/topics/tutorials/config_pitfalls/), `if` is evil).
- YAML gets us most of the expressiveness with minimal parser maintenance and existing tooling (IDE support, linting, schema validation).

### Hierarchical Services Over Flat Config

**Chosen**: Hierarchical (authority → path → tracks) with inheritance.

**Alternative**: Flat list of rules with explicit matchers (like Envoy's route table).

Hierarchical is chosen because:

- Maps directly to the MOQT connection model: authority and path are per-session (CLIENT_SETUP), namespaces are per-subscription.
- Inheritance reduces repetition. If 50 namespace prefixes under one authority share the same auth and upstream, you define those once at the service level.
- Operators think in terms of "services" (the nginx `server` block mental model is pervasive).

### SIGHUP + API, Not inotify/fswatch

We use explicit signal-based reload rather than filesystem watching because:

- **Atomic**: The operator decides when to reload. No risk of picking up a partially-written file.
- **Auditable**: Reload events are explicit and logged.
- Operators using config management tools (Ansible, Puppet) already follow the "write file, then signal" pattern.

The runtime API's (e.g. `POST /v1/config/reload`) provides the same functionality for environments where sending signals is inconvenient (e.g., Kubernetes, where you'd use a sidecar or liveness probe).

### Regex: Supported but Discouraged for High-Frequency Paths

Regex matching is available at all hierarchy levels but is documented as a last resort:

- **Service matching** (per-connection): Regex is acceptable. Evaluated once at session setup.
- **Track matching** (per-subscription): Regex in namespace/track matchers is acceptable but should be simple. Evaluated once at subscription setup.

Matching order within a hierarchy level: exact → prefix (longest match) → regex (first match, config order) → default. This is the same precedence as nginx's location matching.

### Config File Persistence from API

**Chosen**: API changes persist to the config file by default.

**Alternative**: API is ephemeral (HAProxy model).

Persistence prevents config drift, which is the #1 operational problem with HAProxy's runtime API. If an operator modifies a parameter via the API and the process restarts, the change is preserved. This can be disabled for ephemeral/immutable-infrastructure deployments where config is injected at startup and never modified in-place.

When persistence is enabled, the config file is rewritten atomically (write to temp file, then `rename(2)`) after each API mutation.

### No Cross-Service Includes

Include files must contain complete service definitions. This prevents:

- Diamond inheritance problems (service A includes fragment X, service B includes fragment X with overrides).
- Difficulty debugging "where did this value come from?"
- Circular or multi-level include chains.

If operators need shared snippets, they should use their config management tool's templating (Jinja, ERB, Go templates) to generate the final YAML before deploying.

## Configuration Validation Tool

The relay must provide a CLI tool (or subcommand) for validating configuration files before deployment.

### Requirements

- **Syntax validation**: Parse YAML and report syntax errors with line numbers.
- **Schema validation**: Validate against the config schema (required fields, types, allowed values).
- **Semantic validation**: Check for logical errors (e.g., duplicate service names, overlapping matchers, invalid regex patterns, referenced files exist).
- **Dry-run mode**: Load the config as the relay would, including resolving includes, without starting listeners.
- **Diff mode**: Compare two config files and show what would change (useful for reviewing changes before reload).
- **Exit codes**: Return non-zero on validation failure for CI/CD integration.

This follows the pattern established by `nginx -t`, `envoy --mode validate`, and `haproxy -c`.
