/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/ConfigDiff.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <folly/String.h>

#include "config/ServiceRoutes.h"

namespace openmoq::moqx::config {

namespace {

template <typename T>
void compareGlobal(
    std::vector<IgnoredChange>& ignored,
    std::string path,
    const T& current,
    const T& candidate
) {
  if (!(current == candidate)) {
    ignored.push_back({std::move(path), "requires restart"});
  }
}

void diffGlobals(
    const ResolvedConfig& currentResolved,
    const ResolvedConfig& candidate,
    std::vector<IgnoredChange>& ignored
) {
  const Config& current = currentResolved.config;
  const Config& candidateConfig = candidate.config;
  compareGlobal(ignored, "listeners", current.listeners, candidateConfig.listeners);
  compareGlobal(ignored, "admin", current.admin, candidateConfig.admin);
  compareGlobal(ignored, "logging", current.logging, candidateConfig.logging);
  compareGlobal(ignored, "threads", current.threads, candidateConfig.threads);
  compareGlobal(
      ignored,
      "use_relay_thread",
      current.useRelayThread,
      candidateConfig.useRelayThread
  );
  compareGlobal(
      ignored,
      "use_local_forwarders",
      current.useLocalForwarders,
      candidateConfig.useLocalForwarders
  );
  compareGlobal(
      ignored,
      "mvfst_bpf_steering",
      current.mvfstBpfSteering,
      candidateConfig.mvfstBpfSteering
  );

  // A generated ID is random per resolve, so it only compares equal when both sides were
  // generated. An explicit ID that was removed (or newly set) is a real change.
  if (!(currentResolved.relayIdGenerated && candidate.relayIdGenerated)) {
    compareGlobal(ignored, "relay_id", current.relayID, candidateConfig.relayID);
  }

  // Same for hop_id, but keep comparing the rest of the cluster config.
  auto candidateCluster = candidateConfig.cluster;
  if (candidateCluster && current.cluster && currentResolved.hopIdGenerated &&
      candidate.hopIdGenerated) {
    candidateCluster->hopID = current.cluster->hopID;
  }
  compareGlobal(ignored, "cluster", current.cluster, candidateCluster);
}

std::vector<std::string>
sortedServiceNames(const folly::F14FastMap<std::string, ServiceConfig>& services) {
  std::vector<std::string> names;
  names.reserve(services.size());
  for (const auto& [name, _] : services) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

// Candidate order, minus listeners that are not running.
std::vector<std::string> bindingsOnCurrentListeners(
    const std::vector<std::string>& candidateListeners,
    const std::unordered_set<std::string>& currentListenerNames
) {
  std::vector<std::string> kept;
  for (const auto& listener : candidateListeners) {
    if (currentListenerNames.contains(listener)) {
      kept.push_back(listener);
    }
  }
  return kept;
}

void diffExistingServices(
    const Config& current,
    const Config& candidate,
    std::vector<IgnoredChange>& ignored
) {
  std::unordered_set<std::string> currentListenerNames;
  for (const auto& listener : current.listeners) {
    currentListenerNames.insert(listener.name);
  }
  for (const auto& name : sortedServiceNames(current.services)) {
    const auto path = "services." + name;
    auto it = candidate.services.find(name);
    if (it == candidate.services.end()) {
      ignored.push_back({path, "removed from the file; the service keeps running"});
      continue;
    }
    ServiceConfig candidateService = it->second;
    candidateService.listeners =
        bindingsOnCurrentListeners(candidateService.listeners, currentListenerNames);
    const ServiceConfig& currentService = current.services.at(name);
    if (!(candidateService.listeners == currentService.listeners)) {
      ignored.push_back(
          {path + ".listeners", "bindings differ from the running service; restart required"}
      );
    }
    candidateService.listeners = currentService.listeners;
    if (!(currentService == candidateService)) {
      ignored.push_back({path, "changed; restart required"});
    }
  }
}

// Sinks come from the running logging config, since logging changes need a restart.
const AccessLogSinkSelectionConfig* findMissingSink(
    const AccessLogServiceConfig& accessLog,
    const std::optional<LoggingConfig>& logging
) {
  for (const auto& selection : accessLog.sinks) {
    const bool defined = logging && logging->access &&
                         std::ranges::any_of(logging->access->sinks, [&](const auto& sink) {
                           return sink.name == selection.name;
                         });
    if (!defined)
      return &selection;
  }
  return nullptr;
}

void planNewServices(const Config& current, const Config& candidate, ReloadPlan& plan) {
  std::unordered_set<std::string> currentListenerNames;
  std::set<std::string> picoListeners;
  for (const auto& listener : current.listeners) {
    currentListenerNames.insert(listener.name);
    if (listener.quicStack == QuicStack::Picoquic)
      picoListeners.insert(listener.name);
  }

  RuleOwners owners;
  std::map<std::string, std::set<std::string>> picoTable;
  for (const auto& [name, svc] : current.services) {
    for (const auto& entry : svc.match) {
      const std::string key = ruleKey(entry);
      claimRule(owners, name, svc.listeners, key); // ignore return value
      if (const auto* p = std::get_if<ServiceConfig::MatchEntry::ExactPath>(&entry.path)) {
        for (const auto& l : svc.listeners) {
          if (picoListeners.contains(l))
            picoTable[l].insert(p->value);
        }
      }
    }
  }

  for (const auto& name : sortedServiceNames(candidate.services)) {
    if (current.services.contains(name))
      continue;
    auto svc = candidate.services.at(name);
    const auto path = "services." + name;
    auto kept = bindingsOnCurrentListeners(svc.listeners, currentListenerNames);
    for (const auto& listener : svc.listeners) {
      if (!currentListenerNames.contains(listener)) {
        plan.ignored.push_back(
            {path + ".listeners[" + listener + "]", "listener is not running; restart required"}
        );
      }
    }
    if (kept.empty()) {
      plan.ignored.push_back({path, "no running listener to bind to"});
      continue;
    }
    std::vector<std::string> finalListeners;
    for (const auto& listener : kept) {
      bool ok = true;
      if (picoListeners.contains(listener)) {
        bool hasExactPath = false;
        for (const auto& entry : svc.match) {
          const auto* p = std::get_if<ServiceConfig::MatchEntry::ExactPath>(&entry.path);
          if (!p)
            continue;
          hasExactPath = true;
          if (!picoTable[listener].contains(p->value)) {
            ok = false;
            plan.ignored.push_back(
                {path + ".listeners[" + listener + "]",
                 "exact path not registered on this picoquic listener; restart required"}
            );
            break;
          }
        }
        // Prefix paths never route on picoquic, so a service with only those would bind to nothing.
        if (ok && !hasExactPath) {
          ok = false;
          plan.ignored.push_back(
              {path + ".listeners[" + listener + "]",
               "prefix paths cannot route on picoquic listeners; restart required"}
          );
        }
      }
      if (ok)
        finalListeners.push_back(listener);
    }
    if (finalListeners.empty()) {
      plan.ignored.push_back({path, "no running listener to bind to after filtering"});
      continue;
    }

    if (const auto* missing = findMissingSink(svc.accessLog, plan.effective.logging)) {
      plan.ignored.push_back(
          {path,
           "access log sink '" + missing->name +
               "' is not in the running logging config; restart required"}
      );
      continue;
    }

    RuleOwners trial = owners;
    bool conflicted = false;
    for (const auto& entry : svc.match) {
      for (const auto& [other, shared] : claimRule(trial, name, finalListeners, ruleKey(entry))) {
        conflicted = true;
        plan.ignored.push_back(
            {path,
             "route conflicts with service '" + other + "' on listener(s) '" +
                 folly::join("', '", shared) + "'; restart required"}
        );
      }
    }
    if (conflicted)
      continue;
    owners = std::move(trial);

    svc.listeners = std::move(finalListeners);
    plan.addedServices.push_back(name);
    plan.effective.services.emplace(name, std::move(svc));
  }
}

} // namespace

ReloadPlan planReload(const ResolvedConfig& current, const ResolvedConfig& candidate) {

  ReloadPlan plan{.effective = current.config};
  diffGlobals(current, candidate, plan.ignored);
  diffExistingServices(current.config, candidate.config, plan.ignored);
  planNewServices(current.config, candidate.config, plan);
  return plan;
}

} // namespace openmoq::moqx::config
