#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace VertexDB {
namespace {

QueryExecutor makeExecutor(std::string_view suffix) {
    return makeTempExecutor("vertexdb-setops-", suffix);
}

[[nodiscard]] std::unordered_set<std::int64_t> intColumn(const QueryResult &result,
                                                         std::size_t col = 0) {
    std::unordered_set<std::int64_t> values;
    for (const auto &row : result.rows) {
        values.insert(std::get<std::int64_t>(row[col].data()));
    }
    return values;
}

} // namespace

TEST(SetOpsTests, UnionDistinctDeduplicatesTopLevel) {
    Parser parser;
    auto executor = makeExecutor("union-distinct");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1), (2), (2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (2), (3);")).success);

    auto result = executor.execute(
        parser.parse("SELECT id FROM A UNION SELECT id FROM B ORDER BY id;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 3U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[1][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(result.rows[2][0], Value{static_cast<std::int64_t>(3)});
}

TEST(SetOpsTests, UnionAllPreservesDuplicates) {
    Parser parser;
    auto executor = makeExecutor("union-all");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1), (2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (2), (3);")).success);

    auto result =
        executor.execute(parser.parse("SELECT id FROM A UNION ALL SELECT id FROM B;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 4U);
    EXPECT_EQ(intColumn(result), (std::unordered_set<std::int64_t>{1, 2, 3}));
}

TEST(SetOpsTests, IntersectAndExceptDistinct) {
    Parser parser;
    auto executor = makeExecutor("intersect-except");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1), (2), (2), (3);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (2), (3), (4);")).success);

    auto intersect = executor.execute(
        parser.parse("SELECT id FROM A INTERSECT SELECT id FROM B ORDER BY id;"));
    ASSERT_TRUE(intersect.success) << intersect.message;
    ASSERT_EQ(intersect.rows.size(), 2U);
    EXPECT_EQ(intersect.rows[0][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(intersect.rows[1][0], Value{static_cast<std::int64_t>(3)});

    auto except = executor.execute(
        parser.parse("SELECT id FROM A EXCEPT SELECT id FROM B ORDER BY id;"));
    ASSERT_TRUE(except.success) << except.message;
    ASSERT_EQ(except.rows.size(), 1U);
    EXPECT_EQ(except.rows[0][0], Value{static_cast<std::int64_t>(1)});
}

TEST(SetOpsTests, IntersectAllAndExceptAllPreserveMultisets) {
    // Desired: INTERSECT ALL / EXCEPT ALL use bag semantics (min / leftover counts).
    Parser parser;
    auto executor = makeExecutor("intersect-except-all");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO A VALUES (1), (2), (2), (2), (3);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (2), (2), (4);")).success);

    auto intersectAll = executor.execute(
        parser.parse("SELECT id FROM A INTERSECT ALL SELECT id FROM B ORDER BY id;"));
    ASSERT_TRUE(intersectAll.success) << intersectAll.message;
    ASSERT_EQ(intersectAll.rows.size(), 2U);
    EXPECT_EQ(intersectAll.rows[0][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(intersectAll.rows[1][0], Value{static_cast<std::int64_t>(2)});

    auto exceptAll = executor.execute(
        parser.parse("SELECT id FROM A EXCEPT ALL SELECT id FROM B ORDER BY id;"));
    ASSERT_TRUE(exceptAll.success) << exceptAll.message;
    ASSERT_EQ(exceptAll.rows.size(), 3U);
    EXPECT_EQ(exceptAll.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(exceptAll.rows[1][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(exceptAll.rows[2][0], Value{static_cast<std::int64_t>(3)});

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT id FROM A INTERSECT ALL SELECT id FROM B;"));
    ASSERT_TRUE(explain.success) << explain.message;
    bool sawIntersectAll = false;
    for (const auto &row : explain.rows) {
        if (row[0].toString() == "intersect all") {
            sawIntersectAll = true;
        }
    }
    EXPECT_TRUE(sawIntersectAll);
}

TEST(SetOpsTests, SetOpsInsideNonRecursiveCte) {
    Parser parser;
    auto executor = makeExecutor("cte-union");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1), (2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (2), (3);")).success);

    auto result = executor.execute(parser.parse(
        "WITH ids AS (SELECT id FROM A UNION SELECT id FROM B) "
        "SELECT id FROM ids ORDER BY id;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 3U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[1][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(result.rows[2][0], Value{static_cast<std::int64_t>(3)});
}

TEST(SetOpsTests, WithRecursiveUnionDeduplicatesCycles) {
    // Desired: bare UNION filters rows already in the working table (cycle-safe).
    Parser parser;
    auto executor = makeExecutor("recursive-union");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Edges (src INT, dst INT);"))
                    .success);
    // Cycle 1 -> 2 -> 1 would infinite-loop under UNION ALL.
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Edges VALUES (1, 2), (2, 1), (2, 3);"))
                    .success);

    auto parsed = parser.parse(
        "WITH RECURSIVE walk AS ("
        "  SELECT dst FROM Edges WHERE src = 1 "
        "  UNION "
        "  SELECT Edges.dst FROM Edges JOIN walk ON Edges.src = walk.dst"
        ") SELECT dst FROM walk ORDER BY dst;");
    ASSERT_TRUE(std::holds_alternative<Select>(parsed));
    const auto &select = std::get<Select>(parsed);
    ASSERT_EQ(select.ctes.size(), 1U);
    EXPECT_TRUE(select.ctes[0].recursive);
    EXPECT_TRUE(select.ctes[0].recursiveDistinct);

    auto result = executor.execute(parsed);
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 3U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[1][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(result.rows[2][0], Value{static_cast<std::int64_t>(3)});
}

TEST(SetOpsTests, InSubqueryWithUnion) {
    Parser parser;
    auto executor = makeExecutor("in-union");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE T (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (3);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO T VALUES (1), (2), (3);")).success);

    auto result = executor.execute(parser.parse(
        "SELECT id FROM T WHERE id IN (SELECT id FROM A UNION SELECT id FROM B) "
        "ORDER BY id;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[1][0], Value{static_cast<std::int64_t>(3)});
}

TEST(SetOpsTests, ExplainReportsSetOperationArms) {
    Parser parser;
    auto executor = makeExecutor("explain-union");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (2);")).success);

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT id FROM A UNION SELECT id FROM B;"));
    ASSERT_TRUE(explain.success) << explain.message;
    bool sawUnion = false;
    bool sawLeft = false;
    for (const auto &row : explain.rows) {
        const auto text = row[0].toString();
        if (text.find("set operation left arm") != std::string::npos) {
            sawLeft = true;
        }
        if (text == "union") {
            sawUnion = true;
        }
    }
    EXPECT_TRUE(sawLeft);
    EXPECT_TRUE(sawUnion);
}

TEST(SetOpsTests, SetOpColumnCountMismatchRejected) {
    Parser parser;
    auto executor = makeExecutor("width-mismatch");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT, name STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1, \"a\");")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (1);")).success);

    EXPECT_THROW((void)executor.execute(parser.parse(
                     "SELECT id, name FROM A UNION SELECT id FROM B;")),
                 std::runtime_error);
}

} // namespace VertexDB
