/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// A plugin's `config` subtree, kept as YAML until the plugin's typed parse.
// - A YAML scalar keeps its text: `id: 1` or `secret: 007` read as strings
//   when the plugin's field is a string. rfl::Generic would type them first.
// - The schema and any non-YAML write treat it as a free-form value.

#include <rfl.hpp>
#include <rfl/yaml.hpp>
#include <yaml-cpp/yaml.h>

#include <string>

namespace openmoq::moqx::config {

class PluginConfigNode {
public:
  using ReflectionType = rfl::Generic;

  PluginConfigNode() : node_(YAML::NodeType::Map) {}
  explicit PluginConfigNode(YAML::Node node) : node_(YAML::Clone(node)) {}
  // For readers other than YAML.
  PluginConfigNode(const rfl::Generic& generic) // NOLINT(google-explicit-constructor)
      : node_(YAML::Load(rfl::yaml::write(generic))) {}

  static PluginConfigNode from_yaml_obj(const rfl::yaml::Reader::InputVarType& var) {
    return PluginConfigNode(var.node_);
  }

  rfl::Generic reflection() const {
    auto generic = rfl::yaml::read<rfl::Generic>(YAML::Dump(node_));
    return generic ? std::move(*generic) : rfl::Generic();
  }

  const YAML::Node& node() const { return node_; }

private:
  YAML::Node node_;
};

} // namespace openmoq::moqx::config
