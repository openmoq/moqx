# Module Loader Design Plan

## Overview

A relay server plugin system using dynamic loading (`.so` files via `dlopen`/`dlsym`). Supports multiple plugin types with different interfaces: authentication, miss handlers, routing, etc.

## Core Concepts

### Plugin Types

Each plugin type has:
- A well-known interface (pure virtual class)
- A factory function symbol name
- Version compatibility info

```cpp
enum class PluginType {
  Auth,
  MissHandler,
  Router,
  // extensible...
};
```

### Plugin Interface Base

All plugins inherit from a common base for lifecycle management:

```cpp
class Plugin {
 public:
  virtual ~Plugin() = default;
  virtual const char* name() const = 0;
  virtual uint32_t version() const = 0;
};
```

### Type-Specific Interfaces

Each plugin type defines its interface:

```cpp
// Auth plugin interface
class AuthPlugin : public Plugin {
 public:
  struct AuthResult {
    bool allowed;
    std::string reason;
    // additional metadata...
  };

  virtual AuthResult authenticate(
      const std::string& token,
      const std::string& resource) = 0;
};

// Miss handler plugin interface
class MissHandlerPlugin : public Plugin {
 public:
  virtual Task<FetchResult> fetch(FetchRequest req) = 0;
};

// Router plugin interface
class RouterPlugin : public Plugin {
 public:
  virtual std::vector<Endpoint> route(const TrackName& track) = 0;
};
```

## Module Loader

```cpp
class ModuleLoader {
 public:
  // Load a plugin from a shared object file, registered by name
  // Returns nullptr on failure, logs error
  template <typename T>
  std::shared_ptr<T> load(const std::string& name, const std::string& path);

  // Get a previously loaded plugin by name
  template <typename T>
  std::shared_ptr<T> get(const std::string& name);

  // Get all loaded plugins of a type
  template <typename T>
  std::vector<std::shared_ptr<T>> getAll();

  // Unload all plugins (call before shutdown)
  void unloadAll();

 private:
  struct LoadedModule {
    void* handle;           // dlopen handle
    std::string path;
    std::string name;       // config-specified name
    std::shared_ptr<Plugin> instance;
  };
  std::vector<LoadedModule> modules_;
};
```

Multiple plugins of the same type can be loaded. Config determines which to use:

```cpp
// Load multiple auth plugins
loader.load<AuthPlugin>("ldap", "/usr/lib/relay/auth_ldap.so");
loader.load<AuthPlugin>("jwt", "/usr/lib/relay/auth_jwt.so");
loader.load<AuthPlugin>("anonymous", "/usr/lib/relay/auth_anon.so");

// Config specifies which to use per scenario
// e.g., "internal" namespace uses ldap, "public" uses jwt
auto plugin = loader.get<AuthPlugin>(config.authPluginFor(request.namespace));
```

### Factory Function Convention

Each plugin `.so` exports a C factory function. The function names include the plugin type for version checking per-type:

```cpp
// In an auth plugin .so:
extern "C" {
  AuthPlugin* create_auth_plugin(PluginHost* host);
  void destroy_auth_plugin(AuthPlugin*);
  uint32_t auth_plugin_api_version();  // API version for AuthPlugin interface
}

// In a miss handler plugin .so:
extern "C" {
  MissHandlerPlugin* create_miss_handler_plugin(PluginHost* host);
  void destroy_miss_handler_plugin(MissHandlerPlugin*);
  uint32_t miss_handler_plugin_api_version();  // API version for MissHandlerPlugin interface
}
```

Each plugin type has its own API version that increments independently when that interface changes.

### Loading Implementation

Each plugin type registers its symbol names and expected API version:

```cpp
template <typename T>
struct PluginTraits;

template <>
struct PluginTraits<AuthPlugin> {
  static constexpr const char* createSym = "create_auth_plugin";
  static constexpr const char* destroySym = "destroy_auth_plugin";
  static constexpr const char* versionSym = "auth_plugin_api_version";
  static constexpr uint32_t apiVersion = 1;
};

template <>
struct PluginTraits<MissHandlerPlugin> {
  static constexpr const char* createSym = "create_miss_handler_plugin";
  static constexpr const char* destroySym = "destroy_miss_handler_plugin";
  static constexpr const char* versionSym = "miss_handler_plugin_api_version";
  static constexpr uint32_t apiVersion = 1;
};
```

