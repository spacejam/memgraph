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

// TODO(tyler) add role and term to all log statements
// TODO(tyler) buffer out-of-order Append buffers to reassemble more quickly
// TODO(tyler) handle granular batch sizes based on simple flow control
// TODO(tyler) add "application" test that asserts that all state machines apply the same items in-order
// TODO(tyler) fix disparity between 1-based indexing in raft paper and log's index
// TODO(tyler) make rng thread-local to facilitate determinism despite non-deterministic mutex races

#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "io/v3/simulator.hpp"

using Op = std::vector<uint8_t>;
using Term = uint64_t;
using LogIndex = uint64_t;
using Time = uint64_t;
using Duration = uint64_t;
using RequestId = uint64_t;

/// The request that a client sends to request that
/// the cluster replicates their data.
struct ReplicationRequest {
  std::vector<uint8_t> opaque_data;
};

struct ReplicationResponse {
  bool success;
  std::optional<Address> retry_leader;
};

struct AppendRequest {
  Term term;
  LogIndex last_log_index;
  Term last_log_term;
  std::vector<std::pair<Term, Op>> entries;
  LogIndex leader_commit;
};

struct AppendResponse {
  bool success;
  Term term;
  Term last_log_term;
  // a small optimization over the raft paper, tells
  // the leader the offset that we are interested in
  // to send log offsets from for us. This will only
  // be useful at the beginning of a leader's term.
  LogIndex last_log_index;
};

struct VoteRequest {
  Term term;
  LogIndex last_log_index;
  Term last_log_term;
};

struct VoteResponse {
  Term term;
  LogIndex committed_log_size;
  bool vote_granted;
};

struct CommonState {
  Term term = 0;
  std::vector<std::pair<Term, Op>> log;
  LogIndex committed_log_size = 0;
  LogIndex last_applied = 0;
};

struct FollowerTracker {
  LogIndex next_index;
  LogIndex confirmed_contiguous_index = 0;
};

struct PendingClientRequest {
  LogIndex log_index;
  RequestId request_id;
  Address address;
};

struct Leader {
  std::map<Address, FollowerTracker> followers;
  std::deque<PendingClientRequest> pending_client_requests;
};

struct Candidate {
  std::map<Address, LogIndex> successful_votes;
  Time election_began;
  std::set<Address> outstanding_votes;
};

struct Follower {
  Time last_received_append_entries_timestamp;
  Address leader_address;
};

using Role = std::variant<Candidate, Leader, Follower>;

template <typename IoImpl>
class Server {
  CommonState state_;
  Role role_ = Candidate{};
  Io<IoImpl> io_;
  std::vector<Address> peers_;

 public:
  Server(Io<IoImpl> io, std::vector<Address> peers) : io_(io), peers_(peers) {}

  void Run() {
    Time last_cron = io_.Now();

    while (!io_.ShouldShutDown()) {
      auto now = io_.Now();
      Duration random_cron_interval = RandomTimeout(500, 2000);
      if (now - last_cron > random_cron_interval) {
        Cron();
        last_cron = now;
      }

      Duration receive_timeout = RandomTimeout(10000, 50000);

      auto request_result =
          io_.template ReceiveWithTimeout<AppendRequest, AppendResponse, ReplicationRequest, VoteRequest, VoteResponse>(
              receive_timeout);
      if (request_result.HasError()) {
        continue;
      }

      auto request = std::move(request_result.GetValue());

      Handle(std::move(request.message), request.request_id, request.from_address);
    }
  }

 private:
  void BumpCommitIndexAndReplyToClients(Leader &leader) {
    // set the current committed_log_size based on the
    auto indices = std::vector<LogIndex>{state_.log.size()};
    for (const auto &[addr, f] : leader.followers) {
      indices.push_back(f.confirmed_contiguous_index);
    }
    std::ranges::sort(indices, std::ranges::greater());
    // assuming reverse sort (using std::ranges::greater)
    // 3 -> 2 (index 1)
    // 4 -> 3 (index 2)
    // 5 -> 3 (index 2)
    state_.committed_log_size = indices[(indices.size() / 2)];

    Log("Leader committed_log_size is now ", state_.committed_log_size);

    while (!leader.pending_client_requests.empty()) {
      auto &front = leader.pending_client_requests.front();
      if (front.log_index <= state_.committed_log_size) {
        Log("Leader responding SUCCESS to client");
        ReplicationResponse rr{
            .success = true,
            .retry_leader = std::nullopt,
        };
        io_.Send(front.address, front.request_id, std::move(rr));
        leader.pending_client_requests.pop_front();
      } else {
        break;
      }
    }
  }

