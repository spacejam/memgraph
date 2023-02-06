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

// tests in this suite deal with edge cases in logical operator behavior
// that's not easily testable with single-phase testing. instead, for
// easy testing and latter readability they are tested end-to-end.

#include <filesystem>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "query/v2/interpreter.hpp"
#include "result_stream_faker.hpp"
#include "storage/v3/shard.hpp"

DECLARE_bool(query_cost_planner);

namespace memgraph::query::v2::tests {

class QueryExecution : public testing::Test {
 protected:
  storage::v3::Storage db;
  std::optional<storage::v3::Storage> db_;
  std::optional<InterpreterContext> interpreter_context_;
  std::optional<Interpreter> interpreter_;

  std::filesystem::path data_directory{std::filesystem::temp_directory_path() /
                                       "MG_tests_unit_query_v2_query_plan_edge_cases"};

  void SetUp() {
    db_.emplace();
    interpreter_context_.emplace(&*db_, InterpreterConfig{}, data_directory);
    interpreter_.emplace(&*interpreter_context_);
  }

  void TearDown() {
    interpreter_ = std::nullopt;
    interpreter_context_ = std::nullopt;
    db_ = std::nullopt;
  }

  /**
   * Execute the given query and commit the transaction.
   *
   * Return the query results.
   */
  auto Execute(const std::string &query) {
    ResultStreamFaker stream(&*db_);

    auto [header, _, qid] = interpreter_->Prepare(query, {}, nullptr);
    stream.Header(header);
    auto summary = interpreter_->PullAll(&stream);
    stream.Summary(summary);

    return stream.GetResults();
  }
};

TEST_F(QueryExecution, MissingOptionalIntoExpand) {
  Execute("CREATE SCHEMA ON :Person(id INTEGER)");
  Execute("CREATE SCHEMA ON :Dog(id INTEGER)");
  Execute("CREATE SCHEMA ON :Food(id INTEGER)");
  // validating bug where expanding from Null (due to a preceding optional
  // match) exhausts the expansion cursor, even if it's input is still not
  // exhausted
  Execute(
      "CREATE (a:Person {id: 1}), (b:Person "
      "{id:2})-[:Has]->(:Dog {id: 1})-[:Likes]->(:Food {id: 1})");
  ASSERT_EQ(Execute("MATCH (n) RETURN n").size(), 4);

  auto Exec = [this](bool desc, const std::string &edge_pattern) {
    // this test depends on left-to-right query planning
    FLAGS_query_cost_planner = false;
    return Execute(std::string("MATCH (p:Person) WITH p ORDER BY p.id ") + (desc ? "DESC " : "") +
                   "OPTIONAL MATCH (p)-->(d:Dog) WITH p, d "
                   "MATCH (d)" +
                   edge_pattern +
                   "(f:Food) "
                   "RETURN p, d, f")
        .size();
  };

  std::string expand = "-->";
  std::string variable = "-[*1]->";
  std::string bfs = "-[*bfs..1]->";

  EXPECT_EQ(Exec(false, expand), 1);
  EXPECT_EQ(Exec(true, expand), 1);
  EXPECT_EQ(Exec(false, variable), 1);
  EXPECT_EQ(Exec(true, bfs), 1);
  EXPECT_EQ(Exec(true, bfs), 1);
}

TEST_F(QueryExecution, EdgeUniquenessInOptional) {
  Execute("CREATE SCHEMA ON :label(id INTEGER)");
  // Validating that an edge uniqueness check can't fail when the edge is Null
  // due to optional match. Since edge-uniqueness only happens in one OPTIONAL
  // MATCH, we only need to check that scenario.
  Execute("CREATE (:label {id: 1}), (:label {id: 2})-[:Type]->(:label {id: 3})");
  ASSERT_EQ(Execute("MATCH (n) RETURN n").size(), 3);
  EXPECT_EQ(Execute("MATCH (n) OPTIONAL MATCH (n)-[r1]->(), (n)-[r2]->() "
                    "RETURN n, r1, r2")
                .size(),
            3);
}
}  // namespace memgraph::query::v2::tests