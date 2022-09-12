// Copyright 2022 Memgraph Ltd.
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

#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "coordinator/hybrid_logical_clock.hpp"
#include "storage/v3/id_types.hpp"
#include "storage/v3/property_value.hpp"

using memgraph::coordinator::Hlc;
using memgraph::storage::v3::LabelId;

struct Value;

struct Label {
  LabelId id;
};

// TODO(kostasrim) update this with CompoundKey, same for the rest of the file.
using PrimaryKey = std::vector<Value>;
using VertexId = std::pair<Label, PrimaryKey>;
using Gid = size_t;
using PropertyId = memgraph::storage::v3::PropertyId;

struct EdgeType {
  uint64_t id;
};

struct EdgeId {
  VertexId src;
  VertexId dst;
  // std::vector<Value> src;
  // std::vector<Value> dst;
  Gid gid;
};

struct Edge {
  EdgeId id;
  EdgeType type;
};

struct Vertex {
  VertexId id;
  std::vector<Label> labels;
};

struct PathPart {
  Vertex dst;
  Gid edge;
};

struct Path {
  Vertex src;
  std::vector<PathPart> parts;
};

struct Null {};

struct Value {
  Value() : type(Type::NILL), null_v{} {}

  explicit Value(const bool val) : type(Type::BOOL), bool_v(val) {}
  explicit Value(const int64_t val) : type(Type::INT64), int_v(val) {}
  explicit Value(const double val) : type(Type::DOUBLE), double_v(val) {}

  explicit Value(const Vertex val) : type(Type::VERTEX), vertex_v(val) {}

  explicit Value(const std::string &val) : type(Type::STRING) { new (&string_v) std::string(val); }
  explicit Value(const char *val) : type(Type::STRING) { new (&string_v) std::string(val); }

  explicit Value(const std::vector<Value> &val) : type(Type::LIST) { new (&list_v) std::vector<Value>(val); }

  explicit Value(const std::map<std::string, Value> &val) : type(Type::MAP) {
    new (&map_v) std::map<std::string, Value>(val);
  }

  explicit Value(std::string &&val) noexcept : type(Type::STRING) { new (&string_v) std::string(std::move(val)); }

  explicit Value(std::vector<Value> &&val) noexcept : type(Type::LIST) {
    new (&list_v) std::vector<Value>(std::move(val));
  }
  explicit Value(std::map<std::string, Value> &&val) noexcept : type(Type::MAP) {
    new (&map_v) std::map<std::string, Value>(std::move(val));
  }

  ~Value() { DestroyValue(); }

  void DestroyValue() noexcept {
    switch (type) {
      // destructor for primitive types does nothing
      case Type::NILL:
      case Type::BOOL:
      case Type::INT64:
      case Type::DOUBLE:
        return;

      // destructor for non primitive types since we used placement new
      case Type::STRING:
        std::destroy_at(&string_v);
        return;
      case Type::LIST:
        std::destroy_at(&list_v);
        return;
      case Type::MAP:
        std::destroy_at(&map_v);
        return;

      // are these needed to be defined?
      case Type::VERTEX:
        std::destroy_at(&vertex_v);
        return;
      case Type::PATH:
        std::destroy_at(&path_v);
        return;
      case Type::EDGE:
        std::destroy_at(&edge_v);
      default:
        return;
    }
  }

  Value(const Value &other) : type(other.type) {
    switch (other.type) {
      case Type::NILL:
        return;
      case Type::BOOL:
        this->bool_v = other.bool_v;
        return;
      case Type::INT64:
        this->int_v = other.int_v;
        return;
      case Type::DOUBLE:
        this->double_v = other.double_v;
        return;
      case Type::STRING:
        new (&string_v) std::string(other.string_v);
        return;
      case Type::LIST:
        new (&list_v) std::vector<Value>(other.list_v);
        return;
      case Type::MAP:
        new (&map_v) std::map<std::string, Value>(other.map_v);
        return;
      case Type::VERTEX:
        new (&vertex_v) Vertex(other.vertex_v);
        return;
      case Type::EDGE:
        new (&edge_v) Edge(other.edge_v);
        return;
      case Type::PATH:
        new (&path_v) Path(other.path_v);
        return;
    }
  }

