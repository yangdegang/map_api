#ifndef DMAP_CHUNK_BASE_H_
#define DMAP_CHUNK_BASE_H_

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stddef.h>
#include <unordered_set>
#include <vector>

#include <multiagent-mapping-common/unique-id.h>

#include "dmap/chunk-data-container-base.h"

namespace dmap {
class ConstRevisionMap;
class LogicalTime;
class Revision;
class TableDescriptor;

namespace internal {
class ChunkView;
class DeltaView;
}  // namespace internal

class ChunkBase {
  friend class ChunkTransaction;
  friend class internal::ChunkView;
  friend class internal::DeltaView;
  friend class NetTable;

 public:
  virtual ~ChunkBase();
  void initializeNew(const common::Id& id,
                     const std::shared_ptr<TableDescriptor>& descriptor);
  virtual void initializeNewImpl(
      const common::Id& id,
      const std::shared_ptr<TableDescriptor>& descriptor) = 0;
  common::Id id() const;

  virtual void dumpItems(const LogicalTime& time,
                         ConstRevisionMap* items) const = 0;
  virtual size_t numItems(const LogicalTime& time) const = 0;
  virtual size_t itemsSizeBytes(const LogicalTime& time) const = 0;

  virtual void getCommitTimes(const LogicalTime& sample_time,
                              std::set<LogicalTime>* commit_times) const = 0;

  void getUpdateTimes(std::unordered_map<common::Id, LogicalTime>* result);

  virtual bool insert(const LogicalTime& time,
                      const std::shared_ptr<Revision>& item) = 0;

  virtual int peerSize() const = 0;

  // Non-const intended to avoid accidental write-lock while reading.
  virtual void writeLock() = 0;
  // Can be empty implementation if race conditions with committing can be
  // avoided otherwise.
  virtual void readLock() const = 0;

  virtual bool isWriteLocked() const = 0;

  virtual void unlock() const = 0;

  class ConstDataAccess {
   public:
    explicit ConstDataAccess(const ChunkBase& chunk);
    ~ConstDataAccess();

    const ChunkDataContainerBase* operator->() const;

   private:
    const ChunkBase& chunk_;
  };

  inline ConstDataAccess constData() const { return ConstDataAccess(*this); }

  // Requests all peers in MapApiHub to participate in a given chunk.
  // At the moment, this is not disputable by the other peers.
  virtual int requestParticipation() = 0;
  virtual int requestParticipation(const PeerId& peer) = 0;

  // Update: First locks chunk, then sends update to all peers for patching.
  virtual void update(const std::shared_ptr<Revision>& item) = 0;

  typedef std::function<void(const common::IdSet insertions,
                             const common::IdSet updates)> TriggerCallback;
  // Starts tracking insertions / updates after a lock request. The callback is
  // then called at an unlock request. The tracked insertions and updates are
  // passed. Note: If the sets are empty, the lock has probably been acquired
  // to modify chunk peers.
  // Returns position of attached trigger in trigger vector.
  size_t attachTrigger(const TriggerCallback& callback);
  void waitForTriggerCompletion();

  virtual LogicalTime getLatestCommitTime() const = 0;

 protected:
  // The following three MUST be called in the right places in order for
  // triggers to work:
  // After remote insert.
  void handleCommitInsert(const common::Id& inserted_id);
  // After remote update.
  void handleCommitUpdate(const common::Id& updated_id);
  // After the end of a remote commit.
  void handleCommitEnd();

  common::Id id_;
  std::unique_ptr<ChunkDataContainerBase> data_container_;

 private:
  // Insert and update for transactions.
  virtual void bulkInsertLocked(const MutableRevisionMap& items,
                                const LogicalTime& time) = 0;
  virtual void updateLocked(const LogicalTime& time,
                            const std::shared_ptr<Revision>& item) = 0;
  virtual void removeLocked(const LogicalTime& time,
                            const std::shared_ptr<Revision>& item) = 0;

  void leave();
  virtual void leaveImpl() = 0;
  void leaveOnceShared();
  virtual void awaitShared() = 0;

  void triggerWrapper(const std::unordered_set<common::Id>&& insertions,
                      const std::unordered_set<common::Id>&& updates);

  std::vector<TriggerCallback> triggers_;
  mutable std::mutex trigger_mutex_;
  mutable aslam::ReaderWriterMutex triggers_are_active_while_has_readers_;
  std::unordered_set<common::Id> trigger_insertions_, trigger_updates_;
};

}  // namespace dmap

#endif  // DMAP_CHUNK_BASE_H_