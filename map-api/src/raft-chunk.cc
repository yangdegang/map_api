#include <map-api/raft-chunk.h>

#include <multiagent-mapping-common/conversions.h>

#include "./core.pb.h"
#include "./chunk.pb.h"
#include "map-api/raft-chunk-data-ram-container.h"
#include "map-api/legacy-chunk-data-ram-container.h"
#include "map-api/raft-node.h"
#include "map-api/hub.h"
#include "map-api/message.h"
#include "map-api/net-table-manager.h"

namespace map_api {

RaftChunk::RaftChunk()
    : chunk_lock_attempted_(false),
      is_raft_chunk_locked_(false),
      lock_log_index_(0),
      chunk_write_lock_depth_(0) {}

RaftChunk::~RaftChunk() {
  raft_node_.stop();
}

bool RaftChunk::init(const common::Id& id,
                     std::shared_ptr<TableDescriptor> descriptor,
                     bool initialize) {
  id_ = id;
  // TODO(aqurai): No, this is not a good way of doing things.
  data_container_.reset(raft_node_.data_);
  CHECK(data_container_->init(descriptor));
  initialized_ = true;
  raft_node_.chunk_id_ = id_;
  raft_node_.table_name_ = descriptor->name();
  return true;
}

void RaftChunk::initializeNewImpl(
    const common::Id& id, const std::shared_ptr<TableDescriptor>& descriptor) {
  CHECK(init(id, descriptor, true));

  VLOG(1) << " INIT chunk at peer " << PeerId::self() << " in table "
          << raft_node_.table_name_;
  setStateLeaderAndStartRaft();
}

bool RaftChunk::init(const common::Id& id,
                     const proto::InitRequest& init_request,
                     std::shared_ptr<TableDescriptor> descriptor) {
  CHECK(init(id, descriptor, true));

  VLOG(1) << " INIT chunk at peer " << PeerId::self() << " in table "
          << raft_node_.table_name_;
  raft_node_.initChunkData(init_request);
  setStateFollowerAndStartRaft();
  return true;
}

void RaftChunk::dumpItems(const LogicalTime& time, ConstRevisionMap* items) const {
  CHECK_NOTNULL(items);
  data_container_->dump(time, items);
}

size_t RaftChunk::numItems(const LogicalTime& time) const {
  return data_container_->numAvailableIds(time);
}

size_t RaftChunk::itemsSizeBytes(const LogicalTime& time) const {
  ConstRevisionMap items;
  data_container_->dump(time, &items);
  size_t num_bytes = 0;
  for (const std::pair<common::Id, std::shared_ptr<const Revision> >& item :
       items) {
    CHECK(item.second != nullptr);
    const Revision& revision = *item.second;
    num_bytes += revision.byteSize();
  }
  return num_bytes;
}

void RaftChunk::getCommitTimes(const LogicalTime& sample_time,
                               std::set<LogicalTime>* commit_times) const {
  CHECK_NOTNULL(commit_times);
  //  Using a temporary unordered map because it should have a faster insertion
  //  time. The expected amount of commit times << the expected amount of items,
  //  so this should be worth it.
  std::unordered_set<LogicalTime> unordered_commit_times;
  ConstRevisionMap items;
  RaftChunkDataRamContainer::HistoryMap histories;
  static_cast<RaftChunkDataRamContainer*>(data_container_.get())
      ->chunkHistory(id(), sample_time, &histories);
  for (const RaftChunkDataRamContainer::HistoryMap::value_type& history :
       histories) {
    for (const std::shared_ptr<const Revision>& revision : history.second) {
      unordered_commit_times.insert(revision->getUpdateTime());
    }
  }
  commit_times->insert(unordered_commit_times.begin(),
                       unordered_commit_times.end());
}

bool RaftChunk::insert(const LogicalTime& time,
                       const std::shared_ptr<Revision>& item) {
  CHECK(item != nullptr);
  item->setChunkId(id());
  writeLock();
  static_cast<RaftChunkDataRamContainer*>(data_container_.get())
      ->checkAndPrepareInsert(time, item);
  // at this point, checkAndPrepareInsert() has modified the revision such that
  // all default
  // fields are also set, which allows remote peers to just patch the revision
  // into their table.
  CHECK(raft_node_.isRunning());
  if (raftInsertRequest(item) > 0) {
    syncLatestCommitTime(*item);
    unlock();
    return true;
  } else {
    unlock();
    return false;
  }
}

void RaftChunk::writeLock() {
  CHECK(raft_node_.isRunning());
  std::lock_guard<std::mutex> lock_mutex(write_lock_mutex_);
  LOG(WARNING) << PeerId::self() << " Attempting lock for chunk " << id()
               << ". Current depth: " << chunk_write_lock_depth_;
  chunk_lock_attempted_ = true;
  uint64_t serial_id = 0;
  if (is_raft_chunk_locked_) {
    ++chunk_write_lock_depth_;
    // LOG(WARNING) << PeerId::self() << " Inc lock depth, to " <<
    // chunk_write_lock_depth_ << ", chunk " << id(); // NOLINT
  } else {
    // LOG(WARNING) << PeerId::self() << " Sending Lock req, chunk " << id();
    CHECK_EQ(lock_log_index_, 0);
    // TODO(aqurai): make this function return bool instead of loop here.
    serial_id = request_id_.getNewId();
    while (raft_node_.isRunning()) {
      lock_log_index_ = raft_node_.sendChunkLockRequest(serial_id);
      if (lock_log_index_ > 0 &&
          raft_node_.checkIfEntryCommitted(lock_log_index_, serial_id)) {
        if (raft_node_.raft_chunk_lock_.isLockHolder(PeerId::self())) {
          break;
        } else {
          // Someone else has the lock. Try with a new Id.
          serial_id = request_id_.getNewId();
        }
      }
      usleep(500 * kMillisecondsToMicroseconds);
    }
    CHECK(raft_node_.raft_chunk_lock_.isLockHolder(PeerId::self()));

    if (lock_log_index_ > 0) {
      is_raft_chunk_locked_ = true;
    }
  }
  LOG(WARNING) << PeerId::self() << " acquired lock for chunk " << id()
               << ". Current depth: " << chunk_write_lock_depth_;
}

void RaftChunk::readLock() const {}

bool RaftChunk::isWriteLocked() {
  // TODO(aqurai): Implement this.
  std::lock_guard<std::mutex> lock(write_lock_mutex_);
  return is_raft_chunk_locked_;
}

void RaftChunk::unlock() const {
  CHECK(raft_node_.isRunning());
  // TODO(aqurai): Implement this.
  std::lock_guard<std::mutex> lock(write_lock_mutex_);
  LOG(WARNING) << PeerId::self() << " Attempting unlock for chunk " << id()
               << ". Current depth: " << chunk_write_lock_depth_;
  uint64_t serial_id = 0;
  if (!is_raft_chunk_locked_) {
    return;
  }
  if (chunk_write_lock_depth_ > 0) {
    --chunk_write_lock_depth_;
    // LOG(WARNING) << PeerId::self() << " Dec lock depth, to " <<
    // chunk_write_lock_depth_ << ", chunk " << id(); // NOLINT
  } else if (chunk_write_lock_depth_ == 0) {
    // Send unlock request to leader.
    // LOG(WARNING) << PeerId::self() << " Sending unlock req, chunk " << id();
    CHECK(raft_node_.raft_chunk_lock_.isLockHolder(PeerId::self()))
        << PeerId::self();
    serial_id = request_id_.getNewId();
    uint64_t index = 0;
    while (index == 0 && raft_node_.isRunning()) {
      index =
          raft_node_.sendChunkUnlockRequest(serial_id, lock_log_index_, true);
      if (index > 0 && raft_node_.checkIfEntryCommitted(index, serial_id)) {
        break;
      }
      usleep(500 * kMillisecondsToMicroseconds);
    }
    CHECK(!raft_node_.raft_chunk_lock_.isLockHolder(PeerId::self()));
    lock_log_index_ = 0;
    is_raft_chunk_locked_ = false;
    chunk_lock_attempted_ = false;
  }
}

int RaftChunk::requestParticipation() {
  // TODO(aqurai): Handle failure/leader change.
  std::set<PeerId> peers;
  Hub::instance().getPeers(&peers);
  int num_success = 0;
  for (const PeerId& peer : peers) {
    if (requestParticipation(peer)) {
      ++num_success;
    }
  }
  if (num_success > 0) {
    return 1;
  } else {
    return 0;
  }
}

int RaftChunk::requestParticipation(const PeerId& peer) {
  // TODO(aqurai): Handle failure/leader change.
  if (raft_node_.getState() == RaftNode::State::LEADER &&
      !raft_node_.hasPeer(peer)) {
    std::shared_ptr<proto::RaftLogEntry> entry(new proto::RaftLogEntry);
    entry->set_add_peer(peer.ipPort());
    entry->set_sender(PeerId::self().ipPort());
    uint64_t serial_id = request_id_.getNewId();
    entry->set_sender_serial_id(serial_id);
    uint64_t index = raft_node_.leaderSafelyAppendLogEntry(entry);
    if (index > 0 && raft_node_.checkIfEntryCommitted(index, serial_id)) {
      return 1;
    }
  }
  return 0;
}

void RaftChunk::update(const std::shared_ptr<Revision>& item) {
  CHECK(item != nullptr);
  CHECK_EQ(id(), item->getChunkId());
  writeLock();
  static_cast<RaftChunkDataRamContainer*>(data_container_.get())
      ->checkAndPrepareUpdate(LogicalTime::sample(), item);
  CHECK(raft_node_.isRunning());
  if (raftUpdateRequest(item) > 0) {
    syncLatestCommitTime(*item);
  }
  unlock();
  // TODO(aqurai): No return? What to do on fail?
}

bool RaftChunk::sendConnectRequest(const PeerId& peer,
                                   proto::ChunkRequestMetadata& metadata) {
  Message request, response;
  proto::ConnectResponse connect_response;
  connect_response.set_index(0);
  request.impose<RaftNode::kConnectRequest>(metadata);

  // TODO(aqurai): Avoid infinite loop. Use Chord index to get chunk holder
  // if request fails.
  PeerId request_peer = peer;
  while (connect_response.index() == 0) {
    if (!(Hub::instance().try_request(request_peer, &request, &response))) {
      break;
    }
    response.extract<RaftNode::kConnectResponse>(&connect_response);
    if (connect_response.index() > 0) {
      return true;
    } else if (connect_response.has_leader_id()) {
      request_peer = PeerId(connect_response.leader_id());
    }
    usleep(1000);
  }
  return false;
}

void RaftChunk::bulkInsertLocked(const MutableRevisionMap& items,
                                 const LogicalTime& time) {
  std::vector<proto::PatchRequest> insert_requests;
  for (const MutableRevisionMap::value_type& item : items) {
    CHECK_NOTNULL(item.second.get());
    item.second->setChunkId(id());
  }
  static_cast<RaftChunkDataRamContainer*>(data_container_.get())
      ->checkAndPrepareBulkInsert(time, items);
  for (const ConstRevisionMap::value_type& item : items) {
    // TODO(aqurai): Handle partial failure?
    raftInsertRequest(item.second);
  }
  // TODO(aqurai): No return value? What to do on fail?
}

void RaftChunk::updateLocked(const LogicalTime& time,
                             const std::shared_ptr<Revision>& item) {
  CHECK(item != nullptr);
  CHECK_EQ(id(), item->getChunkId());
  static_cast<RaftChunkDataRamContainer*>(data_container_.get())
      ->checkAndPrepareUpdate(LogicalTime::sample(), item);
  // TODO(aqurai): No return? What to do on fail?
  raftUpdateRequest(item);
}

void RaftChunk::removeLocked(const LogicalTime& time,
                             const std::shared_ptr<Revision>& item) {
  // TODO(aqurai): How is this different from Update???
  CHECK(item != nullptr);
  CHECK_EQ(id(), item->getChunkId());
  static_cast<RaftChunkDataRamContainer*>(data_container_.get())
      ->checkAndPrepareUpdate(LogicalTime::sample(), item);
  // TODO(aqurai): No return? What to do on fail?
  raftUpdateRequest(item);
}

uint64_t RaftChunk::raftInsertRequest(const Revision::ConstPtr& item) {
  CHECK(raft_node_.isRunning()) << PeerId::self();
  uint64_t index = 0;
  bool retrying = false;
  uint64_t serial_id = request_id_.getNewId();
  // TODO(aqurai): Retry forever?
  while (true && raft_node_.isRunning()) {
    index = raft_node_.sendInsertRequest(item, serial_id, retrying);
    if (index > 0 && raft_node_.checkIfEntryCommitted(index, serial_id)) {
      break;
    }
    retrying = true;
    usleep(150 * kMillisecondsToMicroseconds);
  }
  return index;
}

uint64_t RaftChunk::raftUpdateRequest(const Revision::ConstPtr& item) {
  CHECK(raft_node_.isRunning());
  uint64_t index = 0;
  bool retrying = false;
  uint64_t serial_id = request_id_.getNewId();
  while (raft_node_.isRunning()) {
    index = raft_node_.sendUpdateRequest(item, serial_id, retrying);
    if (index > 0 && raft_node_.checkIfEntryCommitted(index, serial_id)) {
      break;
    }
    retrying = true;
    usleep(150 * kMillisecondsToMicroseconds);
  }
  // Wait for the entry to be committed on this peer.
  return index;
}

void RaftChunk::leaveImpl() {
  writeLock();
  CHECK(raft_node_.isRunning());
  uint64_t serial_id = request_id_.getNewId();
  while (true) {
    if (raft_node_.sendLeaveRequest(serial_id)) {
      break;
    }
    usleep(500 * kMillisecondsToMicroseconds);
  }
  while (raft_node_.isRunning()) {
    uint64_t index = raft_node_.sendLeaveRequest(serial_id);
    if (index > 0 && raft_node_.checkIfEntryCommitted(index, serial_id)) {
      break;
    }
    usleep(150 * kMillisecondsToMicroseconds);
  }
  LOG(WARNING) << PeerId::self() << " left chunk " << id();
}

void RaftChunk::awaitShared() {}

}  // namespace map_api
