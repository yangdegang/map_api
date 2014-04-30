/*
 * write-only-table-interface.cc
 *
 *  Created on: Apr 4, 2014
 *      Author: titus
 */

#include <map-api/cru-table-interface.h>

#include <cstdio>
#include <map>

#include <Poco/Data/Common.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/BLOB.h>
#include <glog/logging.h>
#include <gflags/gflags.h>

#include "map-api/map-api-core.h"
#include "map-api/transaction.h"
#include "core.pb.h"

namespace map_api {

CRTableInterface::CRTableInterface(const Hash& owner) : owner_(owner) {}

const Hash& CRTableInterface::getOwner() const{
  return owner_;
}

bool CRTableInterface::addField(const std::string& name,
                                proto::TableFieldDescriptor_Type type){
  // make sure the field has not been defined yet
  for (int i=0; i<fields_size(); ++i){
    if (fields(i).name().compare(name) == 0){
      LOG(FATAL) << "In table " << this->name() << ": Field " << name <<
          " defined twice!" << std::endl;
    }
  }
  // otherwise, proceed with adding field
  proto::TableFieldDescriptor *field = add_fields();
  field->set_name(name);
  field->set_type(type);
  return true;
}

bool CRTableInterface::setup(const std::string& name){
  // TODO(tcies) Test before initialized or RAII
  // TODO(tcies) check whether string safe for SQL, e.g. no hyphens
  set_name(name);
  // Define table fields
  // enforced fields id (hash) and owner
  addField<Hash>("ID");
  addField<Hash>("owner");
  // transaction-enforced fields TODO(tcies) later
  // std::shared_ptr<std::vector<proto::TableFieldDescriptor> >
  // transactionFields(Transaction::requiredTableFields());
  // for (const proto::TableFieldDescriptor& descriptor :
  //     *transactionFields){
  //   addField(descriptor.name(), descriptor.type());
  // }
  // user-defined fields
  define();

  // connect to database & create table
  // TODO(tcies) register in master table
  session_ = MapApiCore::getInstance().getSession();
  createQuery();

  // Sync with cluster TODO(tcies)
  // sync();
  return true;
}

std::shared_ptr<Revision> CRTableInterface::getTemplate() const{
  std::shared_ptr<Revision> ret =
      std::shared_ptr<Revision>(
          new Revision);
  // add own name
  ret->set_table(name());
  // add editable fields
  for (int i=0; i<this->fields_size(); ++i){
    *(ret->add_fieldqueries()->mutable_nametype()) = this->fields(i);
  }
  return ret;
}

bool CRTableInterface::createQuery(){
  Poco::Data::Statement stat(*session_);
  stat << "CREATE TABLE IF NOT EXISTS " << name() << " (";
  // parse fields from descriptor as database fields
  for (int i=0; i<this->fields_size(); ++i){
    const proto::TableFieldDescriptor &fieldDescriptor = this->fields(i);
    proto::TableField field;
    // The following is specified in protobuf but not available.
    // We are using an outdated version of protobuf.
    // Consider upgrading once overwhelmingly necessary.
    // field.set_allocated_nametype(&fieldDescriptor);
    *field.mutable_nametype() = fieldDescriptor;
    if (i != 0){
      stat << ", ";
    }
    stat << fieldDescriptor.name() << " ";
    switch (fieldDescriptor.type()){
      case (proto::TableFieldDescriptor_Type_BLOB): stat << "BLOB"; break;
      case (proto::TableFieldDescriptor_Type_DOUBLE): stat << "REAL"; break;
      case (proto::TableFieldDescriptor_Type_HASH128): stat << "TEXT"; break;
      case (proto::TableFieldDescriptor_Type_INT32): stat << "INTEGER"; break;
      case (proto::TableFieldDescriptor_Type_INT64): stat << "INTEGER"; break;
      case (proto::TableFieldDescriptor_Type_STRING): stat << "TEXT"; break;
      default:
        LOG(FATAL) << "Field type not handled";
    }
    if (fieldDescriptor.name().compare("ID") == 0){
      stat << " PRIMARY KEY";
    }
  }
  stat << ");";

  try {
    stat.execute();
  } catch(const std::exception &e){
    LOG(FATAL) << "Create failed with exception " << e.what();
  }

  return true;
}

bool CRTableInterface::rawInsertQuery(const Revision& query){
  // TODO(tcies) verify schema

  // Bag for blobs that need to stay in scope until statement is executed
  std::vector<std::shared_ptr<Poco::Data::BLOB> > placeholderBlobs;

  // assemble SQLite statement
  Poco::Data::Statement stat(*session_);
  // NB: sqlite placeholders work only for column values
  stat << "INSERT INTO " << name() << " ";

  stat << "(";
  for (int i = 0; i < query.fieldqueries_size(); ++i){
    if (i > 0){
      stat << ", ";
    }
    stat << query.fieldqueries(i).nametype().name();
  }
  stat << ") VALUES ( ";
  for (int i = 0; i < query.fieldqueries_size(); ++i){
    if (i > 0){
      stat << " , ";
    }
    placeholderBlobs.push_back(query.insertPlaceHolder(i,stat));
  }
  stat << " ); ";

  try {
    stat.execute();
  } catch(const std::exception &e){
    LOG(FATAL) << "Insert failed with exception " << e.what();
  }

  return true;
}

std::shared_ptr<Revision> CRTableInterface::rawGetRow(
    const map_api::Hash &id) const{
  std::shared_ptr<Revision> query = getTemplate();
  Poco::Data::Statement stat(*session_);
  stat << "SELECT ";

  // because protobuf won't supply mutable pointers to numeric values, we can't
  // pass them as reference to the into() binding of Poco::Data; they have to
  // be assigned a posteriori - this is done through this map
  std::map<std::string, double> doublePostApply;
  std::map<std::string, int32_t> intPostApply;
  std::map<std::string, int64_t> longPostApply;
  std::map<std::string, Poco::Data::BLOB> blobPostApply;

  for (int i=0; i<query->fieldqueries_size(); ++i){
    if (i>0){
      stat << ", ";
    }
    proto::TableField& field = *query->mutable_fieldqueries(i);
    stat << field.nametype().name();
    // TODO(simon) do you see a reasonable way to move this to TableField?
    switch(field.nametype().type()){
      case (proto::TableFieldDescriptor_Type_BLOB):{
        stat, Poco::Data::into(blobPostApply[field.nametype().name()]);
        break;
      }
      case (proto::TableFieldDescriptor_Type_DOUBLE):{
        stat, Poco::Data::into(doublePostApply[field.nametype().name()]);
        break;
      }
      case (proto::TableFieldDescriptor_Type_INT32):{
        stat, Poco::Data::into(intPostApply[field.nametype().name()]);
        break;
      }
      case (proto::TableFieldDescriptor_Type_INT64):{
        stat, Poco::Data::into(longPostApply[field.nametype().name()]);
        break;
      }
      case (proto::TableFieldDescriptor_Type_STRING): // Fallthrough intended
      case (proto::TableFieldDescriptor_Type_HASH128):{
        // default string value allows us to see whether a query failed by
        // looking at the ID
        stat, Poco::Data::into(*field.mutable_stringvalue(),
                               std::string(""));
        break;
      }
      default:{
        LOG(FATAL) << "Type of field supplied to select query unknown" <<
            std::endl;
      }
    }
  }

  stat << " FROM " << name() << " WHERE ID LIKE :id",
      Poco::Data::use(id.getString());

  try{
    stat.execute();
  } catch (const std::exception& e){
    LOG(ERROR) << "Statement failed transaction: " << stat.toString();
    return std::shared_ptr<Revision>();
  }

  // indication of empty result
  Hash test;
  if (!query->get<Hash>("ID", &test)){
    LOG(FATAL) << "Field ID seems to be absent";
  }
  if (test.getString() == ""){
    return std::shared_ptr<Revision>();
  }

  // write values that couldn't be written directly
  for (std::pair<std::string, double> fieldDouble : doublePostApply){
    query->set(fieldDouble.first, fieldDouble.second);
  }
  for (std::pair<std::string, int32_t> fieldInt : intPostApply){
    query->set(fieldInt.first, fieldInt.second);
  }
  for (std::pair<std::string, int64_t> fieldLong : longPostApply){
    query->set(fieldLong.first, fieldLong.second);
  }
  for (const std::pair<std::string, Poco::Data::BLOB>& fieldBlob :
      blobPostApply){
    query->set(fieldBlob.first, fieldBlob.second);
  }

  return query;
}

} /* namespace map_api */