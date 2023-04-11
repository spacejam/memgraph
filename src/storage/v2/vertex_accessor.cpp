// Copyright 2023 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include "storage/v2/vertex_accessor.hpp"

#include "storage/v2/inmemory/edge_accessor.hpp"
#include "storage/v2/inmemory/vertex_accessor.hpp"

namespace memgraph::storage {

std::unique_ptr<VertexAccessor> VertexAccessor::Create(Vertex *vertex, Transaction *transaction, Indices *indices,
                                                       Constraints *constraints, Config::Items config, View view) {
  return InMemoryVertexAccessor::Create(vertex, transaction, indices, constraints, config, view);
}

Result<std::vector<EdgeAccessor>> VertexAccessor::InEdges(View view) const { return InEdges(view, {}, nullptr); }
Result<std::vector<EdgeAccessor>> VertexAccessor::OutEdges(View view) const { return OutEdges(view, {}, nullptr); }

}  // namespace memgraph::storage
