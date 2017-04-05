#include "storage/edge_accessor.hpp"
#include "database/graph_db_accessor.hpp"
#include "storage/vertex_accessor.hpp"

GraphDbTypes::EdgeType EdgeAccessor::edge_type() const {
  return current().edge_type_;
}

VertexAccessor EdgeAccessor::from() const {
  return VertexAccessor(current().from_, db_accessor());
}

VertexAccessor EdgeAccessor::to() const {
  return VertexAccessor(current().to_, db_accessor());
}
