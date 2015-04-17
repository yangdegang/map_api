#include "map-api/raft-node.h"

#include <algorithm>
#include <future>
#include <random>

#include <multiagent-mapping-common/conversions.h>

#include "./raft.pb.h"
#include "map-api/hub.h"
#include "map-api/logical-time.h"
#include "map-api/message.h"
#include "map-api/reader-writer-lock.h"

// TODO(aqurai): decide good values for these
constexpr int kHeartbeatTimeoutMs = 50;
constexpr int kHeartbeatSendPeriodMs = 25;

namespace map_api {

const char RaftNode::kAppendEntries[] = "raft_cluster_append_entries";
const char RaftNode::kAppendEntriesResponse[] = "raft_cluster_append_response";
const char RaftNode::kVoteRequest[] = "raft_cluster_vote_request";
const char RaftNode::kVoteResponse[] = "raft_cluster_vote_response";

MAP_API_PROTO_MESSAGE(RaftNode::kAppendEntries, proto::AppendEntriesRequest);
MAP_API_PROTO_MESSAGE(RaftNode::kAppendEntriesResponse,
                      proto::AppendEntriesResponse);
MAP_API_PROTO_MESSAGE(RaftNode::kVoteRequest, proto::RequestVote);
MAP_API_PROTO_MESSAGE(RaftNode::kVoteResponse, proto::ResponseVote);

const PeerId kInvalidId = PeerId();

RaftNode::RaftNode()
    : leader_id_(PeerId()),
      state_(State::FOLLOWER),
      current_term_(0),
      last_heartbeat_(std::chrono::system_clock::now()),
      state_thread_running_(false),
      is_exiting_(false),
      last_vote_request_term_(0),
      commit_index_(0),
      committed_result_(0) {
  election_timeout_ms_ = setElectionTimeout();
  VLOG(1) << "Peer " << PeerId::self()
          << ": Election timeout = " << election_timeout_ms_;
  LogEntry default_entry;
  default_entry.index = 0;
  default_entry.term = 0;
  default_entry.entry = 0;
  log_entries_.push_back(default_entry);
}

RaftNode::~RaftNode() {
  is_exiting_ = true;
  state_manager_thread_.join();
}

RaftNode& RaftNode::instance() {
  static RaftNode instance;
  return instance;
}

void RaftNode::registerHandlers() {
  Hub::instance().registerHandler(kAppendEntries, staticHandleAppendRequest);
  Hub::instance().registerHandler(kVoteRequest, staticHandleRequestVote);
}

void RaftNode::start() {
  state_manager_thread_ = std::thread(&RaftNode::stateManagerThread, this);
}

uint64_t RaftNode::term() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return current_term_;
}

const PeerId& RaftNode::leader() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return leader_id_;
}

RaftNode::State RaftNode::state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_;
}

void RaftNode::staticHandleAppendRequest(const Message& request,
                                     Message* response) {
  instance().handleAppendRequest(request, response);
}

void RaftNode::staticHandleRequestVote(const Message& request,
                                       Message* response) {
  instance().handleRequestVote(request, response);
}