  Value(Value &&other) noexcept : type(other.type) {
    switch (other.type) {
      case Type::NILL:
        break;
      case Type::BOOL:
        this->bool_v = other.bool_v;
        break;
      case Type::INT64:
        this->int_v = other.int_v;
        break;
      case Type::DOUBLE:
        this->double_v = other.double_v;
        break;
      case Type::STRING:
        new (&string_v) std::string(std::move(other.string_v));
        break;
      case Type::LIST:
        new (&list_v) std::vector<Value>(std::move(other.list_v));
        break;
      case Type::MAP:
        new (&map_v) std::map<std::string, Value>(std::move(other.map_v));
        break;
      case Type::VERTEX:
        new (&vertex_v) Vertex(std::move(other.vertex_v));
        break;
      case Type::EDGE:
        new (&edge_v) Edge(std::move(other.edge_v));
        break;
      case Type::PATH:
        new (&path_v) Path(std::move(other.path_v));
        break;
    }

    // reset the type of other
    other.DestroyValue();
    other.type = Type::NILL;
  }

  Value &operator=(const Value &other) {
    if (this == &other) return *this;

    DestroyValue();
    type = other.type;

    switch (other.type) {
      case Type::NILL:
        break;
      case Type::BOOL:
        this->bool_v = other.bool_v;
        break;
      case Type::INT64:
        this->int_v = other.int_v;
        break;
      case Type::DOUBLE:
        this->double_v = other.double_v;
        break;
      case Type::STRING:
        new (&string_v) std::string(other.string_v);
        break;
      case Type::LIST:
        new (&list_v) std::vector<Value>(other.list_v);
        break;
      case Type::MAP:
        new (&map_v) std::map<std::string, Value>(other.map_v);
        break;
      case Type::VERTEX:
        new (&vertex_v) Vertex(other.vertex_v);
        break;
      case Type::EDGE:
        new (&edge_v) Edge(other.edge_v);
        break;
      case Type::PATH:
        new (&path_v) Path(other.path_v);
        break;
    }

    return *this;
  }

  Value &operator=(Value &&other) noexcept {
    if (this == &other) return *this;

    DestroyValue();
    type = other.type;

    switch (other.type) {
      case Type::NILL:
        break;
      case Type::BOOL:
        this->bool_v = other.bool_v;
        break;
      case Type::INT64:
        this->int_v = other.int_v;
        break;
      case Type::DOUBLE:
        this->double_v = other.double_v;
        break;
      case Type::STRING:
        new (&string_v) std::string(std::move(other.string_v));
        break;
      case Type::LIST:
        new (&list_v) std::vector<Value>(std::move(other.list_v));
        break;
      case Type::MAP:
        new (&map_v) std::map<std::string, Value>(std::move(other.map_v));
        break;
      case Type::VERTEX:
        new (&vertex_v) Vertex(std::move(other.vertex_v));
        break;
      case Type::EDGE:
        new (&edge_v) Edge(std::move(other.edge_v));
        break;
      case Type::PATH:
        new (&path_v) Path(std::move(other.path_v));
        break;
    }

    // reset the type of other
    other.DestroyValue();
    other.type = Type::NILL;

    return *this;
  }
  enum class Type { NILL, BOOL, INT64, DOUBLE, STRING, LIST, MAP, VERTEX, EDGE, PATH };
  Type type{Type::NILL};
  union {
    Null null_v;
    bool bool_v;
    int64_t int_v;
    double double_v;
    std::string string_v;
    std::vector<Value> list_v;
    std::map<std::string, Value> map_v;
    Vertex vertex_v;
    Edge edge_v;
    Path path_v;
  };
};

// this one
struct ValuesMap {
  std::unordered_map<PropertyId, Value> values_map;
};

struct MappedValues {
  std::vector<ValuesMap> values_map;
};

struct ListedValues {
  std::vector<std::vector<Value>> properties;
};

using Values = std::variant<ListedValues, MappedValues>;

struct Expression {
  std::string expression;
};

struct Filter {
  std::string filter_expression;
};

enum class OrderingDirection { ASCENDING = 1, DESCENDING = 2 };

struct OrderBy {
  Expression expression;
  OrderingDirection direction;
};

enum class StorageView { OLD = 0, NEW = 1 };

struct ScanVerticesRequest {
  Hlc transaction_id;
  // VertexId start_id;
  std::vector<Value> start_id;
  std::optional<std::vector<PropertyId>> props_to_return;
  std::optional<std::vector<std::string>> filter_expressions;
  std::optional<size_t> batch_limit;
  StorageView storage_view;
};

struct ScanResultRow {
  Value vertex;
  // empty is no properties returned
  std::map<PropertyId, Value> props;
};

struct ScanVerticesResponse {
  bool success;
  std::optional<VertexId> next_start_id;
  std::vector<ScanResultRow> results;
};

using VertexOrEdgeIds = std::variant<VertexId, EdgeId>;

