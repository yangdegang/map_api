#include "map-api/chunk-transaction.h"

namespace map_api {

void ChunkTransaction::insert(std::shared_ptr<Revision> revision) {
  CHECK_NOTNULL(revision.get());
  CHECK(revision->structureMatch(*structure_reference_));
  Id id;
  revision->get(CRTable::kIdField, &id);
  CHECK(insertions_.insert(std::make_pair(id, revision)).second);
}

void ChunkTransaction::update(std::shared_ptr<Revision> revision) {
  CHECK_NOTNULL(revision.get());
  CHECK(revision->structureMatch(*structure_reference_));
  CHECK(cache_->type() == CRTable::Type::CRU);
  Id id;
  revision->get(CRTable::kIdField, &id);
  CHECK(updates_.insert(std::make_pair(id, revision)).second);
}

std::shared_ptr<Revision> ChunkTransaction::getById(const Id& id) {
  std::shared_ptr<Revision> uncommitted = getByIdFromUncommitted(id);
  if (uncommitted) {
    return uncommitted;
  }
  return cache_->getById(id, begin_time_);
}

std::shared_ptr<Revision> ChunkTransaction::getByIdFromUncommitted(
    const Id& id) const {
  UpdateMap::const_iterator updated = updates_.find(id);
  if (updated != updates_.end()) {
    return updated->second;
  }
  InsertMap::const_iterator inserted = insertions_.find(id);
  if (inserted != insertions_.end()) {
    return inserted->second;
  }
  return std::shared_ptr<Revision>();
}

ChunkTransaction::ChunkTransaction(const LogicalTime& begin_time,
                                   CRTable* cache)
: begin_time_(begin_time), cache_(CHECK_NOTNULL(cache)) {
  CHECK(begin_time < LogicalTime::sample());
  insertions_.clear();
  updates_.clear();
  structure_reference_ = cache_->getTemplate();
}

} /* namespace map_api */
