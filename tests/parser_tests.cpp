#include "VertexDB/parser/parser.hpp"
#include "VertexDB/parser/parse_error.hpp"

#include <gtest/gtest.h>

#include <string>

namespace VertexDB {

TEST(ParserTests, ParsesCreateTable) {
    Parser parser;
    auto query = parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);");

    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &command = std::get<CreateTable>(query);
    EXPECT_EQ(command.name, "Employees");
    ASSERT_EQ(command.columns.size(), 3U);
    EXPECT_EQ(command.columns[0].type, ColumnType::Int);
    EXPECT_EQ(command.columns[1].type, ColumnType::String);
    EXPECT_EQ(command.columns[2].type, ColumnType::Double);
    EXPECT_FALSE(command.columns[0].unique);
    EXPECT_FALSE(command.columns[0].primaryKey);
}

TEST(ParserTests, ParsesSelectWithPredicateAndLimit) {
    Parser parser;
    auto query = parser.parse("SELECT name FROM Employees WHERE salary > 100000.0 LIMIT 5;");

    ASSERT_TRUE(std::holds_alternative<Select>(query));
    const auto &command = std::get<Select>(query);
    EXPECT_EQ(command.table, "Employees");
    ASSERT_EQ(command.columns.size(), 1U);
    EXPECT_EQ(command.columns.front().kind, SelectExpr::Kind::Column);
    EXPECT_EQ(command.columns.front().column, "name");
    ASSERT_TRUE(command.where.has_value());
    EXPECT_EQ(std::get<ComparisonPred>(*command.where).column, "salary");
    ASSERT_TRUE(command.limit.has_value());
    EXPECT_EQ(*command.limit, 5U);
}

