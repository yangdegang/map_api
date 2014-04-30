/*
 * history.cc
 *
 *  Created on: Apr 4, 2014
 *      Author: titus
 */

#include <map-api/history.h>

namespace map_api {

History::History(const std::string& tableName, const Hash& owner) :
            CRTableInterface(owner), tableName_(tableName) {}

bool History::init(){
  return setup(tableName_ + "_history");
}

bool History::define(){
  addField<Hash>("rowId");
  addField<Hash>("previous");
  addField<Revision>("revision");
  addField<Time>("time");
  return true;
}

std::shared_ptr<Revision> History::prepareForInsert(Revision& revision,
                                                    const Hash& previous){
  std::shared_ptr<Revision> query = getTemplate();
  Hash rowId;
  if (!revision.get<Hash>("ID", &rowId)){
    LOG(ERROR) << "revision doesn't seem to contain field ID, aborting";
    return std::shared_ptr<Revision>();
  }
  query->set("rowId", rowId);
  query->set("previous", previous);
  query->set("revision", revision);
  query->set("time", Time());
  return query;
}

std::shared_ptr<Revision> History::revisionAt(const Hash& id,
                                              const Time& time){
  typedef std::shared_ptr<Revision> RevisionPtr;
  RevisionPtr revisionIterator = rawGetRow(id);
  if (!revisionIterator){
    return RevisionPtr();
  }
  Time revisionTime;
  if (!revisionIterator->get<Time>("time", &revisionTime)){
    LOG(ERROR) << "History entry doesn't have field time!";
    return std::shared_ptr<Revision>();
  }
  while (revisionTime > time){
    Hash previous;
    if (!revisionIterator->get<Hash>("previous", &previous)){
      LOG(ERROR) << "History entry doesn't have field previous!";
      return std::shared_ptr<Revision>();
    }
    revisionIterator = rawGetRow(previous);
    if (!revisionIterator){
      LOG(ERROR) << "Failed to get previous revision " << previous.getString();
      return RevisionPtr();
    }
    if (!revisionIterator->get<Time>("time", &revisionTime)){
      LOG(ERROR) << "History entry doesn't have field time!";
      return std::shared_ptr<Revision>();
    }
  }
  std::shared_ptr<Revision> returnValue =
      std::shared_ptr<Revision>(new Revision);
  if (!revisionIterator->get<Revision>("revision", returnValue.get())){
    LOG(ERROR) << "History entry doesn't have field revision!";
    return std::shared_ptr<Revision>();
  }
  return returnValue;
}

} /* namespace map_api */