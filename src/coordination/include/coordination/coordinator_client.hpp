// Copyright 2024 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#ifdef MG_ENTERPRISE

#include "coordination/coordinator_communication_config.hpp"
#include "replication_coordination_glue/common.hpp"
#include "replication_coordination_glue/role.hpp"
#include "rpc/client.hpp"
#include "rpc_errors.hpp"
#include "utils/result.hpp"
#include "utils/scheduler.hpp"
#include "utils/uuid.hpp"

namespace memgraph::coordination {

class CoordinatorInstance;
using HealthCheckClientCallback = std::function<void(CoordinatorInstance *, std::string_view)>;
using ReplicationClientsInfo = std::vector<ReplicationClientInfo>;

class ReplicationInstanceClient {
 public:
  explicit ReplicationInstanceClient(CoordinatorInstance *coord_instance, CoordinatorToReplicaConfig config,
                                     HealthCheckClientCallback succ_cb, HealthCheckClientCallback fail_cb);

  ~ReplicationInstanceClient() = default;

  ReplicationInstanceClient(ReplicationInstanceClient &) = delete;
  ReplicationInstanceClient &operator=(ReplicationInstanceClient const &) = delete;

  ReplicationInstanceClient(ReplicationInstanceClient &&) noexcept = delete;
  ReplicationInstanceClient &operator=(ReplicationInstanceClient &&) noexcept = delete;

  void StartFrequentCheck();
  void StopFrequentCheck();
  void PauseFrequentCheck();
  void ResumeFrequentCheck();

  auto InstanceName() const -> std::string;
  auto CoordinatorSocketAddress() const -> std::string;
  auto ReplicationSocketAddress() const -> std::string;

  [[nodiscard]] auto DemoteToReplica() const -> bool;

  auto SendPromoteReplicaToMainRpc(utils::UUID const &uuid, ReplicationClientsInfo replication_clients_info) const
      -> bool;

  auto SendSwapMainUUIDRpc(utils::UUID const &uuid) const -> bool;

  auto SendUnregisterReplicaRpc(std::string_view instance_name) const -> bool;

  auto SendEnableWritingOnMainRpc() const -> bool;

  auto SendGetInstanceUUIDRpc() const -> memgraph::utils::BasicResult<GetInstanceUUIDError, std::optional<utils::UUID>>;

  auto ReplicationClientInfo() const -> ReplicationClientInfo;

  auto SendFrequentHeartbeat() const -> bool;

  auto SendGetInstanceTimestampsRpc() const
      -> utils::BasicResult<GetInstanceUUIDError, replication_coordination_glue::DatabaseHistories>;

  auto RpcClient() -> rpc::Client & { return rpc_client_; }

  auto InstanceDownTimeoutSec() const -> std::chrono::seconds;

  auto InstanceGetUUIDFrequencySec() const -> std::chrono::seconds;

  friend bool operator==(ReplicationInstanceClient const &first, ReplicationInstanceClient const &second) {
    return first.config_ == second.config_;
  }

 private:
  utils::Scheduler instance_checker_;

  communication::ClientContext rpc_context_;
  mutable rpc::Client rpc_client_;

  CoordinatorToReplicaConfig config_;
  CoordinatorInstance *coord_instance_;
  // The reason why we have HealthCheckClientCallback is because we need to acquire lock
  // before we do correct function call (main or replica), as otherwise we can enter REPLICA callback
  // but right before instance was promoted to MAIN
  HealthCheckClientCallback succ_cb_;
  HealthCheckClientCallback fail_cb_;
};

}  // namespace memgraph::coordination
#endif
