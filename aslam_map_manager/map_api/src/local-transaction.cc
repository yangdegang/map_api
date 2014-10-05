#include "map-api/local-transaction.h"

#include <memory>
#include <unordered_set>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "map-api/core.h"

namespace map_api {

std::recursive_mutex LocalTransaction::dbMutex_;

bool LocalTransaction::begin() {
  active_ = true;
  beginTime_ = LogicalTime::sample();
  return true;
}

// forward declaration required, else "specialization after instantiation"
template <>
inline bool LocalTransaction::hasContainerConflict(
    LocalTransaction::ConflictConditionVector* container);
template <>
inline bool LocalTransaction::hasContainerConflict(
    LocalTransaction::InsertMap* container);
template <>
inline bool LocalTransaction::hasContainerConflict(
    LocalTransaction::UpdateMap* container);

bool LocalTransaction::commit() {
  if (notifyAbortedOrInactive()) {
    return false;
  }
  // return false if no jobs scheduled
  if (insertions_.empty() && updates_.empty()) {
    LOG(WARNING) << "Committing transaction with no queries";
    return false;
  }
  // Acquire lock for database updates TODO(tcies) per-item locks
  {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    LogicalTime commit_time = LogicalTime::sample();
    if (hasContainerConflict(&insertions_)) {
      VLOG(3) << "Insert conflict, commit fails";
      return false;
    }
    if (hasContainerConflict(&updates_)) {
      VLOG(3) << "Update conflict, commit fails";
      return false;
    }
    if (hasContainerConflict(&conflictConditions_)) {
      VLOG(3) << "Conflict condition true, commit fails";
      return false;
    }
    // if no conflicts were found, apply changes, starting from inserts...
    // TODO(tcies) ideally, this should be rollback-able, e.g. by using
    // the SQL built-in transactions
    for (const std::pair<ItemId, SharedRevisionPointer>& insertion :
         insertions_) {
      CRTable* table = insertion.first.table;
      const Id& id = insertion.first.id;
      CRTable::ItemDebugInfo debugInfo(table->name(), id);
      const SharedRevisionPointer &revision = insertion.second;
      CHECK_EQ(id, revision->getId<Id>())
          << "Identifier ID does not match revision ID";
      if (!table->insert(commit_time, revision.get())) {
        LOG(ERROR) << debugInfo << "Insertion failed, aborting commit.";
        return false;
      }
    }
    // ...then updates
    for (const std::pair<ItemId, SharedRevisionPointer>& update : updates_) {
      CRUTable* table = static_cast<CRUTable*>(update.first.table);
      const Id& id = update.first.id;
      CRTable::ItemDebugInfo debugInfo(table->name(), id);
      const SharedRevisionPointer &revision = update.second;
      CHECK_EQ(id, revision->getId<Id>())
          << "Identifier ID does not match revision ID";
      table->update(revision.get());
    }
  }
  active_ = false;
  return true;
}

bool LocalTransaction::abort() {
  if (notifyAbortedOrInactive()) {
    return false;
  }
  active_ = false;
  return true;
}

Id LocalTransaction::insert(const SharedRevisionPointer& item, CRTable* table) {
  CHECK_NOTNULL(table);
  Id id;
  map_api::generateId(&id);
  if (!insert(id, item, table)) {
    return Id();
  }
  return id;
}

bool LocalTransaction::insert(const Id& id, const SharedRevisionPointer& item,
                              CRTable* table) {
  CHECK_NOTNULL(table);
  if (notifyAbortedOrInactive()) {
    return false;
  }
  if (!table->isInitialized()) {
    LOG(ERROR) << "Attempted to insert into uninitialized table";
    return false;
  }
  CHECK(item) << "Passed revision pointer is null";
  std::shared_ptr<Revision> reference = table->getTemplate();
  CHECK(item->structureMatch(*reference))
      << "Structure of item to be inserted: " << item->dumpToString()
      << " doesn't match table template " << reference->dumpToString();
  item->setId(id);
  CHECK(insertions_.insert(std::make_pair(ItemId(id, table), item)).second)
  << "You seem to already have inserted " << ItemId(id, table);
  return true;
}

LocalTransaction::SharedRevisionPointer LocalTransaction::read(const Id& id,
                                                               CRTable* table) {
  CHECK_NOTNULL(table);
  // TODO(tcies) uncommitted (test should fail)
  return table->getById(id, beginTime_);
}

bool LocalTransaction::dumpTable(CRTable* table, CRTable::RevisionMap* dest) {
  CHECK_NOTNULL(table);
  CHECK_NOTNULL(dest);
  return find(-1, 0, table, dest);
}

bool LocalTransaction::update(const Id& id,
                              const SharedRevisionPointer& newRevision,
                              CRUTable* table) {
  CHECK_NOTNULL(table);
  if (notifyAbortedOrInactive()) {
    return false;
  }
  updates_.insert(std::make_pair(ItemId(id, table), newRevision));
  return true;
}

// Going with locks for now TODO(tcies) adopt when moving to per-item locks
// std::shared_ptr<std::vector<proto::TableFieldDescriptor> >
// Transaction::requiredTableFields() {
//   std::shared_ptr<std::vector<proto::TableFieldDescriptor> > fields(
//       new std::vector<proto::TableFieldDescriptor>);
//   fields->push_back(proto::TableFieldDescriptor());
//   fields->back().set_name("locked_by");
//   fields->back().set_type(proto::TableFieldDescriptor_Type_HASH128);
//   return fields;
// }

bool LocalTransaction::notifyAbortedOrInactive() const {
  if (!active_) {
    LOG(ERROR) << "Transaction has not been initialized";
    return true;
  }
  if (aborted_) {
    LOG(ERROR) << "Transaction has previously been aborted";
    return true;
  }
  return false;
}

template <>
bool LocalTransaction::hasItemConflict(
    LocalTransaction::ConflictCondition* item) {
  CRTable::RevisionMap results;
  item->table->findByRevision(item->key, *item->valueHolder,
                              LogicalTime::sample(), &results);
  return !results.empty();
}

template <>
inline bool LocalTransaction::hasContainerConflict<LocalTransaction::InsertMap>(
    LocalTransaction::InsertMap* container) {
  for (const std::pair<ItemId, const SharedRevisionPointer>& item :
       *container) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    // Conflict if id present in table
    if (item.first.table->getById(item.first.id, LogicalTime::sample())) {
      LOG(WARNING) << "Table " << item.first.table->name() <<
          " already contains id " << item.first.id.hexString() <<
          ", transaction conflict!";
      return true;
    }
  }
  return false;
}
template <>
inline bool LocalTransaction::hasContainerConflict<LocalTransaction::UpdateMap>(
    LocalTransaction::UpdateMap* container) {
  for (const std::pair<ItemId, const SharedRevisionPointer>& item :
       *container) {
    if (this->insertions_.find(item.first) != this->insertions_.end()) {
      return false;
    }
    CHECK(item.first.table->type() == CRTable::Type::CRU);
    CRUTable* table = static_cast<CRUTable*>(item.first.table);
    LogicalTime latest_update;
    if (!table->getLatestUpdateTime(item.first.id, &latest_update)) {
      LOG(FATAL) << "Error retrieving update time";
    }
    if (latest_update >= beginTime_) {
      return true;
    }
  }
  return false;
}
template <>
inline bool LocalTransaction::hasContainerConflict<
    LocalTransaction::ConflictConditionVector>(
    LocalTransaction::ConflictConditionVector* container) {
  for (LocalTransaction::ConflictCondition& conflictCondition : *container) {
    if (hasItemConflict(&conflictCondition)) {
      return true;
    }
  }
  return false;
}

} /* namespace map_api */
