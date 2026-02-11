# Configuration & Hot-Reloading Design

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

| Project | Config Format | Hierarchy | Dynamic Reload | Runtime API | Binary Restart |
|---------|--------------|-----------|----------------|-------------|----------------|
| **nginx** | Custom DSL | `http` → `server` → `location` | `SIGHUP`: new workers, old drain | nginx+ only (commercial) | New workers inherit listen sockets; old workers drain |
| **Envoy** | YAML/JSON | `static` (bootstrap) + `dynamic` (xDS) | xDS API: listeners, clusters, routes, endpoints | Full xDS; also admin API | Hot restart via shared memory between old/new |
| **HAProxy** | Custom DSL | `global` → `defaults` → `frontend` → `backend` | `-sf` soft reload; new process inherits sockets | Runtime API for limited changes (server weights, health) | Socket inheritance via Unix domain sockets |
| **Caddy** | JSON + Caddyfile | Sites → routes → handlers | Full config reload via API, zero-downtime | First-class JSON API for all config | Socket inheritance via `SO_REUSEPORT` |
| **Traefik** | YAML/TOML + providers | `static` (entrypoints, global) + `dynamic` (routers, services) | File provider watches for changes | Kubernetes/Docker/file providers | Relies on orchestrator (no built-in) |

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
global          ─── process-wide settings
├── listeners   ─── listen address/port/protocol/TLS (maps to transport layer)
└── services    ─── matched by MOQT authority + path (maps to CLIENT_SETUP)
    └── namespaces ─── matched by track namespace prefix (maps to SUBSCRIBE/PUBLISH)
```

### Matching Model

A client connects to a **listener** (transport), then sends `CLIENT_SETUP` with **authority** and **path**. These are matched against **services**. Subsequent `SUBSCRIBE`/`PUBLISH` messages carry track namespaces, matched against **namespace** rules within the matched service.

**Matching priority** (highest to lowest):
1. **Exact match**: `authority: "live.example.com"`
2. **Wildcard prefix**: `authority: "*.example.com"`
3. **Regex**: `authority_regex: "^[a-z]+-region[0-9]+\\.example\\.com$"`
4. **Default/catch-all**: `authority: "*"`

Same priority order applies to `path` and `namespace_prefix`.

> **Specificity rule**: When multiple prefix rules match within the same priority category, the more specific prefix wins. "More specific" means a longer tuple (more fields) or a domain with more segments (more dots), not longer in terms of bytes.

> **Performance note**: Regex matching is evaluated only when exact and prefix matches fail. Regex is appropriate for service-level matching (per-connection), which happens once at session setup. Namespace-level regex should be used sparingly and is explicitly not evaluated per-object — only at subscription setup time.

> **MOQT Scope note**: The combination of authority + path defines a MOQT scope ([moq-transport#1432](https://github.com/moq-wg/moq-transport/issues/1432). Sessions within the same scope share namespace trees and cache. The config hierarchy reflects this: each service (authority + path pair) is an isolated scope by default. Cross-scope sharing (e.g., shared cache across authorities) can be configured explicitly via named shared resources.

### Inheritance

Configuration parameters are inherited down the hierarchy. A more specific level overrides its parent. This follows the nginx model where `server` inherits from `http`, and `location` overrides `server`.

### Include Directives

Includes are supported **only at the service level**: a `services_include` directive loads service definitions from external files. This keeps the main config file manageable while preventing fragmentation of the hierarchy at arbitrary levels.

```yaml
services_include:
  - /etc/o-rly/services.d/*.yaml  # Each file defines one or more services
```

Each included file must contain complete service definitions. Partial overrides or nested includes are not supported. This is intentional.

## Configuration Schema

### Parameter Lifecycle Classification

Every configuration parameter belongs to one of four lifecycle categories. The config schema documentation (generated from the C++ struct definitions) must annotate each parameter with its lifecycle.

| Lifecycle | Meaning | Examples                                                                            |
|-----------|---------|-------------------------------------------------------------------------------------|
| **Static** | Requires process restart. Cannot be changed at runtime. | `worker_threads`, `listeners[].address`, `listeners[].port`|
| **Reload:NewConn** | Applied on config reload. Affects new connections/sessions only. Existing sessions keep old values. | `services[].auth_plugin`, `services[].provider`, TLS cert/key, service matching rules |
| **Reload:NewSub** | Applied on config reload. Affects new subscriptions/tracks only. Existing subscriptions keep old values. | `namespaces[].max_max_cache_duration_ms`, routing rules, authorization rules        |
| **Reload:Immediate** | Applied on config reload. Takes effect on all existing connections/subscriptions. | `log_level`, `slow_subscriber_queue_max`, cache eviction policy, metrics config     |

## Design Decisions

### YAML Over Custom DSL

A custom DSL (like nginx's config language) would be more concise for our domain but:

- Requires building and maintaining a parser, error reporting, syntax highlighting, linting tools.
- Creates a learning curve for every new operator.
- nginx's DSL is frequently cited as a source of misconfiguration (e.g., [trailing slash behavior](https://www.nginx.com/resources/wiki/start/topics/tutorials/config_pitfalls/), `if` is evil).
- YAML gets us most of the expressiveness with zero parser maintenance and existing tooling (IDE support, linting, schema validation).

### Hierarchical Services Over Flat Config

**Chosen**: Hierarchical (authority → path → namespace) with inheritance.

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
- **Namespace matching** (per-subscription): Regex is acceptable but should be simple. Evaluated once at subscription setup.

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
