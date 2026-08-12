#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/recursive_cte_limits.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VertexDB {

namespace {

QueryExecutor makeExecutor(std::string_view suffix) {
    return makeTempExecutor("vertexdb-nested-", suffix);
}

QueryExecutor makeExecutor() {
    return makeExecutor("nested-sql-tests");
}

} // namespace

TEST(NestedSqlTests, WithRecursiveWalksHierarchy) {
    Parser parser;
    auto executor = makeExecutor("with-recursive");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Nodes (id INT, parent_id INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Nodes VALUES (1, 0, \"root\"), (2, 1, \"child\"), "
                        "(3, 2, \"leaf\"), (4, 1, \"other\");"))
                    .success);

    auto parsed = parser.parse(
        "WITH RECURSIVE tree AS ("
        "SELECT id, parent_id, name FROM Nodes WHERE id = 1 "
        "UNION ALL "
        "SELECT Nodes.id, Nodes.parent_id, Nodes.name FROM Nodes JOIN tree "
        "ON Nodes.parent_id = tree.id"
        ") SELECT name FROM tree ORDER BY id;");
    ASSERT_TRUE(std::holds_alternative<Select>(parsed));
    const auto &select = std::get<Select>(parsed);
    ASSERT_EQ(select.ctes.size(), 1U);
    EXPECT_TRUE(select.ctes[0].recursive);
    ASSERT_TRUE(select.ctes[0].recursiveArm);

    auto result = executor.execute(parsed);
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 4U);
    // ORDER BY id on recursive result — ids 1,2,3,4 in walk order may vary; check membership.
    bool sawRoot = false;
    bool sawChild = false;
    bool sawLeaf = false;
    bool sawOther = false;
    for (const auto &row : result.rows) {
        if (row[0] == Value{"root"}) {
            sawRoot = true;
        }
        if (row[0] == Value{"child"}) {
            sawChild = true;
        }
        if (row[0] == Value{"leaf"}) {
            sawLeaf = true;
        }
        if (row[0] == Value{"other"}) {
            sawOther = true;
        }
    }
    EXPECT_TRUE(sawRoot);
    EXPECT_TRUE(sawChild);
    EXPECT_TRUE(sawLeaf);
    EXPECT_TRUE(sawOther);
}

TEST(NestedSqlTests, WithRecursiveDocumentedRefusals) {
    Parser parser;
    EXPECT_THROW((void)parser.parse(
                     "WITH RECURSIVE t AS (SELECT id FROM Nodes) SELECT id FROM t;"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse(
                     "WITH t AS (SELECT id FROM Nodes WHERE id = 1 UNION ALL "
                     "SELECT Nodes.id FROM Nodes JOIN t ON Nodes.parent_id = t.id) "
                     "SELECT id FROM t;"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse(
                     "WITH RECURSIVE t AS (SELECT id FROM Nodes WHERE id = 1 UNION "
                     "SELECT Nodes.id FROM Nodes JOIN t ON Nodes.parent_id = t.id) "
                     "SELECT id FROM t;"),
                 std::runtime_error);

    // Multiple recursive CTEs in one WITH.
    EXPECT_THROW(
        (void)parser.parse(
            "WITH RECURSIVE a AS (SELECT id FROM Nodes WHERE id = 1 UNION ALL "
            "SELECT Nodes.id FROM Nodes JOIN a ON Nodes.parent_id = a.id), "
            "b AS (SELECT id FROM Nodes WHERE id = 2 UNION ALL "
            "SELECT Nodes.id FROM Nodes JOIN b ON Nodes.parent_id = b.id) "
            "SELECT id FROM a;"),
        std::runtime_error);

    // Recursive arm must reference the CTE name exactly once (0 refs).
    EXPECT_THROW((void)parser.parse(
                     "WITH RECURSIVE t AS (SELECT id FROM Nodes WHERE id = 1 UNION ALL "
                     "SELECT id FROM Nodes WHERE id = 2) SELECT id FROM t;"),
                 std::runtime_error);

    // Recursive arm must reference the CTE name exactly once (2 refs).
    EXPECT_THROW(
        (void)parser.parse(
            "WITH RECURSIVE t AS (SELECT id FROM Nodes WHERE id = 1 UNION ALL "
            "SELECT Nodes.id FROM Nodes JOIN t ON Nodes.parent_id = t.id "
            "JOIN t AS t2 ON Nodes.id = t2.id) SELECT id FROM t;"),
        std::runtime_error);

    // Anchor must not self-reference.
    EXPECT_THROW(
        (void)parser.parse(
            "WITH RECURSIVE t AS (SELECT id FROM t UNION ALL "
            "SELECT Nodes.id FROM Nodes JOIN t ON Nodes.parent_id = t.id) SELECT id FROM t;"),
        std::runtime_error);

    // WITH inside recursive arm is unsupported.
    EXPECT_THROW(
        (void)parser.parse(
            "WITH RECURSIVE t AS (SELECT id FROM Nodes WHERE id = 1 UNION ALL "
            "WITH x AS (SELECT id FROM Nodes) SELECT Nodes.id FROM Nodes JOIN t "
            "ON Nodes.parent_id = t.id) SELECT id FROM t;"),
        std::runtime_error);

    // General set ops outside recursive CTE bodies are unsupported.
    EXPECT_THROW((void)parser.parse("SELECT id FROM Nodes UNION SELECT id FROM Nodes;"),
                 std::runtime_error);
}

TEST(NestedSqlTests, WithRecursiveRowCapRejectsBeforePartialStepInsert) {
    // Desired: oversized recursive steps throw before inserting any of that step's rows.
    ASSERT_EQ(recursiveCteLimits().maxRows, 100000U);
    const auto saved = recursiveCteLimits();
    recursiveCteLimits().maxRows = 5;
    struct Restore {
        RecursiveCteLimits previous;
        ~Restore() { recursiveCteLimits() = previous; }
    } restore{saved};

    Parser parser;
    auto executor = makeExecutor("recursive-row-cap-preinsert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Seed (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Numbers (n INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Seed VALUES (1);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Numbers VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), "
                        "(10);"))
                    .success);

    const auto beforeNumbers =
        executor.execute(parser.parse("SELECT COUNT(*) FROM Numbers;"));
    ASSERT_TRUE(beforeNumbers.success);
    ASSERT_EQ(beforeNumbers.rows.size(), 1U);
    EXPECT_EQ(beforeNumbers.rows[0][0], Value{static_cast<std::int64_t>(10)});

    const auto query = parser.parse(
        "WITH RECURSIVE t AS ("
        "  SELECT id FROM Seed WHERE id = 1 "
        "  UNION ALL "
        "  SELECT Numbers.n FROM Numbers CROSS JOIN t"
        ") SELECT id FROM t;");
    try {
        (void)executor.execute(query);
        FAIL() << "expected WITH RECURSIVE row cap to throw before absorbing the step";
    } catch (const std::runtime_error &ex) {
        EXPECT_NE(std::string_view{ex.what()}.find("maximum row count"), std::string_view::npos)
            << ex.what();
    }

    // Base tables must remain intact; the failed recursive step must not leak mutations.
    const auto afterNumbers =
        executor.execute(parser.parse("SELECT COUNT(*) FROM Numbers;"));
    ASSERT_TRUE(afterNumbers.success);
    ASSERT_EQ(afterNumbers.rows.size(), 1U);
    EXPECT_EQ(afterNumbers.rows[0][0], Value{static_cast<std::int64_t>(10)});
    const auto seed =
        executor.execute(parser.parse("SELECT COUNT(*) FROM Seed;"));
    ASSERT_TRUE(seed.success);
    EXPECT_EQ(seed.rows[0][0], Value{static_cast<std::int64_t>(1)});
}