  void BroadcastAppendEntries(std::map<Address, FollowerTracker> &followers) {
    Log("Leader broadcasting, total log size is ", state_.log.size());
    for (auto &[address, follower] : followers) {
      LogIndex index = follower.confirmed_contiguous_index;

      std::vector<std::pair<Term, Op>> entries;

      entries.insert(entries.begin(), state_.log.begin() + index, state_.log.end());

      Term previous_term_from_index = PreviousTermFromIndex(index);
      Log("previous term from index ", index, " is ", previous_term_from_index);
      AppendRequest ar{
          .term = state_.term,
          .last_log_index = index,
          .last_log_term = previous_term_from_index,
          .entries = entries,
          .leader_commit = state_.committed_log_size,
      };

      // request_id not necessary to set because it's not a Future-backed Request.
      RequestId request_id = 0;

      io_.Send(address, request_id, ar);
    }
  }

  void TimeOutOldClientRequests() { Duration client_request_timeout = RandomTimeout(10000, 150000); }

  Duration RandomTimeout(Duration min, Duration max) {
    std::uniform_int_distribution<> time_distrib(min, max);
    return io_.Rand(time_distrib);
  }

  Term PreviousTermFromIndex(LogIndex index) {
    if (index == 0 || state_.log.size() + 1 <= index) {
      return 0;
    } else {
      auto &[term, data] = state_.log.at(index - 1);
      return term;
    }
  }

  LogIndex CommittedLogIndex() { return state_.committed_log_size; }

  Term CommittedLogTerm() {
    MG_ASSERT(state_.log.size() >= state_.committed_log_size);
    if (state_.log.empty() || state_.committed_log_size == 0) {
      return 0;
    } else {
      auto &[term, data] = state_.log.at(state_.committed_log_size - 1);
      return term;
    }
  }

  LogIndex LastLogIndex() { return state_.log.size(); }

  Term LastLogTerm() {
    if (state_.log.empty()) {
      return 0;
    } else {
      auto &[term, data] = state_.log.back();
      return term;
    }
  }

  /// Periodic protocol maintenance.
  void Cron() {
    Log("running Cron");
    // dispatch periodic logic based on our role to a specific Cron method.
    std::optional<Role> new_role = std::visit([&](auto &&role) { return Cron(role); }, role_);

    if (new_role) {
      role_ = std::move(new_role).value();
    }
  }

  // Candidates keep sending Vote to peers until:
  // 1. receiving Append with a higher term (become Follower)
  // 2. receiving Vote with a higher term (become a Follower)
  // 3. receiving a quorum of responses to our last batch of Vote (become a Leader)
  std::optional<Role> Cron(Candidate &candidate) {
    auto now = io_.Now();
    Duration election_timeout = RandomTimeout(100000, 150000);

    if (now - candidate.election_began > election_timeout) {
      state_.term++;
      Log("becoming Candidate for term ", (int)state_.term);

      VoteRequest request{
          .term = state_.term,
          .last_log_index = LastLogIndex(),
          .last_log_term = LastLogTerm(),
      };

      auto outstanding_votes = std::set<Address>();

      for (const auto &peer : peers_) {
        // request_id not necessary to set because it's not a Future-backed Request.
        auto request_id = 0;
        io_.template Send<VoteRequest>(peer, request_id, request);
        outstanding_votes.insert(peer);
      }

      return Candidate{
          .successful_votes = std::map<Address, LogIndex>(),
          .election_began = now,
          .outstanding_votes = outstanding_votes,
      };
    }
    return std::nullopt;
  }

  // Followers become candidates if we haven't heard from the leader
  // after a randomized timeout.
  std::optional<Role> Cron(Follower &follower) {
    auto now = io_.Now();
    auto time_since_last_append_entries = now - follower.last_received_append_entries_timestamp;
    Duration election_timeout = RandomTimeout(100000, 150000);

    // randomized follower timeout with a range of 100-150ms.
    if (time_since_last_append_entries > election_timeout) {
      // become a Candidate if we haven't heard from the Leader after this timeout
      return Candidate{};
    } else {
      return std::nullopt;
    }
  }

