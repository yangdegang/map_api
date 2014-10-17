#include "map-api/cru-table-stxxl-map.h"

namespace map_api {

CRUTableSTXXLMap::~CRUTableSTXXLMap() {}

bool CRUTableSTXXLMap::initCRDerived() { return true; }

bool CRUTableSTXXLMap::insertCRUDerived(
    const std::shared_ptr<Revision>& query) {
  CHECK(query != nullptr);
  Id id = query->getId<Id>();
  STXXLHistoryMap::iterator found = data_.find(id);
  if (found != data_.end()) {
    return false;
  }
  RevisionInformation revision_information;
  CHECK(revision_store_.storeRevision(*query, &revision_information));
  data_[id].push_front(revision_information);
  return true;
}

bool CRUTableSTXXLMap::bulkInsertCRUDerived(
    const NonConstRevisionMap& query) {
  for (const RevisionMap::value_type& pair : query) {
    if (data_.find(pair.first) != data_.end()) {
      return false;
    }
  }
  for (const RevisionMap::value_type& pair : query) {
    RevisionInformation revision_information;
    CHECK(revision_store_.storeRevision(*pair.second, &revision_information));
    data_[pair.first].push_front(revision_information);
  }
  return true;
}

bool CRUTableSTXXLMap::patchCRDerived(const std::shared_ptr<Revision>& query) {
  CHECK(query != nullptr);
  Id id = query->getId<Id>();
  LogicalTime time = query->getUpdateTime();
  STXXLHistoryMap::iterator found = data_.find(id);
  if (found == data_.end()) {
    found = data_.insert(std::make_pair(id, STXXLHistory())).first;
  }
  RevisionInformation revision_information;
  CHECK(revision_store_.storeRevision(*query, &revision_information));
  for (STXXLHistory::iterator it = found->second.begin();
      it != found->second.end(); ++it) {
    if (it->update_time_ <= time) {
      CHECK_NE(time, it->update_time_);
      found->second.insert(it, revision_information);
      return true;
    }
    LOG(WARNING) << "Patching, not in front!";  // shouldn't usually be the case
  }
  found->second.push_back(revision_information);
  return true;
}

std::shared_ptr<const Revision> CRUTableSTXXLMap::getByIdCRDerived(
    const Id& id, const LogicalTime& time) const {
  STXXLHistoryMap::const_iterator found = data_.find(id);
  if (found == data_.end()) {
    return std::shared_ptr<Revision>();
  }
  STXXLHistory::const_iterator latest = found->second.latestAt(time);
  if (latest == found->second.end()) {
    return std::shared_ptr<Revision>();
  }
  std::shared_ptr<const Revision> revision;
  CHECK(revision_store_.retrieveRevision(*latest, &revision));
  return revision;
}

void CRUTableSTXXLMap::dumpChunkCRDerived(const Id& chunk_id,
                                        const LogicalTime& time,
                                        RevisionMap* dest) {
  CHECK_NOTNULL(dest)->clear();
  forChunkItemsAtTime(
      chunk_id, time,
      [&dest](const Id& id, const Revision& item) {
        CHECK(dest->emplace(id, std::make_shared<Revision>(item)).second);
      });
}

void CRUTableSTXXLMap::findByRevisionCRDerived(int key,
                                             const Revision& value_holder,
                                             const LogicalTime& time,
                                             RevisionMap* dest) {
  CHECK_NOTNULL(dest);
  dest->clear();
  forEachItemFoundAtTime(
      key, value_holder, time,
      [&dest](const Id& id, const Revision& item) {
    CHECK(dest->find(id) == dest->end());
    CHECK(dest->emplace(id, std::make_shared<Revision>(item)).second);
      });
}

void CRUTableSTXXLMap::getAvailableIdsCRDerived(const LogicalTime& time,
                                              std::unordered_set<Id>* ids) {
  CHECK_NOTNULL(ids);
  ids->clear();
  ids->rehash(data_.size());
  for (const STXXLHistoryMap::value_type& pair : data_) {
    STXXLHistory::const_iterator latest = pair.second.latestAt(time);
    if (latest != pair.second.cend()) {
      std::shared_ptr<const Revision> revision;
      CHECK(revision_store_.retrieveRevision(*latest, &revision));
      if (!revision->isRemoved()) {
        ids->insert(pair.first);
      }
    }
  }
}

int CRUTableSTXXLMap::countByRevisionCRDerived(int key,
                                             const Revision& value_holder,
                                             const LogicalTime& time) {
  int count = 0;
  forEachItemFoundAtTime(
      key, value_holder, time,
      [&count](const Id& /*id*/,
               const Revision& /*item*/) { ++count; });
  return count;
}

int CRUTableSTXXLMap::countByChunkCRDerived(const Id& chunk_id,
                                          const LogicalTime& time) {
  int count = 0;
  forChunkItemsAtTime(
      chunk_id, time,
      [&count](const Id& /*id*/,
               const Revision& /*item*/) { ++count; });
  return count;
}

bool CRUTableSTXXLMap::insertUpdatedCRUDerived(
    const std::shared_ptr<Revision>& query) {
  return patchCRDerived(query);
}

void CRUTableSTXXLMap::findHistoryByRevisionCRUDerived(
    int key, const Revision& valueHolder, const LogicalTime& time,
    HistoryMap* dest) {
  CHECK_NOTNULL(dest);
  dest->clear();
  for (const STXXLHistoryMap::value_type& pair : data_) {
    // using current state for filter
    std::shared_ptr<const Revision> revision;
    CHECK(revision_store_.retrieveRevision(*pair.second.begin(), &revision));
    if (key < 0 || valueHolder.fieldMatch(*revision, key)) {
      History history;
      for (const RevisionInformation& revision_information : pair.second) {
        std::shared_ptr<const Revision> history_entry;
        CHECK(revision_store_.retrieveRevision(revision_information,
                                               &history_entry));
        history.emplace_back(history_entry);
      }
      CHECK(dest->emplace(pair.first, history).second);
    }
  }
  trimToTime(time, dest);
}

void CRUTableSTXXLMap::chunkHistory(
    const Id& chunk_id, const LogicalTime& time, HistoryMap* dest) {
  CHECK_NOTNULL(dest)->clear();
  for (const STXXLHistoryMap::value_type& pair : data_) {
    std::shared_ptr<const Revision> revision;
    CHECK(revision_store_.retrieveRevision(*pair.second.begin(), &revision));
    if (revision->getChunkId() == chunk_id) {
      History history;
      for (const RevisionInformation& revision_information : pair.second) {
        std::shared_ptr<const Revision> history_entry;
        CHECK(revision_store_.retrieveRevision(revision_information,
                                               &history_entry));
        history.emplace_back(history_entry);
      }
      CHECK(dest->emplace(std::make_pair(pair.first, history)).second);
    }
  }
  trimToTime(time, dest);
}

void CRUTableSTXXLMap::itemHistoryCRUDerived(const Id& id,
                                           const LogicalTime& time,
                                           History* dest) {
  CHECK_NOTNULL(dest)->clear();
  STXXLHistoryMap::const_iterator found = data_.find(id);
  CHECK(found != data_.end());
  History& history = *dest;
  for (const RevisionInformation& revision_information : found->second) {
    std::shared_ptr<const Revision> history_entry;
    CHECK(revision_store_.retrieveRevision(
        revision_information, &history_entry));
    history.emplace_back(history_entry);
  }
  dest->remove_if([&time](const std::shared_ptr<const Revision>& item) {
    return item->getUpdateTime() > time;
  });
}

inline void CRUTableSTXXLMap::forEachItemFoundAtTime(
    int key, const Revision& value_holder, const LogicalTime& time,
    const std::function<
        void(const Id& id, const Revision& item)>& action) {
  for (const STXXLHistoryMap::value_type& pair : data_) {
    STXXLHistory::const_iterator latest = pair.second.latestAt(time);
    if (latest != pair.second.cend()) {
      std::shared_ptr<const Revision> revision;
      CHECK(revision_store_.retrieveRevision(*latest, &revision));
      if (key < 0 || value_holder.fieldMatch(*revision, key)) {
        if (!revision->isRemoved()) {
          action(pair.first, *revision);
        }
      }
    }
  }
}

inline void CRUTableSTXXLMap::forChunkItemsAtTime(
    const Id& chunk_id, const LogicalTime& time,
    const std::function<
        void(const Id& id, const Revision& item)>& action) {
  for (const STXXLHistoryMap::value_type& pair : data_) {
    std::shared_ptr<const Revision> revision;
    CHECK(revision_store_.retrieveRevision(*pair.second.begin(), &revision));
    if (revision->getChunkId() == chunk_id) {
      STXXLHistory::const_iterator latest = pair.second.latestAt(time);
      if (latest != pair.second.cend()) {
        if (!revision->isRemoved()) {
          action(pair.first, *revision);
        }
      }
    }
  }
}

inline void CRUTableSTXXLMap::trimToTime(const LogicalTime& time,
                                       HistoryMap* subject) {
  CHECK_NOTNULL(subject);
  for (HistoryMap::value_type& pair : *subject) {
    pair.second.remove_if([&time](const std::shared_ptr<const Revision>& item) {
      return item->getUpdateTime() > time;
    });
  }
}

}  // namespace map_api