```cpp
template <typename T>
std::shared_ptr<T> ModuleLoader::load(
    const std::string& name,
    const std::string& path,
    PluginHost* host) {
  using Traits = PluginTraits<T>;

  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    LOG(ERROR) << "Failed to load " << path << ": " << dlerror();
    return nullptr;
  }

  // Check API version for this plugin type
  auto getVersion = (uint32_t(*)())dlsym(handle, Traits::versionSym);
  if (!getVersion || getVersion() != Traits::apiVersion) {
    LOG(ERROR) << "API version mismatch for " << path
               << " (expected " << Traits::apiVersion << ")";
    dlclose(handle);
    return nullptr;
  }

  // Get type-specific factory functions
  auto create = (T*(*)(PluginHost*))dlsym(handle, Traits::createSym);
  auto destroy = (void(*)(T*))dlsym(handle, Traits::destroySym);
  if (!create || !destroy) {
    LOG(ERROR) << "Missing factory functions in " << path;
    dlclose(handle);
    return nullptr;
  }

  T* instance = create(host);
  if (!instance) {
    LOG(ERROR) << Traits::createSym << " returned null for " << path;
    dlclose(handle);
    return nullptr;
  }

  // Wrap in shared_ptr with custom deleter
  auto ptr = std::shared_ptr<T>(instance, [destroy, handle](T* p) {
    destroy(p);
    dlclose(handle);
  });

  modules_.push_back({handle, path, name, ptr});
  LOG(INFO) << "Loaded plugin '" << name << "': " << ptr->name();
  return ptr;
}
```

## Example: Auth Plugin Usage

### Plugin Implementation (auth_ldap.so)

```cpp
// auth_ldap.cpp - compiled to auth_ldap.so

class LdapAuthPlugin : public AuthPlugin {
 public:
  explicit LdapAuthPlugin(PluginHost* host) {
    const char* server = host->getConfigItem(host, "server");
    const char* port = host->getConfigItem(host, "port");
    // Initialize LDAP connection...
  }

  const char* name() const override { return "ldap-auth"; }
  uint32_t version() const override { return 1; }

  AuthResult authenticate(
      const std::string& token,
      const std::string& resource) override {
    // Validate token against LDAP...
    if (ldapValidate(token)) {
      return {true, "OK"};
    }
    return {false, "Invalid credentials"};
  }

 private:
  // LDAP connection state...
};

extern "C" {
  AuthPlugin* create_auth_plugin(PluginHost* host) {
    return new LdapAuthPlugin(host);
  }
  void destroy_auth_plugin(AuthPlugin* p) { delete p; }
  uint32_t auth_plugin_api_version() { return 1; }
}
```

### Relay Server Usage

```cpp
class RelayServer {
 public:
  RelayServer(const Config& config) {
    // Load all configured auth plugins
    for (const auto& [name, path] : config.authPlugins) {
      if (!moduleLoader_.load<AuthPlugin>(name, path)) {
        throw std::runtime_error("Failed to load auth plugin: " + name);
      }
    }

    // Load all configured miss handlers
    for (const auto& [name, path] : config.missHandlers) {
      moduleLoader_.load<MissHandlerPlugin>(name, path);
    }
  }

  bool handleConnection(const ConnectionRequest& req) {
    // Config determines which auth plugin for this scenario
    auto pluginName = config_.authPluginFor(req.namespace);
    auto plugin = moduleLoader_.get<AuthPlugin>(pluginName);

    auto result = plugin->authenticate(req.token, req.resource);
    if (!result.allowed) {
      LOG(WARNING) << "Auth failed: " << result.reason;
      return false;
    }
    return true;
  }

 private:
  ModuleLoader moduleLoader_;
  Config config_;
};
```

### Configuration

```yaml
# relay_config.yaml
plugins:
  auth:
    ldap: /usr/lib/relay/auth_ldap.so
    jwt: /usr/lib/relay/auth_jwt.so
    anonymous: /usr/lib/relay/auth_anon.so
  miss_handler:
    redis: /usr/lib/relay/cache_redis.so
    memory: /usr/lib/relay/cache_memory.so
  router:
    consul: /usr/lib/relay/router_consul.so

# Which plugin to use per scenario
auth_rules:
  - namespace: "internal/*"
    plugin: ldap
  - namespace: "public/*"
    plugin: jwt
  - namespace: "test/*"
    plugin: anonymous
```

## Plugin Discovery

Optional: scan directories for plugins:

```cpp
std::vector<std::shared_ptr<AuthPlugin>> ModuleLoader::loadAll<AuthPlugin>(
    const std::string& dir) {
  std::vector<std::shared_ptr<AuthPlugin>> result;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".so") {
      if (auto plugin = load<AuthPlugin>(entry.path())) {
        result.push_back(plugin);
      }
    }
  }
  return result;
}
```

## Error Handling

- `dlopen` failures: log `dlerror()`, return nullptr
- Missing symbols: log which symbol, return nullptr
- Version mismatch: log expected vs actual, return nullptr
- Type mismatch: `dynamic_cast` fails, cleanup and return nullptr
- Plugin init failure: factory returns nullptr, cleanup handle

## Thread Safety

- `ModuleLoader::load()` is not thread-safe - call during init
- Loaded plugins should be thread-safe for concurrent use
- `unloadAll()` must be called after all plugin usage stops

## Design Decisions

### No Hot Reload

Plugins are loaded at startup and remain loaded for the lifetime of the server. There is no support for unloading or reloading plugins at runtime. To change plugins, restart the server.

### No Plugin Dependencies

