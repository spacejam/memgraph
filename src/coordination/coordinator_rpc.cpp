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

#ifdef MG_ENTERPRISE

#include "coordination/coordinator_rpc.hpp"

#include "coordination/coordinator_slk.hpp"
#include "slk/serialization.hpp"

namespace memgraph {

namespace coordination {

void FailoverReq::Save(const FailoverReq &self, memgraph::slk::Builder *builder) { memgraph::slk::Save(self, builder); }

void FailoverReq::Load(FailoverReq *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }

void FailoverRes::Save(const FailoverRes &self, memgraph::slk::Builder *builder) { memgraph::slk::Save(self, builder); }

void FailoverRes::Load(FailoverRes *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }

}  // namespace coordination

constexpr utils::TypeInfo coordination::FailoverReq::kType{utils::TypeId::COORD_FAILOVER_REQ, "CoordFailoverReq",
                                                           nullptr};

constexpr utils::TypeInfo coordination::FailoverRes::kType{utils::TypeId::COORD_FAILOVER_RES, "CoordFailoverRes",
                                                           nullptr};

namespace slk {

void Save(const memgraph::coordination::FailoverRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.success, builder);
}

void Load(memgraph::coordination::FailoverRes *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->success, reader);
}

void Save(const memgraph::coordination::FailoverReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.replication_clients_info, builder);
}

void Load(memgraph::coordination::FailoverReq *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->replication_clients_info, reader);
}

}  // namespace slk

}  // namespace memgraph

#endif
