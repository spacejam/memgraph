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

namespace memgraph::io::simulator {
struct SimulatorStats {
  uint64_t total_messages = 0;
  uint64_t dropped_messages = 0;
  uint64_t timed_out_requests = 0;
  uint64_t total_requests = 0;
  uint64_t total_responses = 0;
  uint64_t simulator_ticks = 0;
};
};  // namespace memgraph::io::simulator