Plugins cannot depend on other plugins. Each plugin is self-contained and interacts only with the relay server through its defined interface.

### Configuration

Plugins receive configuration via a host-provided API. The factory function receives a `PluginHost` pointer that provides config access:

```cpp
// Provided by the relay, passed to plugins
struct PluginHost {
  // Host API version - plugins should check this
  uint32_t version;

  // Get a config value by key (returns nullptr if not found)
  // Returned string is valid for the lifetime of the plugin
  const char* (*getConfigItem)(PluginHost* host, const char* key);

  // Opaque host context
  void* context;

  // Future versions can add new function pointers here
  // Plugins check `version` before calling newer functions
};

// Current host API version
#define PLUGIN_HOST_API_VERSION 1

extern "C" {
  AuthPlugin* create_auth_plugin(PluginHost* host);
  void destroy_auth_plugin(AuthPlugin*);
  uint32_t auth_plugin_api_version();
}
```

Plugins can check the host version and use features conditionally:

```cpp
LdapAuthPlugin::LdapAuthPlugin(PluginHost* host) {
  if (host->version < 1) {
    // Handle old host or fail
  }
  const char* server = host->getConfigItem(host, "server");
  // ...

  // If host->version >= 2, could use newer host functions
}
```

```yaml
# relay_config.yaml
plugins:
  auth:
    ldap:
      path: /usr/lib/relay/auth_ldap.so
      config:
        server: ldap.example.com
        port: "389"
        base_dn: "dc=example,dc=com"
    jwt:
      path: /usr/lib/relay/auth_jwt.so
      config:
        public_key_path: /etc/relay/jwt_public.pem
        issuer: "auth.example.com"
```

### Platform Support

This plugin system uses `dlopen`/`dlsym` and works on Linux. Windows and macOS builds will not support plugins initially.

### C++ vs Pure C Interface

The current design uses `extern "C"` for factory functions but returns C++ class pointers. This creates ABI coupling—the plugin and relay must use compatible compilers, standard libraries, and vtable layouts.

**Current approach (C++ returns)**: Simpler code, works when plugins are built with the same toolchain as the relay.

**Alternative (pure C interface)**: More portable across compilers. Would use opaque handles and C function pointers:

```cpp
extern "C" {
  typedef void* auth_plugin_handle;

  auth_plugin_handle create_auth_plugin(PluginHost* host);
  void destroy_auth_plugin(auth_plugin_handle);
  uint32_t auth_plugin_api_version();

  int auth_plugin_authenticate(
      auth_plugin_handle h,
      const char* token,
      const char* resource,
      char* reason_out,
      size_t reason_size);
}
```

**Decision**: For moxygen, C++ returns are acceptable since plugins will be built with the same toolchain. If third-party plugins with arbitrary compilers become a requirement, a pure C adapter layer can be added later.

## Interface Versioning

When a plugin interface changes, the API version increments. There are two strategies for handling version transitions:

### Strategy 1: Separate Plugin Files

Deploy different `.so` files for different API versions:

```
/usr/lib/relay/auth_ldap_v1.so  # implements AuthPlugin API v1
/usr/lib/relay/auth_ldap_v2.so  # implements AuthPlugin API v2
```

This is simple and explicit. The relay server loads whichever version matches its expected API.

### Strategy 2: Multi-Version Plugin

A single `.so` can export multiple factory functions for different API versions:

```cpp
// auth_ldap.cpp - supports both v1 and v2

class LdapAuthPluginV1 : public AuthPluginV1 {
  // v1 implementation...
};

class LdapAuthPluginV2 : public AuthPluginV2 {
  // v2 implementation, possibly wrapping v1
};

extern "C" {
  // V1 exports
  AuthPluginV1* create_auth_plugin_v1(PluginHost* host);
  void destroy_auth_plugin_v1(AuthPluginV1*);
  uint32_t auth_plugin_v1_api_version() { return 1; }

  // V2 exports
  AuthPluginV2* create_auth_plugin_v2(PluginHost* host);
  void destroy_auth_plugin_v2(AuthPluginV2*);
  uint32_t auth_plugin_v2_api_version() { return 2; }
}
```

The loader would try to load the highest compatible version:

```cpp
template <>
struct PluginTraits<AuthPlugin> {
  static constexpr uint32_t apiVersion = 2;  // current version
  static constexpr uint32_t minApiVersion = 1;  // oldest supported

  static const char* createSym(uint32_t v) {
    return v == 2 ? "create_auth_plugin_v2" : "create_auth_plugin_v1";
  }
  // ...
};
```

### Recommendation

**Use Strategy 1 (separate files)** for simplicity. Multi-version plugins add complexity and the benefits are marginal—plugin authors can maintain separate branches/builds for different API versions. The relay config explicitly specifies which plugin file to load, making version management clear and predictable.

## Verification

- Unit test: mock plugin .so, verify load/unload lifecycle
- Unit test: version mismatch handling
- Unit test: missing symbol handling
- Integration test: real .so with simple auth logic
