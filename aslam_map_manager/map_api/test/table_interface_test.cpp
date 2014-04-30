/*
 * table_interface_test.cpp
 *
 *  Created on: Mar 31, 2014
 *      Author: titus
 */
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <Poco/Data/Common.h>
#include <Poco/Data/BLOB.h>
#include <Poco/Data/Statement.h>

#include <map-api/cru-table-interface.h>
#include <map-api/hash.h>
#include <map-api/time.h>

#include "test_table.cpp"

using namespace map_api;

TEST(TableInterFace, initEmpty){
  TestTable table(Hash::randomHash());
  table.init();
  std::shared_ptr<Revision> structure = table.templateForward();
  ASSERT_TRUE(static_cast<bool>(structure));
  EXPECT_EQ(structure->fieldqueries_size(), 3);
}

/**
 **********************************************************
 * TEMPLATED TABLE WITH A SINGLE FIELD OF THE TEMPLATE TYPE
 **********************************************************
 */

template <typename FieldType>
class FieldTestTable : public TestTable{
 public:
  FieldTestTable(const Hash& owner) : TestTable(owner) {}
  virtual bool init(){
    setup("field_test_table");
    return true;
  }
  std::shared_ptr<Revision> prepareInsert(const FieldType& value){
    std::shared_ptr<Revision> query = getTemplate();
    query->set("test_field",value);
    return query;
  }
 protected:
  virtual bool define(){
    addField<FieldType>("test_field");
    return true;
  }
};


/**
 **************************************
 * FIXTURES FOR TYPED TABLE FIELD TESTS
 **************************************
 */
template <typename TestedType>
class FieldTest : public ::testing::Test{
 protected:
  /**
   * Sample data for tests. MUST BE NON-DEFAULT!
   */
  TestedType sample_data_1();
  TestedType sample_data_2();
};

template <>
class FieldTest<std::string> : public ::testing::Test{
 protected:
  std::string sample_data_1(){
    return "Test_string_1";
  }
  std::string sample_data_2(){
    return "Test_string_2";
  }
};
template <>
class FieldTest<double> : public ::testing::Test{
 protected:
  double sample_data_1(){
    return 3.14;
  }
  double sample_data_2(){
    return -3.14;
  }
};
template <>
class FieldTest<int32_t> : public ::testing::Test{
 protected:
  int32_t sample_data_1(){
    return 42;
  }
  int32_t sample_data_2(){
    return -42;
  }
};
template <>
class FieldTest<map_api::Hash> : public ::testing::Test{
 protected:
  map_api::Hash sample_data_1(){
    return map_api::Hash("One hash");
  }
  map_api::Hash sample_data_2(){
    return map_api::Hash("Another hash");
  }
};
template <>
class FieldTest<int64_t> : public ::testing::Test{
 protected:
  int64_t sample_data_1(){
    return 9223372036854775807;
  }
  int64_t sample_data_2(){
    return -9223372036854775807;
  }
};
template <>
class FieldTest<map_api::Time> : public ::testing::Test{
 protected:
  Time sample_data_1(){
    return Time(9223372036854775807);
  }
  Time sample_data_2(){
    return Time(9223372036854775);
  }
};
template <>
class FieldTest<testBlob> : public ::testing::Test{
 protected:
  testBlob sample_data_1(){
    testBlob field;
    *field.mutable_nametype() = map_api::proto::TableFieldDescriptor();
    field.mutable_nametype()->set_name("A name");
    field.mutable_nametype()->
        set_type(map_api::proto::TableFieldDescriptor_Type_DOUBLE);
    field.set_doublevalue(3);
    return field;
  }
  testBlob sample_data_2(){
    testBlob field;
    *field.mutable_nametype() = map_api::proto::TableFieldDescriptor();
    field.mutable_nametype()->set_name("Another name");
    field.mutable_nametype()->
        set_type(map_api::proto::TableFieldDescriptor_Type_INT32);
    field.set_doublevalue(42);
    return field;
  }
};

/**
 *************************
 * TYPED TABLE FIELD TESTS
 *************************
 */

typedef ::testing::Types<testBlob, std::string, int32_t, double,
    map_api::Hash, int64_t, map_api::Time> MyTypes;
TYPED_TEST_CASE(FieldTest, MyTypes);

TYPED_TEST(FieldTest, Init){
  Hash owner = Hash::randomHash();
  FieldTestTable<TypeParam> table(owner);
  table.init();
  std::shared_ptr<Revision> structure = table.templateForward();
  EXPECT_EQ(structure->fieldqueries_size(), 3);
  table.cleanup();
}

/**
 * TODO(tcies) outdated with transaction-centricity, needs update
 *
TYPED_TEST(FieldTest, CreateBeforeInit){
  FieldTestTable<TypeParam> table;
  EXPECT_DEATH(table.insert(this->sample_data_1()),"^");
}

TYPED_TEST(FieldTest, ReadBeforeInit){
  FieldTestTable<TypeParam> table;
  TypeParam value;
  EXPECT_DEATH(table.get(Hash("Give me any hash"), value),"^");
}

TYPED_TEST(FieldTest, CreateRead){
  FieldTestTable<TypeParam> table;
  table.init();
  Hash createTest = table.insert(this->sample_data_1());
  EXPECT_EQ(table.getOwner(), table.owner(createTest));
  TypeParam readValue;
  EXPECT_TRUE(table.get(createTest, readValue));
  EXPECT_EQ(readValue, this->sample_data_1());
  table.cleanup();
}

TYPED_TEST(FieldTest, CreateTwice){
  FieldTestTable<TypeParam> table;
  table.init();
  table.insert(this->sample_data_1());
  EXPECT_DEATH(table.insert(this->sample_data_1()),"^");
}

TYPED_TEST(FieldTest, ReadInexistent){
  FieldTestTable<TypeParam> table;
  table.init();
  TypeParam value;
  EXPECT_FALSE(table.get(Hash("Give me any hash"), value));
  table.cleanup();
}

TYPED_TEST(FieldTest, UpdateBeforeInit){
  FieldTestTable<TypeParam> table;
  EXPECT_DEATH(table.update(Hash("Give me any hash"),
                            this->sample_data_1()),"^");
}

TYPED_TEST(FieldTest, UpdateRead){
  FieldTestTable<TypeParam> table;
  table.init();
  TypeParam readValue;
  Hash updateTest = table.insert(this->sample_data_1());
  EXPECT_TRUE(table.get(updateTest, readValue));
  EXPECT_EQ(readValue, this->sample_data_1());
  EXPECT_TRUE(table.update(updateTest, this->sample_data_2()));
  EXPECT_TRUE(table.get(updateTest, readValue));
  EXPECT_EQ(readValue, this->sample_data_2());
  table.cleanup();
}
*/