TEST(NestedSqlTests, WithRecursiveExceedsDocumentedIterationCap) {
    // Intentional v1 limit: default maxIterations is 1000 (sql.md). Lower for a fast throw path.
    ASSERT_EQ(recursiveCteLimits().maxIterations, 1000U);
    ASSERT_EQ(recursiveCteLimits().maxRows, 100000U);

    const auto saved = recursiveCteLimits();
    recursiveCteLimits().maxIterations = 5;
    struct Restore {
        RecursiveCteLimits previous;
        ~Restore() { recursiveCteLimits() = previous; }
    } restore{saved};

    Parser parser;
    auto executor = makeExecutor("recursive-iter-cap");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Seed (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Seed VALUES (1);")).success);

    const auto query = parser.parse(
        "WITH RECURSIVE t AS ("
        "  SELECT id FROM Seed WHERE id = 1 "
        "  UNION ALL "
        "  SELECT id FROM t"
        ") SELECT id FROM t;");
    try {
        (void)executor.execute(query);
        FAIL() << "expected WITH RECURSIVE iteration cap to throw";
    } catch (const std::runtime_error &ex) {
        EXPECT_NE(std::string_view{ex.what()}.find("maximum iteration count"),
                  std::string_view::npos)
            << ex.what();
    }
}

TEST(NestedSqlTests, WithRecursiveExceedsDocumentedRowCap) {
    // Intentional v1 limit: default maxRows is 100000 (sql.md). Lower for a fast throw path.
    ASSERT_EQ(recursiveCteLimits().maxRows, 100000U);

    const auto saved = recursiveCteLimits();
    recursiveCteLimits().maxRows = 10;
    struct Restore {
        RecursiveCteLimits previous;
        ~Restore() { recursiveCteLimits() = previous; }
    } restore{saved};

    Parser parser;
    auto executor = makeExecutor("recursive-row-cap");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Seed (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Numbers (n INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Seed VALUES (1);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Numbers VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), "
                        "(10);"))
                    .success);

    const auto query = parser.parse(
        "WITH RECURSIVE t AS ("
        "  SELECT id FROM Seed WHERE id = 1 "
        "  UNION ALL "
        "  SELECT Numbers.n FROM Numbers CROSS JOIN t"
        ") SELECT id FROM t LIMIT 1;");
    try {
        (void)executor.execute(query);
        FAIL() << "expected WITH RECURSIVE row cap to throw";
    } catch (const std::runtime_error &ex) {
        EXPECT_NE(std::string_view{ex.what()}.find("maximum row count"), std::string_view::npos)
            << ex.what();
    }
}

} // namespace VertexDB