  // Leaders (re)send AppendRequest to followers.
  std::optional<Role> Cron(Leader &leader) {
    // TODO time-out client requests if we haven't made progress after some threshold
    BroadcastAppendEntries(leader.followers);
    return std::nullopt;
  }

  /// **********************************************
  /// Handle + std::visit is how events are dispatched
  /// to certain code based on Server role.
  ///
  /// Handle(role, message, ...)
  /// takes as the first argument a reference
  /// to its role, and as the second argument, the
  /// message that has been received.
  /// **********************************************
  void Handle(
      std::variant<AppendRequest, AppendResponse, ReplicationRequest, VoteRequest, VoteResponse> &&message_variant,
      RequestId request_id, Address from_address) {
    // dispatch the message to a handler based on our role,
    // which can be specified in the Handle first argument,
    // or it can be `auto` if it's a handler for several roles
    // or messages.
    std::optional<Role> new_role =
        std::visit([&](auto &&msg, auto &&role) { return Handle(role, std::move(msg), request_id, from_address); },
                   std::move(message_variant), role_);

    // TODO(m3) maybe replace std::visit with get_if for explicit prioritized matching, [[likely]] etc...
    if (new_role) {
      role_ = std::move(new_role).value();
    }
  }

  // all roles can receive Vote and possibly become a follower
  template <typename AllRoles>
  std::optional<Role> Handle(AllRoles &, VoteRequest &&req, RequestId request_id, Address from_address) {
    Log("received Vote");
    bool last_log_term_dominates = req.last_log_term >= LastLogTerm();
    bool term_dominates = req.term > state_.term;
    bool last_log_index_dominates = req.last_log_index >= LastLogIndex();
    bool new_leader = last_log_term_dominates && term_dominates && last_log_index_dominates;

    if (new_leader) {
      MG_ASSERT(req.term > state_.term);
      MG_ASSERT(std::max(req.term, state_.term) == req.term);
    }

    VoteResponse res{
        .term = std::max(req.term, state_.term),
        .committed_log_size = state_.committed_log_size,
        .vote_granted = new_leader,
    };

    io_.Send(from_address, request_id, res);

    if (new_leader) {
      // become a follower
      state_.term = req.term;
      return Follower{
          .last_received_append_entries_timestamp = io_.Now(),
          .leader_address = from_address,
      };
    } else {
      return std::nullopt;
    }
  }

  std::optional<Role> Handle(Candidate &candidate, VoteResponse &&res, RequestId, Address from_address) {
    Log("Candidate received VoteResponse");

    if (!res.vote_granted || res.term != state_.term) {
      Log("received unsuccessful VoteResponse from term ", res.term, " when our candidacy term is ", state_.term);
      // we received a delayed VoteResponse from the past, which has to do with an election that is
      // no longer valid. We can simply drop this.
      return std::nullopt;
    }

    MG_ASSERT(candidate.outstanding_votes.contains(from_address),
              "Received unexpected VoteResponse from server not present in Candidate's outstanding_votes!");
    candidate.outstanding_votes.erase(from_address);

    MG_ASSERT(!candidate.successful_votes.contains(from_address),
              "Received unexpected VoteResponse from server already in Candidate's successful_votes!");
    candidate.successful_votes.insert({from_address, res.committed_log_size});

    if (candidate.successful_votes.size() >= candidate.outstanding_votes.size()) {
      std::map<Address, FollowerTracker> followers{};

      for (const auto &[address, committed_log_size] : candidate.successful_votes) {
        FollowerTracker follower{
            .next_index = committed_log_size,
            .confirmed_contiguous_index = committed_log_size,
        };
        followers.insert({address, std::move(follower)});
      }
      for (const auto &address : candidate.outstanding_votes) {
        FollowerTracker follower{
            .next_index = state_.log.size(),
            .confirmed_contiguous_index = 0,
        };
        followers.insert({address, std::move(follower)});
      }

      BroadcastAppendEntries(followers);

      Log("becoming Leader at term ", (int)state_.term);
      return Leader{
          .followers = std::move(followers),
          .pending_client_requests = std::deque<PendingClientRequest>(),
      };
    }

    return std::nullopt;
  }