// If there are no new entries, Leader sends empty message (heartbeat)
// Message contains leader commit index, used to update own commit index
// In Follower state, ONLY this thread writes to log_entries_ (via the function
//  followerAppendNewEntries(..))
void RaftNode::handleAppendRequest(const Message& request, Message* response) {
  proto::AppendEntriesRequest append_request;
  proto::AppendEntriesResponse append_response;
  request.extract<kAppendEntries>(&append_request);

  VLOG(3) << "Received AppendRequest/Heartbeat from " << request.sender();

  const PeerId request_sender = PeerId(request.sender());
  const uint64_t request_term = append_request.term();

  // Lock and read the state.
  std::unique_lock<std::mutex> state_lock(state_mutex_);

  bool sender_changed =
      (request_sender != leader_id_ || request_term != current_term_);

  // Lock and read log info
  log_mutex_.acquireReadLock();
  const uint64_t last_log_index = log_entries_.back().index;
  const uint64_t last_log_term = log_entries_.back().term;
  bool is_sender_log_newer = append_request.last_log_term() > last_log_term ||
                             (append_request.last_log_term() == last_log_term &&
                              append_request.last_log_index() >= last_log_index);

  // ================================
  // Check if the sender has changed.
  // ================================
  if (sender_changed) {
    if (request_term > current_term_ ||
        (request_term == current_term_ && !leader_id_.isValid()) ||
        (request_term < current_term_ && !leader_id_.isValid() &&
         is_sender_log_newer)) {
      // Update state and leader info if another leader with newer term is found
      // or, if a leader is found when a there isn't a known one. The new leader
      // should either have same/higher term or more updated log.
      current_term_ = request_term;
      leader_id_ = request_sender;
      if (state_ == State::LEADER) {
        state_ = State::FOLLOWER;
        follower_trackers_run_ = false;
      }

      // Update the last heartbeat info.
      std::lock_guard<std::mutex> heartbeat_lock(last_heartbeat_mutex_);
      last_heartbeat_ = std::chrono::system_clock::now();
    } else if (state_ == State::FOLLOWER && request_term == current_term_ &&
               request_sender != leader_id_ && current_term_ > 0 &&
               leader_id_.isValid()) {
      // This should not happen.
      LOG(FATAL) << "Peer " << PeerId::self().ipPort()
                 << " has found 2 leaders in the same term (" << current_term_
                 << "). They are " << leader_id_.ipPort() << " (current) and "
                 << request_sender.ipPort() << " (new) ";
    } else {
      // TODO(aqurai): Handle AppendEntry from a server with older term and log.
      append_response.set_response(proto::Response::REJECTED);
      append_response.set_last_log_index(log_entries_.back().index);
      append_response.set_last_log_term(log_entries_.back().term);
      append_response.set_commit_index(commit_index());
      log_mutex_.releaseReadLock();
      response->impose<kAppendEntriesResponse>(append_response);
      return;
    }
  } else {
    // Leader didn't change. Simply update last heartbeat time.
    std::lock_guard<std::mutex> heartbeat_lock(last_heartbeat_mutex_);
    last_heartbeat_ = std::chrono::system_clock::now();
  }
  append_response.set_term(current_term_);

  // ==============================
  // Append/commit new log entries.
  // ==============================
  CHECK(log_mutex_.upgradeToWriteLock());

  // Check if there are new entries.
  proto::Response response_status = followerAppendNewEntries(append_request);

  // Check if new entries are committed.
  if (response_status == proto::Response::SUCCESS) {
    followerCommitNewEntries(append_request);
  }

  append_response.set_response(response_status);
  append_response.set_last_log_index(log_entries_.back().index);
  append_response.set_last_log_term(log_entries_.back().term);
  append_response.set_commit_index(commit_index());
  log_mutex_.releaseWriteLock();
  state_lock.unlock();
  response->impose<kAppendEntriesResponse>(append_response);
}

