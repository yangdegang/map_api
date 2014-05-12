#ifndef REVISION_H_
#define REVISION_H_

#include <map>
#include <memory>

#include <Poco/Data/BLOB.h>
#include <Poco/Data/Statement.h>

#include "core.pb.h"

namespace map_api {

class Revision : public proto::Revision {
 public:
  /**
   * Insert placeholder in SQLite insert statements. Returns blob shared pointer
   * for dynamically created blob objects
   */
  std::shared_ptr<Poco::Data::BLOB>
  insertPlaceHolder(int field, Poco::Data::Statement& stat) const;
  std::shared_ptr<Poco::Data::BLOB>
  insertPlaceHolder(const std::string& field, Poco::Data::Statement& stat)
  const;

  /**
   * Gets protocol buffer enum for type
   */
  template <typename FieldType>
  static map_api::proto::TableFieldDescriptor_Type protobufEnum();
  /**
   * Supporting macro
   */
#define REVISION_ENUM(TYPE, ENUM) \
    template <> \
    map_api::proto::TableFieldDescriptor_Type \
    Revision::protobufEnum<TYPE>() { \
  return ENUM ; \
}

  /**
   * Overriding adding field in order to impose indexing
   */
  void addField(const proto::TableFieldDescriptor& descriptor);

  /**
   * Sets field according to type.
   */
  template <typename FieldType>
  bool set(const std::string& fieldName, const FieldType& value);

  /**
   * Gets field according to type.
   */
  template <typename FieldType>
  bool get(const std::string& fieldName, FieldType* value) const;

  /**
   * Returns true if Revision contains same fields as other
   */
  bool structureMatch(Revision& other);

  /**
   * Overriding parsing from string in order to add indexing.
   */
  bool ParseFromString(const std::string& data);

 private:
  /**
   * Making mutable_fieldqueries private forces use of addField(), which leads
   * to properly indexed data. We couldn't just override mutable_fieldqueries
   * as we need to know the name of the field at index time.
   */
  using proto::Revision::mutable_fieldqueries;
  /**
   * A map of fields for more intuitive access.
   */
  typedef std::map<std::string, int> FieldMap;
  FieldMap fields_;
  /**
   * Access to the map.
   */
  bool find(const std::string& name, proto::TableField** field);
  bool find(const std::string& name, const proto::TableField** field) const;
  /**
   * Sets field according to type.
   */
  template <typename FieldType>
  bool set(proto::TableField& field, const FieldType& value);
  /**
   * Supporting macro
   */
#define REVISION_SET(TYPE) \
    template <> \
    bool Revision::set<TYPE>(proto::TableField& field, const TYPE& value)
  /**
   * Gets field according to type.
   */
  template <typename FieldType>
  bool get(const proto::TableField& field, FieldType* value) const;
  /**
   * Supporting macro
   */
#define REVISION_GET(TYPE) \
    template <> \
    bool Revision::get<TYPE>(const proto::TableField& field, TYPE* value) const

};

/**
 * One Macro to define REVISION_ENUM, _SET and _GET for Protobuf objects
 */
#define REVISION_PROTOBUF(TYPE) \
    REVISION_ENUM(TYPE, proto::TableFieldDescriptor_Type_BLOB) \
    \
    REVISION_SET(TYPE){ \
  field.set_blobvalue(value.SerializeAsString()); \
  return true; \
} \
\
REVISION_GET(TYPE){ \
  bool parsed = value->ParseFromString(field.blobvalue()); \
  if (!parsed) { \
    LOG(ERROR) << "Failed to parse " << #TYPE; \
    return false; \
  } \
  return true; \
} \
extern void __FILE__ ## __LINE__(void)
// in order to swallow the semicolon
// http://gcc.gnu.org/onlinedocs/cpp/Swallowing-the-Semicolon.html
// http://stackoverflow.com/questions/18786848/macro-that-swallows-semicolon-out
// side-of-function

/**
 * A generic, blob-y field type for testing blob insertion
 */
class testBlob : public map_api::proto::TableField{
 public:
  inline bool operator==(const testBlob& other) const{
    if (!this->has_nametype())
      return !other.has_nametype();
    return nametype().name() == other.nametype().name();
  }
};

} /* namespace map_api */

#include "map-api/revision-inl.h"

#endif /* REVISION_H_ */