  template <typename AllRoles>
  std::optional<Role> Handle(AllRoles &, VoteResponse &&res, RequestId request_id, Address from_address) {
    Log("non-Candidate received VoteResponse");
    return std::nullopt;
  }

  // only leaders actually handle replication requests from clients
  std::optional<Role> Handle(Leader &leader, ReplicationRequest &&req, RequestId request_id, Address from_address) {
    Log("Leader received ReplicationRequest");

    // we are the leader. add item to log and send Append to peers
    state_.log.emplace_back(std::pair(state_.term, std::move(req.opaque_data)));

    PendingClientRequest pcr{
        .log_index = state_.log.size(),
        .request_id = request_id,
        .address = from_address,
    };

    leader.pending_client_requests.push_back(pcr);

    BroadcastAppendEntries(leader.followers);

    // TODO add message to pending requests buffer, reply asynchronously
    return std::nullopt;
  }

  std::optional<Role> Handle(Follower &follower, ReplicationRequest &&req, RequestId request_id, Address from_address) {
    auto res = ReplicationResponse{};

    res.success = false;
    Log("redirecting client to known Leader with port ", follower.leader_address.last_known_port);
    res.retry_leader = follower.leader_address;

    io_.Send(from_address, request_id, res);

    return std::nullopt;
  }

  std::optional<Role> Handle(Candidate &, ReplicationRequest &&req, RequestId request_id, Address from_address) {
    Log("Candidate received ReplicationRequest - not redirecting because no Leader is known");
    auto res = ReplicationResponse{};

    res.success = false;

    Cron();

    io_.Send(from_address, request_id, res);

    return std::nullopt;
  }

  template <typename AllRoles>
  std::optional<Role> Handle(AllRoles &role, AppendRequest &&req, RequestId request_id, Address from_address) {
    AppendResponse res{
        .success = false,
        .term = state_.term,
        .last_log_term = CommittedLogTerm(),
        .last_log_index = CommittedLogIndex(),
    };

    // Handle early-exit conditions.
    if (req.term > state_.term) {
      // become follower of this leader, reply with our log status
      state_.term = req.term;

      io_.Send(from_address, request_id, res);

      Log("becoming Follower");
      return Follower{
          .last_received_append_entries_timestamp = io_.Now(),
          .leader_address = from_address,
      };
    } else if (req.term < state_.term) {
      // nack this request from an old leader
      io_.Send(from_address, request_id, res);

      return std::nullopt;
    };

    if constexpr (std::is_same<AllRoles, Follower>()) {
      // small specialization for when we're already a Follower
      MG_ASSERT(role.leader_address == from_address, "Multiple Leaders are acting under the same term number!");
      role.last_received_append_entries_timestamp = io_.Now();
    }

    // Handle steady-state conditions.
    if (req.last_log_index != LastLogIndex()) {
      Log("req.last_log_index is above our last applied log index");
    } else if (req.last_log_term != LastLogTerm()) {
      Log("req.last_log_term differs from our leader term at that slot, expected: ", LastLogTerm(), " but got ",
          req.last_log_term);
    } else {
      // happy path
      Log("Follower applying batch of entries to log of size ", req.entries.size());

      // possibly chop-off stuff that was replaced by
      // things with different terms (we got data that
      // hasn't reached consensus yet, which is normal)
      state_.log.resize(req.last_log_index);

      state_.log.insert(state_.log.end(), req.entries.begin(), req.entries.end());

      state_.committed_log_size = std::min(req.leader_commit, LastLogIndex());

      res.success = true;
      res.last_log_term = LastLogTerm();
      res.last_log_index = LastLogIndex();
    }

    io_.Send(from_address, request_id, res);

    return std::nullopt;
  }

  std::optional<Role> Handle(Leader &leader, AppendResponse &&res, RequestId request_id, Address from_address) {
    if (res.term != state_.term) {
    } else if (!leader.followers.contains(from_address)) {
    } else if (!res.success) {
    } else {
      Log("Leader got successful AppendResponse");
      FollowerTracker &follower = leader.followers.at(from_address);
      follower.next_index = std::max(follower.next_index, res.last_log_index);
      follower.confirmed_contiguous_index = std::max(follower.confirmed_contiguous_index, res.last_log_index);

      BumpCommitIndexAndReplyToClients(leader);
    }
    return std::nullopt;
  }

