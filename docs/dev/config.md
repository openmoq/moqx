# Config System

For user-facing configuration reference, see [docs/config.md](../config.md).

## Overview

Config goes through three stages:

```
YAML file  →  ParsedConfig  →  resolveConfig()  →  Config (via ResolvedConfig)
               (Loader.h)       (ConfigResolver.h)    (Config.h)
```

1. **Load & parse** — `loadConfig()` reads a YAML file and deserializes it into `ParsedConfig` structs using reflect-cpp (`rfl::yaml::read`). This handles syntax and type checking. Optional `strict` mode rejects unknown fields.

2. **Validate & resolve** — `resolveConfig()` takes a `ParsedConfig` and applies semantic validation (e.g. "port must be non-zero", "cert required when not insecure"). It transforms the parsed representation into the final `Config` types and collects warnings for non-fatal issues.

3. **Use** — The rest of the codebase only sees `Config` (and its nested types like `ListenerConfig`, `CacheConfig`). These are plain C++ structs with no reflect-cpp dependency.

The split between `ParsedConfig` and `Config` exists because the YAML-facing shape (flat fields, optionals, description annotations) differs from what the application needs (e.g. `folly::SocketAddress`, `std::variant<Insecure, TlsConfig>`).

## Key files

| File | Role |
|---|---|
| `src/config/loader/ParsedConfig.h` | YAML-facing config structs with `rfl::Description` annotations |
| `src/config/Config.h` | Final config types used by the application |
| `src/config/ResolvedConfig.h` | `ResolvedConfig` — wraps `Config` + warnings |
| `src/config/loader/Loader.h` | `loadConfig()`, `generateSchema()` |
| `src/config/Loader.cpp` | Load & parse implementation |
| `src/config/loader/ConfigResolver.h` | `resolveConfig()` |
| `src/config/ConfigResolver.cpp` | Validation & resolution logic |
| `src/config/loader/ConfigInit.h` | CLI subcommand handling (`validate-config`, `dump-config-schema`) |
| `src/config/ConfigInit.cpp` | Subcommand orchestration |
| `src/config/loader/StringLiteral.h` | Compile-time string utilities for descriptions |
| `config.example.yaml` | Example config file |

## How to add a new config field

### 1. Add to `ParsedConfig` (`src/config/loader/ParsedConfig.h`)

Add a field to the appropriate `Parsed*Config` struct. Every field is wrapped in `rfl::Description<"...", T>` which serves double duty: the string becomes the JSON schema description, and the type `T` is what gets deserialized from YAML.

```cpp
struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", std::optional<bool>> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", std::optional<uint32_t>> max_tracks;
  // New field:
  rfl::Description<"Max cached groups per track, ignored when disabled", std::optional<uint32_t>> max_groups_per_track;
};
```

Use `std::optional<T>` for fields that can be omitted in the YAML file:

```cpp
rfl::Description<"Optional timeout in ms", std::optional<uint32_t>> timeout_ms;
```

Field names use `snake_case` — reflect-cpp maps them directly to YAML keys.

**Accessing values:** `rfl::Description` wraps the inner type, accessed via `.value()`. For `std::optional` fields that's two levels: `field.value()` returns the `std::optional`, then `.value()` or `.value_or()` unwraps it.

### 2. Add to `Config` (`src/config/Config.h`)

Add the corresponding field to the final config struct. Use concrete types — no reflect-cpp wrappers, no optionals (resolve defaults in step 3).

```cpp
struct CacheConfig {
  size_t maxCachedTracks; // 0 when cache disabled
  // New field:
  size_t maxCachedGroupsPerTrack;
};
```

### 3. Validate and resolve (`src/config/ConfigResolver.cpp`)

In `resolveConfig()`:
- Add validation rules to the validation section (push errors/warnings as needed).
- Map the parsed field to the final `Config` field in the resolution section.

```cpp
// Validation
if (parsed.max_groups_per_track.value().value_or(0) == 0) {
  errors.push_back("cache.max_groups_per_track must be >= 1");
}

// Resolution
CacheConfig cacheConfig{
    .maxCachedTracks = parsed.max_tracks.value().value_or(0),
    .maxCachedGroupsPerTrack = parsed.max_groups_per_track.value().value_or(1),
};
```

### 4. Update `config.example.yaml`

Add the new field with a comment explaining it.

### 5. Adding a new config section

If you need a new top-level section (not just a field), also:
- Create a new `Parsed*Config` struct in `ParsedConfig.h` and add it to `ParsedConfig`.
- Create a new final struct in `Config.h` and add it to `Config`.
- Handle it in `resolveConfig()`.

## reflect-cpp notes

- We use reflect-cpp v0.18.0 for YAML deserialization and JSON schema generation.
- `rfl::Description<"text", T>` annotates fields for schema generation. Access the inner value with `.value()`.
- `rfl::yaml::read<T>()` deserializes; `rfl::yaml::read<T, rfl::NoExtraFields>()` for strict mode.
- `rfl::json::to_schema<T>()` generates JSON schema from the type structure.
- Compile-time string utilities in `StringLiteral.h` help build description strings from constants.
