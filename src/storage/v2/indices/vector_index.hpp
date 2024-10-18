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

#include <cstdint>
#include <json/json.hpp>
#include <string>
#include "storage/v2/indices/vector_index_utils.hpp"
#include "storage/v2/vertex.hpp"

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_string(experimental_vector_indexes);
namespace memgraph::storage {

// The `VectorIndex` class is a high-level interface for managing vector indexes.
// It supports creating new indexes, adding nodes to an index, listing all indexes,
// and searching for nodes using a query vector.
// The class is thread-safe.
// pimpl is used to hide the implementation details. Inside the class, we have a unique pointer to the implementation.
// Look into the implementation details in the vector_index.cpp file.
class VectorIndex {
 public:
  VectorIndex();
  ~VectorIndex();
  VectorIndex(const VectorIndex &) = delete;
  VectorIndex &operator=(const VectorIndex &) = delete;
  VectorIndex(VectorIndex &&) noexcept;
  VectorIndex &operator=(VectorIndex &&) noexcept;

  void CreateIndex(const VectorIndexSpec &spec);
  void AddNode(Vertex *vertex, uint64_t commit_timestamp, std::vector<VectorIndexKey> &keys);
  void Commit(const std::vector<VectorIndexKey> &keys, uint64_t commit_timestamp);
  std::vector<std::string> ListAllIndices();
  std::size_t Size(const std::string &index_name);
  std::vector<Vertex *> Search(const std::string &index_name, uint64_t start_timestamp, uint64_t result_set_size,
                               const std::vector<float> &query_vector);

 private:
  struct Impl;
  std::unique_ptr<Impl> pimpl;
};

}  // namespace memgraph::storage