TEST(ParserTests, ParsesInListAndKeepsInSubquery) {
    Parser parser;
    auto listQuery = parser.parse("SELECT name FROM Employees WHERE id IN (1, 3, 5);");
    ASSERT_TRUE(std::holds_alternative<Select>(listQuery));
    const auto &listSelect = std::get<Select>(listQuery);
    ASSERT_TRUE(listSelect.where.has_value());
    ASSERT_EQ(predicateKind(*listSelect.where), PredicateKind::InList);
    const auto &inList = std::get<InListPred>(*listSelect.where);
    EXPECT_EQ(inList.column, "id");
    ASSERT_EQ(inList.inValues.size(), 3U);
    EXPECT_EQ(inList.inValues[0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(inList.inValues[2], Value{static_cast<std::int64_t>(5)});

    auto subQuery =
        parser.parse("SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees);");
    ASSERT_TRUE(std::holds_alternative<Select>(subQuery));
    ASSERT_TRUE(std::get<Select>(subQuery).where.has_value());
    EXPECT_EQ(predicateKind(*std::get<Select>(subQuery).where), PredicateKind::InSubquery);
}

TEST(ParserTests, ParsesMultiRowInsertNullableColumnsAndCompoundPredicates) {
    Parser parser;

    auto create = parser.parse("CREATE TABLE People (id INT, nickname STRING NULL);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(create));
    const auto &table = std::get<CreateTable>(create);
    ASSERT_EQ(table.columns.size(), 2U);
    EXPECT_TRUE(table.columns[1].nullable);

    auto insert = parser.parse("INSERT INTO People VALUES (1, \"Al\"), (2, NULL);");
    ASSERT_TRUE(std::holds_alternative<Insert>(insert));
    const auto &insertCommand = std::get<Insert>(insert);
    ASSERT_EQ(insertCommand.rows.size(), 2U);
    EXPECT_TRUE(insertCommand.rows[1][1].isNull());

    auto select = parser.parse("SELECT * FROM People WHERE id = 1 OR (id > 2 AND id < 5);");
    ASSERT_TRUE(std::holds_alternative<Select>(select));
    const auto &selectCommand = std::get<Select>(select);
    ASSERT_TRUE(selectCommand.where.has_value());
    EXPECT_EQ(predicateKind(*selectCommand.where), PredicateKind::Or);
}

TEST(ParserTests, ParsesOrderByAndTransactionCommands) {
    Parser parser;

    auto select = parser.parse("SELECT * FROM Employees ORDER BY salary DESC LIMIT 2;");
    ASSERT_TRUE(std::holds_alternative<Select>(select));
    const auto &command = std::get<Select>(select);
    ASSERT_TRUE(command.orderBy.has_value());
    EXPECT_EQ(command.orderBy->column, "salary");
    EXPECT_FALSE(command.orderBy->ascending);

    EXPECT_TRUE(std::holds_alternative<BeginTransaction>(parser.parse("BEGIN;")));
    EXPECT_TRUE(std::holds_alternative<CommitTransaction>(parser.parse("COMMIT;")));
    EXPECT_TRUE(std::holds_alternative<RollbackTransaction>(parser.parse("ROLLBACK;")));
}

TEST(ParserTests, ParsesTableManagementCommands) {
    Parser parser;

    EXPECT_TRUE(std::holds_alternative<DropTable>(parser.parse("DROP TABLE Employees;")));
    auto dropIndex = parser.parse("DROP INDEX idx_id ON Employees;");
    ASSERT_TRUE(std::holds_alternative<DropIndex>(dropIndex));
    EXPECT_EQ(std::get<DropIndex>(dropIndex).name, "idx_id");
    EXPECT_EQ(std::get<DropIndex>(dropIndex).table, "Employees");
    EXPECT_TRUE(
        std::holds_alternative<RenameTable>(parser.parse("RENAME TABLE Employees TO Staff;")));
    EXPECT_TRUE(std::holds_alternative<ListTables>(parser.parse("LIST TABLES;")));
}

TEST(ParserTests, RejectsTrailingTokens) {
    Parser parser;

    // Alias-aware FROM accepts `FROM t alias`, so trailing junk must follow a completed clause.
    EXPECT_THROW((void)parser.parse("SELECT * FROM Employees WHERE id = 1 unexpected;"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse("SELECT * FROM Employees LIMIT 1 unexpected;"),
                 std::runtime_error);
}

TEST(ParserTests, ParseErrorsIncludeSourcePositions) {
    Parser parser;
    try {
        (void)parser.parse("SELECT * FROM Employees LIMIT 1 unexpected;");
        FAIL() << "expected ParseError";
    } catch (const ParseError &error) {
        EXPECT_EQ(error.line(), 1U);
        EXPECT_GT(error.column(), 1U);
        EXPECT_NE(std::string{error.what()}.find("line 1, column"), std::string::npos);
        EXPECT_NE(std::string{error.what()}.find("unexpected trailing token"), std::string::npos);
    }

    try {
        (void)parser.parse("CREATE\nFOO;");
        FAIL() << "expected ParseError";
    } catch (const ParseError &error) {
        EXPECT_EQ(error.line(), 2U);
        EXPECT_NE(std::string{error.what()}.find("line 2, column"), std::string::npos);
        EXPECT_NE(std::string{error.what()}.find("expected DATABASE, TABLE, or INDEX"),
                  std::string::npos);
    }
}

TEST(ParserTests, ParsesJoinAndPreparedStatements) {
    Parser parser;

    auto join = parser.parse("SELECT * FROM Employees JOIN Departments ON dept_id = id LIMIT 5;");
    ASSERT_TRUE(std::holds_alternative<Select>(join));
    const auto &select = std::get<Select>(join);
    ASSERT_EQ(select.joins.size(), 1U);
    EXPECT_EQ(select.joins[0].table, "Departments");
    EXPECT_EQ(select.joins[0].leftColumn, "dept_id");
    EXPECT_EQ(select.joins[0].rightColumn, "id");
    EXPECT_FALSE(select.joins[0].tableAlias.has_value());

    auto aliasedJoin =
        parser.parse("SELECT e.name, d.dept FROM Employees AS e JOIN Departments AS d ON "
                     "e.dept_id = d.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(aliasedJoin));
    const auto &aliased = std::get<Select>(aliasedJoin);
    ASSERT_TRUE(aliased.tableAlias.has_value());
    EXPECT_EQ(*aliased.tableAlias, "e");
    ASSERT_EQ(aliased.joins.size(), 1U);
    ASSERT_TRUE(aliased.joins[0].tableAlias.has_value());
    EXPECT_EQ(*aliased.joins[0].tableAlias, "d");

    auto qualified =
        parser.parse("SELECT Employees.name, Departments.dept FROM Employees JOIN Departments ON "
                     "Employees.dept_id = Departments.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(qualified));
    const auto &qualifiedSelect = std::get<Select>(qualified);
    ASSERT_EQ(qualifiedSelect.columns.size(), 2U);
    EXPECT_EQ(qualifiedSelect.columns[0].column, "Employees.name");
    EXPECT_EQ(qualifiedSelect.joins[0].leftColumn, "Employees.dept_id");

    auto multiJoin = parser.parse(
        "SELECT * FROM Employees JOIN Departments ON dept_id = id JOIN Offices ON "
        "Departments.id = Offices.dept_id;");
    ASSERT_TRUE(std::holds_alternative<Select>(multiJoin));
    EXPECT_EQ(std::get<Select>(multiJoin).joins.size(), 2U);

    auto aggregate = parser.parse(
        "SELECT dept_id, COUNT(*), SUM(salary) FROM Employees GROUP BY dept_id;");

    ASSERT_TRUE(std::holds_alternative<Select>(aggregate));
    const auto &agg = std::get<Select>(aggregate);
    ASSERT_EQ(agg.columns.size(), 3U);
    EXPECT_EQ(agg.columns[1].kind, SelectExpr::Kind::Aggregate);
    EXPECT_EQ(agg.columns[1].aggregate, AggregateOp::CountStar);
    EXPECT_EQ(agg.columns[2].aggregate, AggregateOp::Sum);
    ASSERT_EQ(agg.groupBy.size(), 1U);
    EXPECT_EQ(agg.groupBy[0], "dept_id");

    EXPECT_TRUE(std::holds_alternative<PrepareStatement>(
        parser.parse("PREPARE by_id AS \"SELECT name FROM Employees WHERE id = ?;\";")));

    auto execute = parser.parse("EXECUTE by_id VALUES (1);");
    ASSERT_TRUE(std::holds_alternative<ExecutePrepared>(execute));
    EXPECT_EQ(std::get<ExecutePrepared>(execute).parameters.size(), 1U);

    EXPECT_TRUE(std::holds_alternative<Analyze>(parser.parse("ANALYZE;")));
    auto analyzeTable = parser.parse("ANALYZE TABLE Employees;");
    ASSERT_TRUE(std::holds_alternative<Analyze>(analyzeTable));
    EXPECT_EQ(std::get<Analyze>(analyzeTable).table, std::optional<std::string>{"Employees"});
}

TEST(ParserTests, ParsesLeftJoinNonEquiLikeAndRegex) {
    Parser parser;

    auto leftJoin = parser.parse(
        "SELECT e.name, d.dept FROM Employees e LEFT JOIN Departments d ON e.dept_id = d.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(leftJoin));
    const auto &lj = std::get<Select>(leftJoin);
    ASSERT_EQ(lj.joins.size(), 1U);
    EXPECT_EQ(lj.joins[0].kind, JoinKind::LeftOuter);
    EXPECT_EQ(lj.joins[0].op, ComparisonOperator::Equal);

    auto nonEqui = parser.parse(
        "SELECT * FROM Employees JOIN Departments ON Employees.salary > Departments.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(nonEqui));
    EXPECT_EQ(std::get<Select>(nonEqui).joins[0].op, ComparisonOperator::Greater);

    auto like = parser.parse("SELECT name FROM Employees WHERE name LIKE \"Al%\";");
    ASSERT_TRUE(std::holds_alternative<Select>(like));
    ASSERT_TRUE(std::get<Select>(like).where.has_value());
    EXPECT_TRUE(std::holds_alternative<LikePred>(*std::get<Select>(like).where));

    auto regex = parser.parse("SELECT name FROM Employees WHERE name ~ \"^A\";");
    ASSERT_TRUE(std::holds_alternative<Select>(regex));
    EXPECT_TRUE(std::holds_alternative<RegexPred>(*std::get<Select>(regex).where));

}

TEST(ParserTests, ParsesRightFullAndCrossJoins) {
    Parser parser;

    auto rightJoin = parser.parse(
        "SELECT * FROM Employees RIGHT JOIN Departments ON Employees.dept_id = Departments.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(rightJoin));
    EXPECT_EQ(std::get<Select>(rightJoin).joins[0].kind, JoinKind::RightOuter);

    auto fullJoin = parser.parse(
        "SELECT * FROM Employees FULL OUTER JOIN Departments ON Employees.dept_id = Departments.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(fullJoin));
    EXPECT_EQ(std::get<Select>(fullJoin).joins[0].kind, JoinKind::FullOuter);

    auto crossJoin = parser.parse("SELECT * FROM Employees CROSS JOIN Departments;");
    ASSERT_TRUE(std::holds_alternative<Select>(crossJoin));
    const auto &cj = std::get<Select>(crossJoin);
    ASSERT_EQ(cj.joins.size(), 1U);
    EXPECT_EQ(cj.joins[0].kind, JoinKind::Cross);
    EXPECT_TRUE(cj.joins[0].leftColumn.empty());
    EXPECT_TRUE(cj.joins[0].rightColumn.empty());
}

TEST(ParserTests, CrossJoinWithOnClauseIsRejected) {
    // Documented: CROSS JOIN has no ON; leftover ON is unexpected trailing syntax.
    Parser parser;
    EXPECT_THROW((void)parser.parse(
                     "SELECT * FROM Employees CROSS JOIN Departments ON Employees.id = "
                     "Departments.id;"),
                 std::runtime_error);
}

TEST(ParserTests, ParenthesizedOrBushyJoinSyntaxIsRejected) {
    // Documented gap: joins beyond left-deep chains / derived tables in JOIN position.
    Parser parser;
    EXPECT_THROW((void)parser.parse(
                     "SELECT * FROM Employees JOIN (Departments JOIN Offices ON "
                     "Departments.office_id = Offices.id) ON Employees.dept_id = Departments.id;"),
                 std::runtime_error);
}

TEST(ParserTests, ParsesExplainAnalyzeSelectAndWith) {
    Parser parser;

    auto plain = parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(plain));
    EXPECT_FALSE(std::get<ExplainQuery>(plain).analyze);

    auto analyze = parser.parse("EXPLAIN ANALYZE SELECT name FROM Employees WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(analyze));
    const auto &analyzed = std::get<ExplainQuery>(analyze);
    EXPECT_TRUE(analyzed.analyze);
    ASSERT_TRUE(std::holds_alternative<Select>(analyzed.query));
    EXPECT_EQ(std::get<Select>(analyzed.query).table, "Employees");

    auto withAnalyze = parser.parse(
        "EXPLAIN ANALYZE WITH high AS (SELECT id, name FROM Employees) "
        "SELECT name FROM high WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(withAnalyze));
    EXPECT_TRUE(std::get<ExplainQuery>(withAnalyze).analyze);
    ASSERT_TRUE(std::holds_alternative<Select>(std::get<ExplainQuery>(withAnalyze).query));
    EXPECT_EQ(std::get<Select>(std::get<ExplainQuery>(withAnalyze).query).table, "high");

    // Standalone ANALYZE remains histogram stats, not EXPLAIN ANALYZE.
    auto histogram = parser.parse("ANALYZE;");
    ASSERT_TRUE(std::holds_alternative<Analyze>(histogram));
}

TEST(ParserTests, ExplainAnalyzeRejectsMutations) {
    Parser parser;
    EXPECT_THROW((void)parser.parse("EXPLAIN ANALYZE UPDATE Employees SET name = \"x\" WHERE id = 1;"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse("EXPLAIN ANALYZE DELETE FROM Employees WHERE id = 1;"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse("EXPLAIN ANALYZE INSERT INTO Employees VALUES (1, \"x\");"),
                 std::runtime_error);
}

TEST(ParserTests, ParsesExplainUpdateDeleteAndInsert) {
    Parser parser;

    auto explainUpdate =
        parser.parse("EXPLAIN UPDATE Employees SET name = \"x\" WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(explainUpdate));
    EXPECT_FALSE(std::get<ExplainQuery>(explainUpdate).analyze);
    ASSERT_TRUE(std::holds_alternative<Update>(std::get<ExplainQuery>(explainUpdate).query));
    EXPECT_EQ(std::get<Update>(std::get<ExplainQuery>(explainUpdate).query).table, "Employees");

    auto explainDelete = parser.parse("EXPLAIN DELETE FROM Employees WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(explainDelete));
    ASSERT_TRUE(std::holds_alternative<Delete>(std::get<ExplainQuery>(explainDelete).query));
    EXPECT_EQ(std::get<Delete>(std::get<ExplainQuery>(explainDelete).query).table, "Employees");

    auto explainInsert =
        parser.parse("EXPLAIN INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", 1.0);");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(explainInsert));
    ASSERT_TRUE(std::holds_alternative<Insert>(std::get<ExplainQuery>(explainInsert).query));
    EXPECT_EQ(std::get<Insert>(std::get<ExplainQuery>(explainInsert).query).table, "Employees");
    EXPECT_EQ(std::get<Insert>(std::get<ExplainQuery>(explainInsert).query).rows.size(), 2U);
}

TEST(ParserTests, ParsesDropDatabase) {
    Parser parser;
    auto drop = parser.parse("DROP DATABASE company;");
    ASSERT_TRUE(std::holds_alternative<DropDatabase>(drop));
    EXPECT_EQ(std::get<DropDatabase>(drop).name, "company");
}

} // namespace VertexDB
