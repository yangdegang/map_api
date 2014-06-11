#ifndef MAP_API_CORE_HPP
#define MAP_API_CORE_HPP

#include <memory>

#include <gtest/gtest.h>

#include <Poco/Data/Common.h>

#include "map-api/chunk-manager.h"
#include "map-api/cru-table.h"
#include "map-api/id.h"
#include "map-api/map-api-hub.h"
#include "map-api/metatable.h"
#include "core.pb.h"

DECLARE_string(ip_port);

namespace map_api {

/**
 * The map api core class is the first interface between robot application and
 * the map api system. It is a singleton in order to:
 * - Ensure that only one instance of the database is created and used
 * - Ensure that only one thread is present to communicate with other nodes
 */
class MapApiCore final {
 public:
  /**
   * Get singleton instance of Map Api Core
   * TODO(tcies) just make all functions static (thread-safety!)...
   */
  static MapApiCore& instance();
  /**
   * Synchronizes table definition with peers
   * by using standard table operations on the metatable
   */
  bool syncTableDefinition(const proto::TableDescriptor& descriptor);
  /**
   * Initializer
   */
  bool init(const std::string &ipPort);
  /**
   * Check if initialized
   */
  bool isInitialized() const;
  /**
   * Makes the server thread re-enter, disconnects from database
   */
  void kill();

 protected:
  /**
   * Resets the database, clearing all its contents. TO BE USED FOR TESTING
   * ONLY. After a call to this function ALL TABLES MUST BE RE-INITIALIZED.
   * resetDb already re-initializes the metatable
   */
  void resetDb();
  friend class CoreTester;

 private:
  /**
   * Constructor: Creates database if not existing, launches a new thread
   * that takes care of handling requests from other nodes.
   */
  MapApiCore();
  /**
   * Returns a weak pointer to the database session
   */
  std::weak_ptr<Poco::Data::Session> getSession();
  friend class CRTable;
  friend class CRUTable;
  friend class LocalTransaction;
  /**
   * Initializes metatable if not initialized. Unfortunately, the metatable
   * can't be initialized in init, as the initializer of metatable calls init
   * indirectly itself, so there would be an endless recursion.
   */
  inline void ensureMetatable();

  Id owner_;
  /**
   * Session of local database
   */
  std::shared_ptr<Poco::Data::Session> dbSess_;
  /**
   * Hub instance
   */
  MapApiHub &hub_;
  /**
   * Chunk manager instance
   */
  ChunkManager &chunk_manager_;

  /**
   * initialized?
   */
  bool initialized_;
};

class CoreTester {
 protected:
  inline void resetDb() {
    MapApiCore::instance().resetDb();
  }
};

}

#endif  // MAP_API_CORE_HPP