  template <typename AllRoles>
  std::optional<Role> Handle(AllRoles &, AppendResponse &&res, RequestId request_id, Address from_address) {
    // we used to be the leader, and are getting old delayed responses
    return std::nullopt;
  }

  template <typename... Ts>
  void Log(Ts &&...args) {
    auto now = io_.Now();
    std::cout << (int)now << " raft server " << (int)io_.GetAddress().last_known_port << " ";
    (std::cout << ... << args) << std::endl;
  }
};

template <typename IoImpl>
void RunServer(Server<IoImpl> server) {
  server.Run();
}

void RunSimulation() {
  auto config = SimulatorConfig{
      .drop_percent = 0,
      .perform_timeouts = true,
      .scramble_messages = true,
      .rng_seed = 0,
  };

  auto simulator = Simulator(config);

  auto cli_addr = Address::TestAddress(1);
  auto srv_addr_1 = Address::TestAddress(2);
  auto srv_addr_2 = Address::TestAddress(3);
  auto srv_addr_3 = Address::TestAddress(4);

  Io<SimulatorTransport> cli_io = simulator.Register(cli_addr);
  Io<SimulatorTransport> srv_io_1 = simulator.Register(srv_addr_1);
  Io<SimulatorTransport> srv_io_2 = simulator.Register(srv_addr_2);
  Io<SimulatorTransport> srv_io_3 = simulator.Register(srv_addr_3);

  std::vector<Address> srv_1_peers = {srv_addr_2, srv_addr_3};
  std::vector<Address> srv_2_peers = {srv_addr_1, srv_addr_3};
  std::vector<Address> srv_3_peers = {srv_addr_1, srv_addr_2};

  Server srv_1{srv_io_1, srv_1_peers};
  Server srv_2{srv_io_2, srv_2_peers};
  Server srv_3{srv_io_3, srv_3_peers};

  auto srv_thread_1 = std::jthread(RunServer<SimulatorTransport>, std::move(srv_1));
  simulator.IncrementServerCountAndWaitForQuiescentState(srv_addr_1);

  auto srv_thread_2 = std::jthread(RunServer<SimulatorTransport>, std::move(srv_2));
  simulator.IncrementServerCountAndWaitForQuiescentState(srv_addr_2);

  auto srv_thread_3 = std::jthread(RunServer<SimulatorTransport>, std::move(srv_3));
  simulator.IncrementServerCountAndWaitForQuiescentState(srv_addr_3);

  std::cout << "beginning test after servers have become quiescent" << std::endl;

  bool success = false;
  Address leader = srv_addr_1;
  for (int retries = 0; retries < 30; retries++) {
    // send request
    ReplicationRequest cli_req;
    cli_req.opaque_data = std::vector<uint8_t>{1, 2, 3, 4};

    ResponseFuture<ReplicationResponse> response_future =
        cli_io.RequestWithTimeout<ReplicationRequest, ReplicationResponse>(leader, cli_req, 5000);

    // receive response
    ResponseResult<ReplicationResponse> response_result = response_future.Wait();

    if (response_result.HasError()) {
      std::cout << "client timed out while trying to communicate with leader server " << std::endl;
      continue;
    }

    ResponseEnvelope<ReplicationResponse> response_envelope = response_result.GetValue();
    ReplicationResponse response = response_envelope.message;

    if (response.success) {
      success = true;
      break;
    }

    if (response.retry_leader) {
      leader = response.retry_leader.value();
      std::cout << "client redirected to leader server " << leader.last_known_port << std::endl;
    } else {
      std::cout << "client NOT redirected to leader server " << std::endl;
    }
  }

  MG_ASSERT(success);

  simulator.ShutDown();
  std::cout << "========================== SUCCESS :) ==========================" << std::endl;

  /*
  this is implicit in jthread's dtor
  srv_thread_1.join();
  srv_thread_2.join();
  srv_thread_3.join();
  */
}

int main() {
  int n_tests = 500;

  for (int i = 0; i < n_tests; i++) {
    std::cout << "========================== NEW SIMULATION " << i << " ==========================" << std::endl;
    RunSimulation();
  }

  std::cout << "passed " << n_tests << " tests!" << std::endl;

  return 0;
}