struct GetPropertiesRequest {
  Hlc transaction_id;
  VertexOrEdgeIds vertex_or_edge_ids;
  std::vector<PropertyId> property_ids;
  std::vector<Expression> expressions;
  bool only_unique = false;
  std::optional<std::vector<OrderBy>> order_by;
  std::optional<size_t> limit;
  std::optional<Filter> filter;
};

struct GetPropertiesResponse {
  bool success;
  Values values;
};

enum class EdgeDirection : uint8_t { OUT = 1, IN = 2, BOTH = 3 };

struct ExpandOneRequest {
  Hlc transaction_id;
  std::vector<VertexId> src_vertices;
  std::vector<EdgeType> edge_types;
  EdgeDirection direction;
  bool only_unique_neighbor_rows = false;
  //  The empty optional means return all of the properties, while an empty
  //  list means do not return any properties
  //  TODO(antaljanosbenjamin): All of the special values should be communicated through a single vertex object
  //                            after schema is implemented
  //  Special values are accepted:
  //  * __mg__labels
  std::optional<std::vector<PropertyId>> src_vertex_properties;
  //  TODO(antaljanosbenjamin): All of the special values should be communicated through a single vertex object
  //                            after schema is implemented
  //  Special values are accepted:
  //  * __mg__dst_id (Vertex, but without labels)
  //  * __mg__type (binary)
  std::optional<std::vector<PropertyId>> edge_properties;
  //  QUESTION(antaljanosbenjamin): Maybe also add possibility to expressions evaluated on the source vertex?
  //  List of expressions evaluated on edges
  std::vector<Expression> expressions;
  std::optional<std::vector<OrderBy>> order_by;
  std::optional<size_t> limit;
  std::optional<Filter> filter;
};

struct ExpandOneResultRow {
  // NOTE: This struct could be a single Values with columns something like this:
  // src_vertex(Vertex), vertex_prop1(Value), vertex_prop2(Value), edges(list<Value>)
  // where edges might be a list of:
  // 1. list<Value> if only a defined list of edge properties are returned
  // 2. map<binary, Value> if all of the edge properties are returned
  // The drawback of this is currently the key of the map is always interpreted as a string in Value, not as an
  // integer, which should be in case of mapped properties.
  Vertex src_vertex;
  std::optional<Values> src_vertex_properties;
  Values edges;
};

struct ExpandOneResponse {
  std::vector<ExpandOneResultRow> result;
};

// Update related messages
struct UpdateVertexProp {
  VertexId primary_key;  // This will very likely be deleted
  std::vector<Value> vertex;
  std::vector<std::pair<PropertyId, Value>> property_updates;
};

struct UpdateEdgeProp {
  Edge edge;
  std::vector<std::pair<PropertyId, Value>> property_updates;
};

/*
 * Vertices
 */
struct NewVertex {
  std::vector<Label> label_ids;
  PrimaryKey primary_key;
  std::vector<std::pair<PropertyId, Value>> properties;
};

struct CreateVerticesRequest {
  Hlc transaction_id;
  std::vector<NewVertex> new_vertices;
};

struct CreateVerticesResponse {
  bool success;
};

struct DeleteVerticesRequest {
  enum class DeletionType { DELETE, DETACH_DELETE };
  Hlc transaction_id;
  std::vector<std::vector<Value>> primary_keys;
  DeletionType deletion_type;
};

struct DeleteVerticesResponse {
  bool success;
};

struct UpdateVerticesRequest {
  Hlc transaction_id;
  std::vector<UpdateVertexProp> new_properties;
};

struct UpdateVerticesResponse {
  bool success;
};

/*
 * Edges
 */
struct CreateEdgesRequest {
  Hlc transaction_id;
  std::vector<Edge> edges;
};

struct CreateEdgesResponse {
  bool success;
};

struct DeleteEdgesRequest {
  Hlc transaction_id;
  std::vector<Edge> edges;
};

struct DeleteEdgesResponse {
  bool success;
};

struct UpdateEdgesRequest {
  Hlc transaction_id;
  std::vector<UpdateEdgeProp> new_properties;
};

struct UpdateEdgesResponse {
  bool success;
};

using ReadRequests = std::variant<ExpandOneRequest, GetPropertiesRequest, ScanVerticesRequest>;
using ReadResponses = std::variant<ExpandOneResponse, GetPropertiesResponse, ScanVerticesResponse>;

using WriteRequests = std::variant<CreateVerticesRequest, DeleteVerticesRequest, UpdateVerticesRequest,
                                   CreateEdgesRequest, DeleteEdgesRequest, UpdateEdgesRequest>;
using WriteResponses = std::variant<CreateVerticesResponse, DeleteVerticesResponse, UpdateVerticesResponse,
                                    CreateEdgesResponse, DeleteEdgesResponse, UpdateEdgesResponse>;