void RaftNode::handleRequestVote(const Message& request, Message* response) {
  {
    std::lock_guard<std::mutex> lock(last_heartbeat_mutex_);
    last_heartbeat_ = std::chrono::system_clock::now();
  }
  proto::RequestVote request_vote;
  proto::ResponseVote response_vote;
  request.extract<kVoteRequest>(&request_vote);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  log_mutex_.acquireReadLock();
  response_vote.set_previous_log_index(log_entries_.back().index);
  response_vote.set_previous_log_term(log_entries_.back().term);
  
  bool is_candidate_log_newer = request_vote.last_log_term() > log_entries_.back().term ||
                                (request_vote.last_log_term() == log_entries_.back().term &&
                                 request_vote.last_log_index() >= log_entries_.back().index);
  log_mutex_.releaseReadLock();
  last_vote_request_term_ =
      std::max(static_cast<uint64_t>(last_vote_request_term_), request_vote.term());
    
  if (request_vote.term() > current_term_ && is_candidate_log_newer) {
    response_vote.set_vote(true);
    current_term_ = request_vote.term();
    leader_id_ = PeerId();
    if (state_ == State::LEADER) {
      follower_trackers_run_ = false;
    }
    state_ = State::FOLLOWER;
    VLOG(1) << "Peer " << PeerId::self().ipPort() << " is voting for "
            << request.sender() << " in term " << current_term_;
  } else {
    VLOG(1) << "Peer " << PeerId::self().ipPort() << " is declining vote for "
            << request.sender() << " in term " << request_vote.term() << ". Reason: "
            << (request_vote.term() > current_term_ ? "" : "Term is equal or less. ")
            << (is_candidate_log_newer ? "" : "Log is older. ");
    response_vote.set_vote(false);
  }
  
  response->impose<kVoteResponse>(response_vote);
  election_timeout_ms_ = setElectionTimeout();
}

bool RaftNode::sendAppendEntries(
    const PeerId& peer, const proto::AppendEntriesRequest& append_entries,
    proto::AppendEntriesResponse* append_response) {
  Message request, response;
  request.impose<kAppendEntries>(append_entries);
  if (Hub::instance().try_request(peer, &request, &response)) {
    response.extract<kAppendEntriesResponse>(append_response);
    return true;
  } else {
    VLOG(1) << "AppendEntries RPC failed for peer " << peer.ipPort();
    return false;
  }
}

RaftNode::VoteResponse RaftNode::sendRequestVote(const PeerId& peer, uint64_t term,
                              uint64_t last_log_index, uint64_t last_log_term) {
  Message request, response;
  proto::RequestVote vote_request;
  vote_request.set_term(term);
  vote_request.set_commit_index(commit_index());
  vote_request.set_last_log_index(last_log_index);
  vote_request.set_last_log_term(last_log_term);
  request.impose<kVoteRequest>(vote_request);
  if (Hub::instance().try_request(peer, &request, &response)) {
    proto::ResponseVote vote_response;
    response.extract<kVoteResponse>(&vote_response);
    if (vote_response.vote())
      return VoteResponse::VOTE_GRANTED;
    else
      return VoteResponse::VOTE_DECLINED;
  } else {
    return VoteResponse::FAILED_REQUEST;
  }
}

void RaftNode::stateManagerThread() {
  TimePoint last_hb_time;
  bool election_timeout = false;
  State state;
  uint64_t current_term;
  state_thread_running_ = true;

  while (!is_exiting_) {
    // Conduct election if timeout has occurred.
    if (election_timeout) {
      election_timeout = false;
      conductElection();
    }

    // Read state information.
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      state = state_;
      current_term = current_term_;
    }

    if (state == State::FOLLOWER) {
      // Check for heartbeat timeout if in follower state.
      {
        std::lock_guard<std::mutex> lock(last_heartbeat_mutex_);
        last_hb_time = last_heartbeat_;
      }
      TimePoint now = std::chrono::system_clock::now();
      double duration_ms = static_cast<double>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - last_hb_time).count());

      if (duration_ms >= election_timeout_ms_) {
        VLOG(1) << "Follower: " << PeerId::self() << " : Heartbeat timed out. ";
        election_timeout = true;
      } else {
        usleep(election_timeout_ms_ * kMillisecondsToMicroseconds);
      }
    } else if (state == State::LEADER) {
      // Launch follower_handler threads if state is LEADER.
      follower_trackers_run_ = true;
      for (const PeerId& peer : peer_list_) {
        follower_trackers_.emplace_back(&RaftNode::followerTrackerThread, this,
                                        peer, current_term);
      }

      std::mutex wait_mutex;
      std::unique_lock<std::mutex> wait_lock(wait_mutex);
      while (follower_trackers_run_) {
        leaderCommitReplicatedEntries();
        if (follower_trackers_run_) {
          usleep(kHeartbeatSendPeriodMs * kMillisecondsToMicroseconds);
        }
      }
      VLOG(1) << "Peer " << PeerId::self() << " Lost leadership. ";
      for (std::thread& follower_thread : follower_trackers_) {
        follower_thread.join();
      }
      follower_trackers_.clear();
      VLOG(1) << "Peer " << PeerId::self() << ": Follower trackers closed. ";
    }
  }  // while(!is_exiting_)
  state_thread_running_ = false;
}

