#include "VertexDB/indexing/hash_index.hpp"

#include <gtest/gtest.h>

namespace VertexDB {

TEST(IndexTests, FindsInsertedRowIds) {
    HashIndex index;
    index.insert(Value{42}, 7);
    index.insert(Value{42}, 8);

    const auto rowIds = index.find(Value{42});
    ASSERT_EQ(rowIds.size(), 2U);
    EXPECT_EQ(rowIds[0], 7U);
    EXPECT_EQ(rowIds[1], 8U);
}

} // namespace VertexDB
