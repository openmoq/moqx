# Config System

## Overview

Config goes through three stages:

```
YAML file  →  ParsedConfig  →  resolveConfig()  →  Config (via ResolvedConfig)
               (loader.h)      (config_resolver.h)   (config.h)
```

1. **Load & parse** — `loadConfig()` reads a YAML file and deserializes it into `ParsedConfig` structs using reflect-cpp (`rfl::yaml::read`). This handles syntax and type checking. Optional `strict` mode rejects unknown fields.

2. **Validate & resolve** — `resolveConfig()` takes a `ParsedConfig` and applies semantic validation (e.g. "port must be non-zero", "cert required when not insecure"). It transforms the parsed representation into the final `Config` types and collects warnings for non-fatal issues.

3. **Use** — The rest of the codebase only sees `Config` (and its nested types like `ListenerConfig`, `CacheConfig`). These are plain C++ structs with no reflect-cpp dependency.

The split between `ParsedConfig` and `Config` exists because the YAML-facing shape (flat fields, optionals, description annotations) differs from what the application needs (e.g. `folly::SocketAddress`, `std::variant<Insecure, TlsConfig>`).

## Key files

| File | Role |
|---|---|
| `include/moqx/config/loader/parsed_config.h` | YAML-facing config structs with `rfl::Description` annotations |
| `include/moqx/config/config.h` | Final config types used by the application |
| `include/moqx/config/resolved_config.h` | `ResolvedConfig` — wraps `Config` + warnings |
| `include/moqx/config/loader/loader.h` | `loadConfig()`, `generateSchema()` |
| `include/moqx/config/loader/config_resolver.h` | `resolveConfig()` |
| `include/moqx/config/loader/config_init.h` | CLI subcommand handling (`validate-config`, `dump-config-schema`) |
| `src/config/loader.cpp` | Load & parse implementation |
| `src/config/config_resolver.cpp` | Validation & resolution logic |
| `src/config/config_init.cpp` | Subcommand orchestration |
| `config.example.yaml` | Example config file |

## How to add a new config field

### 1. Add to `ParsedConfig` (`parsed_config.h`)

Add a field to the appropriate `Parsed*Config` struct. Every field is wrapped in `rfl::Description<"...", T>` which serves double duty: the string becomes the JSON schema description, and the type `T` is what gets deserialized from YAML.

```cpp
struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", bool> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", uint32_t> max_tracks;
  // New field:
  rfl::Description<"Eviction policy", std::string> eviction_policy;
};
```

Use `std::optional<T>` for fields that can be omitted in the YAML file:

```cpp
rfl::Description<"Optional timeout in ms", std::optional<uint32_t>> timeout_ms;
```

Field names use `snake_case` — reflect-cpp maps them directly to YAML keys.

**Accessing values:** `rfl::Description` wraps the inner type, accessed via `.value()`. For `std::optional` fields that's two levels: `field.value()` returns the `std::optional`, then `.value()` or `.value_or()` unwraps it.

### 2. Add to `Config` (`config.h`)

Add the corresponding field to the final config struct. Use concrete types — no reflect-cpp wrappers, no optionals (resolve defaults in step 3).

```cpp
struct CacheConfig {
  size_t maxCachedTracks;
  size_t maxCachedGroupsPerTrack;
  // New field:
  std::string evictionPolicy;
};
```

### 3. Validate and resolve (`config_resolver.cpp`)

In `resolveConfig()`:
- Add validation rules to the validation section (push errors/warnings as needed).
- Map the parsed field to the final `Config` field in the resolution section.

```cpp
// Validation
if (cache.eviction_policy.value() != "lru" && cache.eviction_policy.value() != "fifo") {
  errors.push_back("cache.eviction_policy must be 'lru' or 'fifo'");
}

// Resolution
CacheConfig cacheConfig{
    .maxCachedTracks = ...,
    .maxCachedGroupsPerTrack = ...,
    .evictionPolicy = cache.eviction_policy.value(),
};
```

### 4. Update `config.example.yaml`

Add the new field with a comment explaining it.

### 5. Adding a new config section

If you need a new top-level section (not just a field), also:
- Create a new `Parsed*Config` struct in `parsed_config.h` and add it to `ParsedConfig`.
- Create a new final struct in `config.h` and add it to `Config`.
- Handle it in `resolveConfig()`.

## Subcommands

- `moqx dump-config-schema` — prints the JSON schema (auto-generated from `ParsedConfig` + `rfl::Description` annotations).
- `moqx validate-config --config <path>` — loads, parses, and validates without starting the server.
- `--strict_config` flag — rejects unknown YAML fields and promotes warnings to errors.

## reflect-cpp notes

- We use reflect-cpp v0.18.0 for YAML deserialization and JSON schema generation.
- `rfl::Description<"text", T>` annotates fields for schema generation. Access the inner value with `.value()`.
- `rfl::yaml::read<T>()` deserializes; `rfl::yaml::read<T, rfl::NoExtraFields>()` for strict mode.
- `rfl::json::to_schema<T>()` generates JSON schema from the type structure.
- Compile-time string utilities in `string_literal.h` help build description strings from constants.
