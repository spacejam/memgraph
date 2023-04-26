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

#pragma once

#include <optional>

#include "storage/v2/edge.hpp"
#include "storage/v2/edge_accessor.hpp"
#include "storage/v2/edge_ref.hpp"

#include "storage/v2/config.hpp"
#include "storage/v2/id_types.hpp"
#include "storage/v2/result.hpp"
#include "storage/v2/transaction.hpp"
#include "storage/v2/view.hpp"

namespace memgraph::storage {

struct Vertex;
class VertexAccessor;
struct Indices;
struct Constraints;

class InMemoryEdgeAccessor final : public EdgeAccessor {
 private:
  friend class InMemoryStorage;

 public:
  InMemoryEdgeAccessor(EdgeRef edge, EdgeTypeId edge_type, Vertex *from_vertex, Vertex *to_vertex,
                       Transaction *transaction, Indices *indices, Constraints *constraints, Config::Items config,
                       bool for_deleted = false)
      : EdgeAccessor(edge_type, transaction, config, for_deleted),
        edge_(edge),
        from_vertex_(from_vertex),
        to_vertex_(to_vertex),
        indices_(indices),
        constraints_(constraints) {}

  /// @return true if the object is visible from the current transaction
  bool IsVisible(View view) const override;

  std::unique_ptr<VertexAccessor> FromVertex() const override;

  std::unique_ptr<VertexAccessor> ToVertex() const override;

  EdgeTypeId EdgeType() const { return edge_type_; }

  /// Set a property value and return the old value.
  /// @throw std::bad_alloc
  Result<storage::PropertyValue> SetProperty(PropertyId property, const PropertyValue &value) override;

  /// Set property values only if property store is empty. Returns `true` if successully set all values,
  /// `false` otherwise.
  /// @throw std::bad_alloc
  Result<bool> InitProperties(const std::map<storage::PropertyId, storage::PropertyValue> &properties) override;

  /// Remove all properties and return old values for each removed property.
  /// @throw std::bad_alloc
  Result<std::map<PropertyId, PropertyValue>> ClearProperties() override;

  /// @throw std::bad_alloc
  Result<PropertyValue> GetProperty(PropertyId property, View view) const override;

  /// @throw std::bad_alloc
  Result<std::map<PropertyId, PropertyValue>> Properties(View view) const override;

  storage::Gid Gid() const noexcept override {
    if (config_.properties_on_edges) {
      return edge_.ptr->gid;
    } else {
      return edge_.gid;
    }
  }

  bool IsCycle() const override { return from_vertex_ == to_vertex_; }

  std::unique_ptr<EdgeAccessor> Copy() const override { return std::make_unique<InMemoryEdgeAccessor>(*this); }

  bool operator==(const EdgeAccessor &other) const noexcept override {
    const auto *otherEdge = dynamic_cast<const InMemoryEdgeAccessor *>(&other);
    if (otherEdge == nullptr) return false;
    return edge_ == otherEdge->edge_ && transaction_ == otherEdge->transaction_;
  }
  bool operator!=(const EdgeAccessor &other) const noexcept { return !(*this == other); }

 private:
  EdgeRef edge_;
  Vertex *from_vertex_;
  Vertex *to_vertex_;
  Indices *indices_;
  Constraints *constraints_;
};

}  // namespace memgraph::storage