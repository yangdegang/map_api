#include <glog/logging.h>
#include <gtest/gtest.h>

#include <multiagent_mapping_common/test/testing_entrypoint.h>

#include "map-api/ipc.h"
#include "map-api/map-api-core.h"

#include "multiprocess_fixture.cpp"

using namespace map_api;

class MultiprocessTest;

TEST_F(MultiprocessTest, LaunchTest) {
  enum Processes {ROOT, SLAVE};
  enum Barriers {BEFORE_COUNT, AFTER_COUNT};
  if (getSubprocessId() == ROOT) {
    EXPECT_EQ(0, MapApiHub::instance().peerSize());
    launchSubprocess(SLAVE);
    IPC::barrier(BEFORE_COUNT, 1);
    EXPECT_EQ(1, MapApiHub::instance().peerSize());
    IPC::barrier(AFTER_COUNT, 1);
  } else {
    IPC::barrier(BEFORE_COUNT, 1);
    IPC::barrier(AFTER_COUNT, 1);
  }
}

MULTIAGENT_MAPPING_UNITTEST_ENTRYPOINT
