#ifndef MAP_API_UNIQUE_ID_H_
#define MAP_API_UNIQUE_ID_H_

#include <string>
#include <unordered_set>
#include <vector>

#include <glog/logging.h>
#include <sm/hash_id.hpp>

#include "map-api/internal/unique-id.h"

static constexpr int kDefaultIDPrintLength = 10;

namespace map_api {
template <typename IdType>
class UniqueId;
}  // namespace map_api

#define UNIQUE_ID_DEFINE_ID(TypeName)                           \
  class TypeName : public map_api::UniqueId<TypeName> {         \
   public:                                                      \
    TypeName() = default;                                       \
    explicit inline TypeName(const common::proto::Id& id_field) \
        : map_api::UniqueId<TypeName>(id_field) {}              \
  };                                                            \
  typedef std::vector<TypeName> TypeName##List;                 \
  typedef std::unordered_set<TypeName> TypeName##Set;           \
  extern void defineId##__FILE__##__LINE__(void)

#define UNIQUE_ID_DEFINE_IMMUTABLE_ID(TypeName, BaseTypeName)         \
  class TypeName : public map_api::UniqueId<TypeName> {               \
   public:                                                            \
    TypeName() = default;                                             \
    explicit inline TypeName(const common::proto::Id& id_field)       \
        : map_api::UniqueId<TypeName>(id_field) {}                    \
    inline void from##BaseTypeName(const BaseTypeName& landmark_id) { \
      sm::HashId hash_id;                                             \
      landmark_id.toHashId(&hash_id);                                 \
      this->fromHashId(hash_id);                                      \
    }                                                                 \
  };                                                                  \
  typedef std::vector<TypeName> TypeName##List;                       \
  typedef std::unordered_set<TypeName> TypeName##Set;                 \
  extern void defineId##__FILE__##__LINE__(void)

// This macro needs to be called outside of any namespace.
#define UNIQUE_ID_DEFINE_ID_HASH(TypeName)                      \
  namespace std {                                               \
  template <>                                                   \
  struct hash<TypeName> {                                       \
    typedef TypeName argument_type;                             \
    typedef std::size_t value_type;                             \
    value_type operator()(const argument_type& hash_id) const { \
      return hash_id.hashToSizeT();                             \
    }                                                           \
  };                                                            \
  }                                                             \
  extern void defineId##__FILE__##__LINE__(void)

namespace map_api {
template <typename IdType>
void generateId(IdType* id) {
  CHECK_NOTNULL(id);
  uint64_t hash[2];
  internal::generateUnique128BitHash(hash);
  id->fromUint64(hash);
}

template <typename IdType>
IdType createRandomId() {
  IdType id;
  generateId(&id);
  return id;
}

template <typename IdType>
void generateIdFromInt(unsigned int idx, IdType* id) {
  CHECK_NOTNULL(id);
  std::stringstream ss;
  ss << std::setfill('0') << std::setw(32) << idx;
  id->fromHexString(ss.str());
}

// For database internal use only.
class Id : public sm::HashId {
 public:
  Id() = default;
  explicit inline Id(const common::proto::Id& id_field) {
    deserialize(id_field);
  }
  inline void deserialize(const common::proto::Id& id_field) {
    CHECK_EQ(id_field.uint_size(), 2);
    fromUint64(id_field.uint().data());
  }
  inline void serialize(common::proto::Id* id_field) const {
    CHECK_NOTNULL(id_field)->clear_uint();
    id_field->mutable_uint()->Add();
    id_field->mutable_uint()->Add();
    toUint64(id_field->mutable_uint()->mutable_data());
  }

  inline void fromHashId(const sm::HashId& id) {
    static_cast<sm::HashId&>(*this) = id;
  }
  inline void toHashId(sm::HashId* id) const {
    CHECK_NOTNULL(id);
    *id = static_cast<const sm::HashId&>(*this);
  }
  template <typename IdType>
  inline IdType toIdType() const {
    IdType value;
    value.fromHashId(*this);
    return value;
  }
  template <typename GenerateIdType>
  friend void generateId(GenerateIdType* id);

  bool correspondsTo(const common::proto::Id& proto_id) const {
    Id corresponding(proto_id);
    return operator==(corresponding);
  }

 private:
  using sm::HashId::fromUint64;
  using sm::HashId::toUint64;
};

typedef std::unordered_set<Id> IdSet;
typedef std::vector<Id> IdList;

// To be used for general IDs.
template <typename IdType>
class UniqueId : private Id {
 public:
  UniqueId() = default;
  explicit inline UniqueId(const common::proto::Id& id_field) : Id(id_field) {}

  using sm::HashId::hexString;
  using sm::HashId::fromHexString;
  using sm::HashId::hashToSizeT;
  using sm::HashId::isValid;
  using sm::HashId::setInvalid;

  using Id::deserialize;
  using Id::serialize;

  std::ostream& operator<<(std::ostream& os) const {
    return os << hexString().substr(0, kDefaultIDPrintLength);
  }

  inline void fromHashId(const sm::HashId& id) {
    static_cast<sm::HashId&>(*this) = id;
  }

  inline void toHashId(sm::HashId* id) const {
    CHECK_NOTNULL(id);
    *id = static_cast<const sm::HashId&>(*this);
  }

  inline bool operator==(const IdType& other) const {
    return sm::HashId::operator==(other);
  }

  inline bool operator==(const Id& other) const {
    return sm::HashId::operator==(other);
  }

  inline bool operator!=(const IdType& other) const {
    return sm::HashId::operator!=(other);
  }

  inline bool operator!=(const Id& other) const {
    return sm::HashId::operator!=(other);
  }

  inline bool operator<(const IdType& other) const {
    return sm::HashId::operator<(other);
  }

  inline bool operator<(const Id& other) const {
    return sm::HashId::operator<(other);
  }

  template <typename GenerateIdType>
  friend void generateId(GenerateIdType* id);

  // Making base type accessible to select Map API classes.
  friend class NetTable;
  friend class NetTableTransaction;
};

}  // namespace map_api

namespace std {
inline ostream& operator<<(ostream& out, const map_api::Id& hash) {
  out << hash.hexString().substr(0, kDefaultIDPrintLength);
  return out;
}

template <typename IdType>
inline ostream& operator<<(ostream& out,
                           const map_api::UniqueId<IdType>& hash) {
  out << hash.hexString().substr(0, kDefaultIDPrintLength);
  return out;
}

template <>
struct hash<map_api::Id> {
  std::size_t operator()(const map_api::Id& hashId) const {
    return std::hash<std::string>()(hashId.hexString());
  }
};
}  // namespace std

#endif  // MAP_API_UNIQUE_ID_H_