void RaftNode::conductElection() {
  uint16_t num_votes = 0;
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  state_ = State::CANDIDATE;
  current_term_ = std::max(current_term_ + 1, last_vote_request_term_ + 1);
  uint64_t term = current_term_;
  leader_id_ = PeerId();
  log_mutex_.acquireReadLock();
  const uint64_t last_log_index = log_entries_.back().index;
  const uint64_t last_log_term = log_entries_.back().term;
  log_mutex_.releaseReadLock();
  state_lock.unlock();

  VLOG(1) << "Peer " << PeerId::self() << " is an election candidate for term "
          << term;

  std::vector<std::future<VoteResponse>> responses;
  for (const PeerId& peer : peer_list_) {
    std::future<VoteResponse> vote_response =
        std::async(std::launch::async, &RaftNode::sendRequestVote, this, peer,
                   term, last_log_index, last_log_term);
    responses.push_back(std::move(vote_response));
  }
  for (std::future<VoteResponse>& response : responses) {
    if (response.get() == VoteResponse::VOTE_GRANTED) {
      ++num_votes;
    } else {
      // TODO(aqurai): Handle non-responding peers
    }
  }

  state_lock.lock();
  if (state_ == State::CANDIDATE && num_votes >= peer_list_.size() / 2) {
    // This peer wins the election.
    state_ = State::LEADER;
    leader_id_ = PeerId::self();
    // Renew election timeout every session.
    election_timeout_ms_ = setElectionTimeout();
    VLOG(1) << "*** Peer " << PeerId::self()
            << " Elected as the leader for term " << current_term_ << " with "
            << num_votes + 1 << " votes. ***";
  } else if (state_ == State::CANDIDATE) {
    // This peer doesn't win the election.
    state_ = State::FOLLOWER;
    leader_id_ = PeerId();
    // Set a longer election timeout if the candidate loses election to prevent
    // from holding elections and getting rejected repeatedly in consecutive
    // terms (due to less updated log) and blocking other peers from holding
    // election.
    election_timeout_ms_ = 4 * setElectionTimeout();
  }
  std::unique_lock<std::mutex> heartbeat_lock(last_heartbeat_mutex_);
  last_heartbeat_ = std::chrono::system_clock::now();
}

