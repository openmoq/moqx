# Plugins

Prior discussion:

- [#3](https://github.com/openmoq/moqx/pull/3): `dlopen` module loader with per-type C++ interfaces.
- [#814](https://github.com/openmoq/moqx/pull/814): plugin levels, hook model and use cases (slides). Discussed at the relay-devs meeting on 2026-10-12.
- [#651](https://github.com/openmoq/moqx/pull/651): multi-cert TLS, parked until the plugin model settles.

## Goals

- Users and forks customize the relay through plugins, without changing its core.
- Auth, RelayHops and the memory cache move into plugins. They are the API's first users.
- One hook model serves every level. Later levels restrict it.
- Plugins see MoQ semantics through moqx-owned types.

Use cases the model must cover:

- Auth: admit a session, authorize each request, possibly by asking an external service.
- Peering and routing: relay hops, loop suppression, default upstream.
- Cache: passive ingest, fetch serving, tiered storage.
- Object filters: per-object inspection or transformation, configured or registered by a control-plane hook.
- TLS: certificate selection and signing with keys held outside moqx.
- Plugin-originated media: a plugin subscribes and publishes inside the relay, for example to watermark one subscriber's copy.
- Synthetic responses: a plugin answers a request itself, as a local publisher or subscriber. The host needs explicit support for this.

## Non-goals

- Changing a live service's plugin config. See [hot-reloading.md](hot-reloading.md).
- Request rewriting. It needs a dispatch refactor first.
- A security boundary for native code.

## Levels

| Level | Plugin is | Loaded | Trust | For |
| --- | --- | --- | --- | --- |
| 1 | C++ compiled into moqx | Build time | Trusted | In-tree plugins and forks that accept a rebuild |
| 2 | Shared library, stable C ABI | Startup (`dlopen`) | Trusted | Plugins shipped separately, written against an SDK |
| 3 | WebAssembly module | Config reload | Trusted | Operators changing behavior without a restart |
| 4 | WebAssembly module | Config reload | Untrusted | Tenants running their own code at the edge |

- Every level gets a stability policy. Nothing is stable before v1.

### Level 1: compiled in

- Plugins are C++ and use folly directly (`coro::Task`, `IOBuf`, executors, cancellation).
- A fork adds plugins in tree.
- Compatibility is at the C++ source level. Plugins rebuild with moqx.
- Limits:
  - Adding or upgrading a plugin means rebuilding moqx.
  - A plugin can crash the relay or corrupt its memory.
  - No unload or reload.

### Level 2: stable C ABI

- The ABI is C. A C++ ABI ties plugins to the host's toolchain and build flags.
- We provide SDKs for C, C++ and Rust at minimum. The model follows [Envoy dynamic modules](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/advanced/dynamic_modules).
- Async becomes a continuation: a hook returns `PENDING` and completes through a one-shot host callback.
- Payloads cross as refcounted slices. Retaining a slice copies no payload bytes.
- Exceptions and panics do not cross the boundary.
- The ABI is versioned.
- Limits:
  - Loaded only at startup.
  - Same trust as Level 1.
  - Linux only at first ([#3](https://github.com/openmoq/moqx/pull/3)).
  - Config needs a serialized subtree or a typed accessor API. `folly::dynamic` cannot cross a C ABI.

### Levels 3 and 4: WebAssembly

- Same hooks, restricted to a subset.
- Level 3 needs no per-tenant accounting.
- Level 4 isolates the module. Wasm confines its memory, and the host functions it imports bound what it can reach.
- Level 4 adds CPU and memory accounting per tenant.
  - Wasmtime epochs and fuel don't interrupt a guest blocked in a host call. Host functions need their own timeouts.
- Limits:
  - Per-call boundary cost is unmeasured. Per-object hooks need a benchmark first. Batching and a filter DSL are alternatives.
  - The hook subset and the Wasm ABI are undecided.

## Hook model

Shared by all levels.

- Control-plane hooks run per request, or per session lifetime event such as setup and close.
- Data-plane hooks run per object, on ingress and egress.
- Transport hooks run per listener and per TLS handshake, before admission.
- A plugin configures the data plane: its control-plane hooks register and deregister data-plane hooks for a request or a track.
- A plugin may register several data-plane hooks, each with its own rank.
- The host owns request IDs, protocol validation, routing and fanout.
- Plugins own policy, per-session state and storage.

### Ordering

- Every control-plane and data-plane hook has an integer `rank`. Lower ranks run first.
- Hooks with equal rank run in config order.
- Egress runs the whole chain in reverse by default, equal ranks included.
- A plugin may expose its hooks' ranks in its config.
- The first rejection stops a control-plane chain.
