

## Hot-Reloading Design

### Terminology

- **Config reload**: Re-reading and applying configuration without stopping the process. May affect only new or all connections depending on parameter lifecycle.
- **Graceful restart**: Replacing the process binary. Old process drains existing connections; new process accepts new ones.

### 1. File-Based Config Reload (SIGHUP)

Sending `SIGHUP` to the relay process triggers config reload, it does never trigger graceful restart.
Attempting to change static config triggers an error. 

### 2. Runtime API

The relay exposes an HTTP API (per [Relay Requirements 3.3.2](https://github.com/OpenMOQ/relay-requirements/blob/main/relay-requirements.md)) for programmatic configuration. This API is served on a separate listener (the management/control plane listener) and is authenticated via mTLS or API key.

```
GET    /v1/config                    # Return current running config (JSON)
GET    /v1/config/services           # List services
GET    /v1/config/services/{name}    # Get specific service config
PUT    /v1/config                    # Replace entire config (same as file reload)
PATCH  /v1/config/services/{name}    # Update specific service parameters
POST   /v1/config/reload             # Trigger file-based reload (equivalent to SIGHUP)
GET    /v1/config/schema             # Return config schema with lifecycle annotations
```

The API enforces the same lifecycle rules as file-based reload. Attempting to modify a static parameter via the API returns `409 Conflict` with an explanation.

**Consistency**: Changes made via the API are persisted to the config file by default (configurable). This prevents config drift where the running config diverges from the file. If persistence is disabled (e.g., ephemeral container deployment), changes are lost on restart.

This is important because config drift is a common operational problem. Envoy solves this by treating xDS as the source of truth (no file). HAProxy's runtime API explicitly does not persist changes to disk, which operators frequently complain about. We should default to persisting.

### 3. Graceful Binary Restart

For deploying new binaries or making changes to static parameters, the relay supports graceful restart where the old process drains while a new process takes over.

**eBPF-based steering**: An eBPF program attached to the UDP socket steers packets for existing connection IDs to the old process and new connections to the new process.

> **Note**: We will likely need eBPF-based steering between I/O threads anyway for QUIC connection affinity (ensuring all packets for a given connection are processed by the same thread). Extending this to process-level steering during restarts is incremental work on top of that foundation. See [QUIC-LB](https://datatracker.ietf.org/doc/draft-ietf-quic-load-balancers/) and [Relay Requirements §3.7](https://github.com/OpenMOQ/relay-requirements/blob/main/relay-requirements.md) for the connection ID generation API requirement.

**State handoff**: We explicitly do **not** attempt to transfer in-memory state (cache, subscription state) between old and new processes. This is complex, error-prone, and the correct approach for stateless horizontal scaling is to rely on upstream re-subscription and cache warming. GOAWAY gives clients the signal to reconnect to the new process.