void RaftNode::followerTrackerThread(const PeerId& peer, uint64_t term) {
  uint64_t follower_next_index = commit_index() + 1;  // This is at least 1.
  uint64_t follower_commit_index = 0;
  proto::AppendEntriesRequest append_entries;
  proto::AppendEntriesResponse append_response;

  // This lock is only used for waiting on a condition variable
  // (new_entries_signal_), which is notified when there is a new log entry.
  std::mutex wait_mutex;
  std::unique_lock<std::mutex> wait_lock(wait_mutex);

  while (follower_trackers_run_) {
    bool append_successs = false;
    while (!append_successs && follower_trackers_run_) {
      append_entries.Clear();
      append_response.Clear();
      append_entries.set_term(term);
      log_mutex_.acquireReadLock();
      append_entries.set_commit_index(commit_index());
      append_entries.set_last_log_index(log_entries_.back().index);
      append_entries.set_last_log_term(log_entries_.back().term);
      if (follower_next_index > log_entries_.back().index) {
        // There are no new entries to send. Send an empty message (heartbeat).
      } else if (follower_next_index <= log_entries_.back().index) {
        // There is at least one new entry to be sent.
        std::vector<LogEntry>::iterator it =
            getIteratorByIndex(follower_next_index);

        // if this is the case, the control shouldn't have reached here,
        CHECK(it != log_entries_.end());
        append_entries.set_new_entry(it->entry);
        append_entries.set_new_entry_term(it->term);
        append_entries.set_previous_log_index((it - 1)->index);
        append_entries.set_previous_log_term((it - 1)->term);
      }
      log_mutex_.releaseReadLock();

      if (!sendAppendEntries(peer, append_entries, &append_response)) {
        VLOG(1) << PeerId::self() << ": Failed sendAppendEntries to " << peer;
        continue;
      }

      follower_commit_index = append_response.commit_index();
      append_successs =
          (append_response.response() == proto::Response::SUCCESS);
      if (append_successs) {
        if (follower_next_index == append_response.last_log_index()) {
          // The response is from an append entry RPC, not a regular heartbeat.
          log_mutex_.acquireWriteLock();
          std::vector<LogEntry>::iterator it =
              getIteratorByIndex(follower_next_index);
          it->replicator_peers.insert(peer);
          // TODO(aqurai): Remove later
          VLOG_IF(1, it->index % 20 == 0 &&
                         it->replicator_peers.size() == peer_list_.size())
              << "******** Entry " << it->index << " replicated on all peers";
          log_mutex_.releaseWriteLock();
          ++follower_next_index;
        }
      } else {
        // Append on follower failed due to a conflict. Send an older entry
        // and try again.
        CHECK_GT(follower_next_index, 1);
        --follower_next_index;
        if (follower_commit_index >= follower_next_index &&
            (append_response.response() != proto::Response::REJECTED)) {
          // This should not happen.
          LOG(FATAL) << PeerId::self()
                     << ": Conflicting entry already committed on peer " << peer
                     << ". Peer commit index " << follower_commit_index
                     << ". Peer last log index, term "
                     << append_response.last_log_index() << ", "
                     << append_response.last_log_term() << ". "
                     << "Leader previous log index, term "
                     << append_entries.previous_log_index() << ", "
                     << append_entries.previous_log_term() << ". ";
        }
      }
    }  //  while (!append_successs && follower_trackers_run_)

    if (follower_trackers_run_) {
      new_entries_signal_.wait_for(
          wait_lock, std::chrono::milliseconds(kHeartbeatSendPeriodMs));
    }
  }  // while (follower_trackers_run_)
}

int RaftNode::setElectionTimeout() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dist(kHeartbeatTimeoutMs,
                                       3 * kHeartbeatTimeoutMs);
  return dist(gen);
}

// Assumes at least read lock is acquired for log_mutex_
std::vector<RaftNode::LogEntry>::iterator RaftNode::getIteratorByIndex(
    uint64_t index) {
  std::vector<LogEntry>::iterator it = log_entries_.end();
  if (index < log_entries_.front().index || index > log_entries_.back().index) {
    return it;
  } else {
    // The log indexes are always sequential.
    it = log_entries_.begin() + (index - log_entries_.front().index);
    // TODO(aqurai): Remove check later?
    CHECK(it->index == index);
    return it;
  }
}

