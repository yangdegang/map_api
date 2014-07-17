#include "map-api/chord-index.h"

#include <type_traits>

#include <glog/logging.h>
#include <gflags/gflags.h>

#include <Poco/DigestStream.h>
#include <Poco/MD5Engine.h>

const std::string kCleanJoin("clean");
const std::string kStabilizeJoin("stabilize");

DEFINE_string(join_mode, kStabilizeJoin,
              ("Can be " + kCleanJoin + " or " + kStabilizeJoin).c_str());
DEFINE_uint64(stabilize_us, 1000, "Interval of stabilization in microseconds");

namespace map_api {

ChordIndex::~ChordIndex() {}

template<typename DataType>
ChordIndex::Key ChordIndex::hash(const DataType& data) {
  // TODO(tcies) better method?
  Poco::MD5Engine md5;
  Poco::DigestOutputStream digest_stream(md5);
  digest_stream << data;
  digest_stream.flush();
  const Poco::DigestEngine::Digest& digest = md5.digest();
  bool digest_still_uchar_vec =
      std::is_same<
      Poco::DigestEngine::Digest, std::vector<unsigned char> >::value;
  CHECK(digest_still_uchar_vec) <<
      "Underlying type of Digest changed since Poco 1.3.6";
  union KeyUnion {
    Key key;
    unsigned char bytes[sizeof(Key)];
  };
  CHECK_EQ(sizeof(Key), sizeof(KeyUnion));
  KeyUnion return_value;
  for (size_t i = 0; i < sizeof(Key); ++i) {
    return_value.bytes[i] = digest[i];
  }
  return return_value.key;
}

bool ChordIndex::handleGetClosestPrecedingFinger(
    const Key& key, PeerId* result) {
  CHECK_NOTNULL(result);
  if (!waitUntilInitialized()) {
    return false;
  }
  *result = closestPrecedingFinger(key)->id;
  return true;
}

bool ChordIndex::handleGetSuccessor(PeerId* result) {
  CHECK_NOTNULL(result);
  if (!waitUntilInitialized()) {
    return false;
  }
  std::lock_guard<std::mutex> access_lock(peer_access_);
  *result = successor_->id;
  return true;
}

bool ChordIndex::handleGetPredecessor(PeerId* result) {
  if (!waitUntilInitialized()) {
    return false;
  }
  std::lock_guard<std::mutex> access_lock(peer_access_);
  *result = predecessor_->id;
  return true;
}

bool ChordIndex::handleJoin(
    const PeerId& requester, bool* success, std::vector<PeerId>* fingers,
    PeerId* predecessor, PeerId* redirection) {
  LOG(FATAL) << "Deprecated. Use stabilize join.";
  CHECK_NOTNULL(success);
  CHECK_NOTNULL(fingers);
  CHECK_NOTNULL(predecessor);
  CHECK_NOTNULL(redirection);
  if (!waitUntilInitialized()) {
    return false;
  }
  Key requester_key = hash(requester);
  if (isIn(requester_key, predecessor_->key, own_key_)) {
    fingers->clear();
    for (size_t i = 0; i < M; ++i) {
      // overflow intended
      PeerId finger = findSuccessor(requester_key + (1 << i));
      fingers->push_back(finger);
    }
    *predecessor = predecessor_->id;
    if (predecessor_->id != PeerId::self()) {
      CHECK(notifyRpc(predecessor_->id, requester));
    }
    handleNotify(requester);
    *success = true;
    return true;
  } else {
    *redirection = findSuccessor(requester_key);
    *success = false;
    return true;
  }
}

bool ChordIndex::handleNotify(const PeerId& peer_id) {
  if (!waitUntilInitialized()) {
    return false;
  }
  std::lock_guard<std::mutex> access_lock(peer_access_);
  if (peers_.find(peer_id) != peers_.end()) {
    // already aware of the node
    return true;
  }
  std::shared_ptr<ChordPeer> peer(new ChordPeer(peer_id));
  // TODO(tcies) fix fingers
  //  for (size_t i = 0; i < M; ++i) {
  //    if (isIn(peer->key, fingers_[i].base_key, fingers_[i].peer->key)) {
  //      fingers_[i].peer = peer;
  //      // no break intended: multiple fingers can have same peer
  //    }
  //  }
  if (isIn(peer->key, own_key_, successor_->key)) {
    successor_ = peer;
    VLOG(3) << own_key_ << " changed successor to " << peer->key <<
        " by notification";
  }
  // fix predecessor
  if (isIn(peer->key, predecessor_->key, own_key_)) {
    predecessor_ = peer;
    VLOG(3) << own_key_ << " changed predecessor to " << peer->key <<
        " by notification";
    // at this point we could start receiving data requests
    if (!integrated_) {
      // this has to be done in a separate thread in order to avoid deadlocks
      // of the kind of mutual waiting for response
      std::thread integrate_thread(integrateThread, this);
      integrate_thread.detach();
    }
  }
  // save peer to peer map only if information has been useful anywhere
  if (peer.use_count() > 1) {
    peers_[peer_id] = std::weak_ptr<ChordPeer>(peer);
    // TODO(tcies) how will it be removed?
  }
  return true;
}

bool ChordIndex::handleAddData(
    const std::string& key, const std::string& value) {
  // TODO(tcies) try-again-later & integrate if not integrated
  return addDataLocally(key, value);
}

bool ChordIndex::handleRetrieveData(
    const std::string& key, std::string* value) {
  CHECK_NOTNULL(value);
  // TODO(tcies) try-again-later & integrate if not integrated
  return retrieveDataLocally(key, value);
}

bool ChordIndex::handleFetchResponsibilities(
      const PeerId& requester, DataMap* responsibilities) {
  CHECK_NOTNULL(responsibilities);
  // TODO(tcies) try-again-later if not integrated
  for (const DataMap::value_type& item : data_) {
    if (!isIn(hash(item.first), hash(requester), own_key_)) {
      responsibilities->insert(item);
    }
  }
  return true;
}

bool ChordIndex::addData(const std::string& key, const std::string& value) {
  Key chord_key = hash(key);
  PeerId successor = findSuccessor(chord_key);
  if (successor == PeerId::self()) {
    return addDataLocally(key, value);
  } else {
    CHECK(addDataRpc(successor, key, value));
    return true;
  }
}

bool ChordIndex::retrieveData(const std::string& key, std::string* value) {
  CHECK_NOTNULL(value);
  Key chord_key = hash(key);
  PeerId successor = findSuccessor(chord_key);
  if (successor == PeerId::self()) {
    return retrieveDataLocally(key, value);
  } else {
    CHECK(retrieveDataRpc(successor, key, value));
    return true;
  }
}

PeerId ChordIndex::findSuccessor(const Key& key) {
  if (isIn(key, own_key_, successor_->key)) {
    return successor_->id;
  } else {
    PeerId result;
    CHECK(getSuccessorRpc(findPredecessor(key), &result));
    return result;
  }
}

PeerId ChordIndex::findPredecessor(const Key& key) {
  CHECK(!isIn(key, own_key_, successor_->key)) <<
      "FindPredecessor called while it's the calling peer";
  PeerId result = closestPrecedingFinger(key)->id, result_successor;
  CHECK(getSuccessorRpc(result, &result_successor));
  while (!isIn(key, hash(result), hash(result_successor))) {
    CHECK(getClosestPrecedingFingerRpc(result, key, &result));
    CHECK(getSuccessorRpc(result, &result_successor));
  }
  return result;
}

void ChordIndex::create() {
  init();
  for (size_t i = 0; i < M; ++i) {
    fingers_[i].peer = self_;
  }
  successor_ = self_;
  predecessor_ = self_;
  std::unique_lock<std::mutex> lock(initialized_mutex_);
  initialized_ = true;
  lock.unlock();
  initialized_cv_.notify_all();
}

void ChordIndex::join(const PeerId& other) {
  init();
  if (FLAGS_join_mode == kCleanJoin) {
    cleanJoin(other);
  } else {
    CHECK_EQ(kStabilizeJoin, FLAGS_join_mode);
    stabilizeJoin(other);
  }
  std::unique_lock<std::mutex> lock(initialized_mutex_);
  initialized_ = true;
  lock.unlock();
  initialized_cv_.notify_all();
}

void ChordIndex::cleanJoin(const PeerId& other) {
  LOG(FATAL) << "Deprecated. Use stabilize join.";
  // TODO(tcies) deadlock-free design, if any
  bool success = false;
  PeerId predecessor, redirect = other;
  std::vector<PeerId> fingers;
  while (!success) {
    CHECK(joinRpc(redirect, &success, &fingers, &predecessor, &redirect));
  }
  CHECK_EQ(M, fingers.size());
  for (size_t i = 0; i < fingers.size(); ++i) {
    registerPeer(fingers[i], &fingers_[i].peer);
  }
  successor_ = fingers_[0].peer;
  registerPeer(predecessor, &predecessor_);
}

void ChordIndex::stabilizeJoin(const PeerId& other) {
  registerPeer(other, &successor_);
  registerPeer(other, &predecessor_);
  //  LOG(INFO) << PeerId::self() << " stabilize-joined " << other;
}

void ChordIndex::leave() {
  terminate_ = true;
  stabilizer_.join();
  // TODO(tcies) move data to successor
  usleep(5000); // TODO(tcies) unhack!
  initialized_ = false;
  initialized_cv_.notify_all();
  integrated_ = false;
}

std::shared_ptr<ChordIndex::ChordPeer> ChordIndex::closestPrecedingFinger(
    const Key& key) const {
  // TODO(tcies) fingers
  //  for (size_t i = 0; i < M; ++i) {
  //    size_t index = M - 1 - i;
  //    Key actual_key = fingers_[index].peer->key;
  //    // + 1 in case finger = self TODO(tcies) cleaner?
  //    if (isIn(actual_key, own_key_ + 1, key)) {
  //      return fingers_[index].peer;
  //    }
  //  }
  //  LOG(FATAL) << "Called closest preceding finger on key which is smaller " <<
  //      "than successor key";
  //  return std::shared_ptr<ChordIndex::ChordPeer>();
  CHECK(!isIn(key, own_key_, successor_->key));
  return successor_;
}

void ChordIndex::stabilizeThread(ChordIndex* self) {
  CHECK_NOTNULL(self);
  if (!self->waitUntilInitialized()) {
    return;
  }
  while (!self->terminate_) {
    PeerId successor_predecessor;
    // self->peer_access_.lock(); // TODO(tcies) deadlock? FIXME(tcies) yes
    if (self->successor_->id != PeerId::self()) {
      if (!self->getPredecessorRpc(self->successor_->id, &successor_predecessor)) {
        // Node leaves have not been accounted for yet. However, not crashing
        // the program is necessery for successful (simultaneous) shutdown of a
        // network.
        continue;
      }
      if (successor_predecessor != PeerId::self() &&
          isIn(hash(successor_predecessor), self->own_key_,
               self->successor_->key)) {
        self->registerPeer(successor_predecessor, &self->successor_);
        VLOG(3) << self->own_key_ << " changed successor to " <<
            hash(successor_predecessor) << " through stabilization";
      }
      if (!self->notifyRpc(self->successor_->id, PeerId::self())) {
        continue;
      }
    }
    // self->peer_access_.unlock(); FIXME(tcies) deadlock: RWLock?
    usleep(FLAGS_stabilize_us);
    // TODO(tcies) finger fixing
  }
}

void ChordIndex::integrateThread(ChordIndex* self) {
  CHECK_NOTNULL(self);
  std::lock_guard<std::mutex> lock(self->integrate_mutex_);
  if (self->integrated_) {
    return;
  }
  // The assumption is made that successor is indeed the peer that contains the
  // required data. In general, this is valid because a peer gets notified
  // from its actual predecessor only once its true successor has registered it
  // as predecessor (for proof see report).
  // There is, however, a corner case in which this is not valid: Another
  // peer that is between this peer and its successor could join at the same
  // time as this peer sends this request. For now, this request should still
  // succeed as data is not being deleted once delegated. TODO(tcies) delete
  // data once delegated?
  CHECK(self->fetchResponsibilitiesRpc(self->successor_->id, &self->data_));
  self->integrated_ = true;
}

void ChordIndex::init() {
  //  LOG(INFO) << "Initializing chord for " << PeerId::self();
  own_key_ = hash(PeerId::self());
  self_.reset(new ChordPeer(PeerId::self()));
  //  LOG(INFO) << "Self key is " << self_->key;
  for (size_t i = 0; i < M; ++i) {
    fingers_[i].base_key = own_key_ + (1 << i); // overflow intended
  }
  terminate_ = false;
  stabilizer_ = std::thread(stabilizeThread, this);
}

void ChordIndex::registerPeer(
    const PeerId& peer, std::shared_ptr<ChordPeer>* target) {
  CHECK_NOTNULL(target);
  PeerMap::iterator found = peers_.find(peer);
  if (found == peers_.end()){
    target->reset(new ChordPeer(peer));
    peers_[peer] = std::weak_ptr<ChordPeer>(*target);
  } else {
    std::shared_ptr<ChordPeer> existing = found->second.lock();
    CHECK(existing);
    *target = existing;
  }
}

bool ChordIndex::isIn(
    const Key& key, const Key& from_inclusive, const Key& to_exclusive) {
  if (key == from_inclusive) {
    return true;
  }
  if (to_exclusive == from_inclusive) {
    return true;
  }
  if (from_inclusive <= to_exclusive) { // case doesn't pass 0
    return (from_inclusive < key && key < to_exclusive);
  } else { // case passes 0
    return (from_inclusive < key || key < to_exclusive);
  }
}

bool ChordIndex::waitUntilInitialized() {
  std::unique_lock<std::mutex> lock(initialized_mutex_);
  while (!initialized_) {
    if (terminate_) {
      return false;
    }
    initialized_cv_.wait(lock);
  }
  lock.unlock();
  initialized_cv_.notify_all();
  return true;
}

bool ChordIndex::addDataLocally(
    const std::string& key, const std::string& value) {
  if (data_.find(key) != data_.end()) {
    LOG(ERROR) << "Data with given key " << key << " already exists";
    return false;
  }
  data_[key] = value;
  return true;
}

bool ChordIndex::retrieveDataLocally(
    const std::string& key, std::string* value) {
  CHECK_NOTNULL(value);
  DataMap::iterator found = data_.find(key);
  if (found == data_.end()) {
    LOG(ERROR) << "Data with given key " << key << " does not exist";
    return false;
  }
  *value = data_[key];
  return true;
}

} /* namespace map_api */
