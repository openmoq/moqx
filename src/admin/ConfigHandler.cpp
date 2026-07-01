/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/ConfigHandler.h"

#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminServer.h"
#include "admin/JsonWriter.h"
#include "config/ConfigSerializer.h"

namespace openmoq::moqx::admin {

namespace {

// Renders the format-agnostic config walk (config::serializeConfig) as JSON.
// An empty key marks an array element, which JsonWriter emits without a key.
class JsonConfigSink : public config::ConfigSink {
  JsonWriter& w_;

public:
  explicit JsonConfigSink(JsonWriter& w) : w_(w) {}

  void beginObject(std::string_view key) override {
    if (!key.empty()) {
      w_.key(key);
    }
    w_.beginObject();
  }
  void endObject() override { w_.endObject(); }
  void beginArray(std::string_view key) override {
    if (!key.empty()) {
      w_.key(key);
    }
    w_.beginArray();
  }
  void endArray() override { w_.endArray(); }

  void stringField(std::string_view key, std::string_view value) override {
    if (key.empty()) {
      w_.strVal(value);
    } else {
      w_.field(key, value);
    }
  }
  void boolField(std::string_view key, bool value) override {
    if (key.empty()) {
      w_.boolVal(value);
    } else {
      w_.field(key, value);
    }
  }
  void intField(std::string_view key, int64_t value) override {
    if (key.empty()) {
      w_.intVal(value);
    } else {
      w_.field(key, value);
    }
  }
  void uintField(std::string_view key, uint64_t value) override {
    if (key.empty()) {
      w_.uintVal(value);
    } else {
      w_.field(key, value);
    }
  }
  void doubleField(std::string_view key, double value) override {
    if (key.empty()) {
      w_.doubleVal(value);
    } else {
      w_.field(key, value);
    }
  }
  void nullField(std::string_view key) override {
    if (!key.empty()) {
      w_.key(key);
    }
    w_.nullVal();
  }
};

std::unique_ptr<folly::IOBuf> buildConfigBody(const config::Config& cfg) {
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender app{&queue, 4096};
  JsonWriter w{app};
  JsonConfigSink sink{w};
  config::serializeConfig(cfg, sink);
  app.write(static_cast<uint8_t>('\n'));
  return queue.move();
}

} // namespace

void registerConfigRoute(AdminServer& adminServer, std::shared_ptr<const config::Config> config) {
  adminServer.addRoute(
      "GET",
      "/config",
      [config = std::move(config
       )](auto /*req*/, auto /*body*/, auto* downstream, auto /*cancelToken*/
      ) {
        proxygen::ResponseBuilder(downstream)
            .status(200, proxygen::HTTPMessage::getDefaultReason(200))
            .header("Content-Type", "application/json")
            .body(buildConfigBody(*config))
            .sendWithEOM();
      }
  );
}

} // namespace openmoq::moqx::admin