proto::Response RaftNode::followerAppendNewEntries(
    const proto::AppendEntriesRequest& request) {
  if (!request.has_new_entry() || !request.has_new_entry_term() ||
      !request.has_previous_log_index() || !request.has_previous_log_term()) {
    // No need to proceed further as message contains no new entries
    return proto::Response::SUCCESS;
  } else if (request.previous_log_index() == log_entries_.back().index &&
             request.previous_log_term() == log_entries_.back().term) {
    // All is well, append the new entry, but don't commit it yet.
    LogEntry new_entry;
    new_entry.index = log_entries_.back().index + 1;
    new_entry.term = request.new_entry_term();
    new_entry.entry = request.new_entry();
    log_entries_.push_back(new_entry);
    return proto::Response::SUCCESS;
  } else if (request.previous_log_index() < log_entries_.back().index) {
    // Leader sends an older entry due to a conflict
    std::vector<LogEntry>::iterator it =
        getIteratorByIndex(request.previous_log_index());
    if (it != log_entries_.end() && request.previous_log_term() == it->term) {
      // The received entry matched one of the older entries in the log.
      CHECK(it != log_entries_.end());
      VLOG(1) << "Leader is erasing entries in log of " << PeerId::self()
              << ". from " << (it + 1)->index;
      CHECK_LT(commit_index(), (it + 1)->index);
      log_entries_.resize(std::distance(log_entries_.begin(), it + 1));
      LogEntry new_entry;
      new_entry.index = log_entries_.back().index + 1;
      new_entry.term = request.new_entry_term();
      new_entry.entry = request.new_entry();
      log_entries_.push_back(new_entry);
      return proto::Response::SUCCESS;
    } else {
      return proto::Response::FAILED;
    }
  } else {
    // term and index don't match with leader's log.
    return proto::Response::FAILED;
  }
  return proto::Response::FAILED;
}

void RaftNode::followerCommitNewEntries(
    const proto::AppendEntriesRequest& request) {
  CHECK_LE(commit_index(), log_entries_.back().index);
  if (commit_index() < request.commit_index() &&
      commit_index() < log_entries_.back().index) {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::vector<LogEntry>::iterator it = getIteratorByIndex(commit_index_);
    commit_index_ = std::min(log_entries_.back().index, request.commit_index());
    uint64_t result_increment = 0;

    std::vector<LogEntry>::iterator it2 = getIteratorByIndex(commit_index_);
    std::for_each(it + 1, it2 + 1,
                  [&](const LogEntry& e) { result_increment += e.entry; });

    committed_result_ += result_increment;
    // TODO(aqurai): Remove later
    VLOG_EVERY_N(1, 50) << PeerId::self() << ": Entry " << commit_index_ 
                        << " committed *****";
  }
}

const uint64_t& RaftNode::commit_index() const {
  std::lock_guard<std::mutex> lock(commit_mutex_);
  return commit_index_;
}

const uint64_t& RaftNode::committed_result() const {
  std::lock_guard<std::mutex> lock(commit_mutex_);
  return committed_result_;
}

uint64_t RaftNode::leaderAppendLogEntry(uint32_t entry) {
  uint64_t current_term;
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (state_ != State::LEADER) {
      return 0;
    }
    current_term = current_term_;
  }
  ScopedWriteLock log_lock(&log_mutex_);
  LogEntry new_entry;
  new_entry.index = log_entries_.back().index + 1;
  new_entry.term = current_term;
  new_entry.entry = entry;
  log_entries_.push_back(new_entry);
  new_entries_signal_.notify_all();
  VLOG_EVERY_N(1,10) << "Adding entry to log with index " << new_entry.index;
  return new_entry.index;
}

void RaftNode::leaderCommitReplicatedEntries() {
  ScopedReadLock log_lock(&log_mutex_);
  std::lock_guard<std::mutex> commit_lock(commit_mutex_);
  std::vector<LogEntry>::iterator it = getIteratorByIndex(commit_index_ + 1);
  if (it != log_entries_.end()) {
    if (it->replicator_peers.size() > peer_list_.size()) {
      LOG(FATAL) << "Replication count (" << it->replicator_peers.size()
                 << ") is higher than peer size (" << peer_list_.size()
                 << ") at peer " << PeerId::self() << " for entry index "
                 << commit_index_;
    }
    if (it->replicator_peers.size() > peer_list_.size() / 2) {
      // Replicated on more than half of the peers.
      ++commit_index_;
      CHECK_LE(commit_index_, log_entries_.back().index);
      VLOG_EVERY_N(1, 10) << PeerId::self() << ": Commit index increased to " 
                          << commit_index_ << " With replication count " 
                          << it->replicator_peers.size()
                          << " and with term " << it->term;
      committed_result_ += it->entry;
      return;
    }
  }
}

}  // namespace map_api