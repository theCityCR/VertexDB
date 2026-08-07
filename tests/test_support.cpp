#include "test_support.hpp"

#include <gtest/gtest.h>

namespace VertexDB {

std::filesystem::path makeTempRoot(std::string_view prefix, std::string_view suffix) {
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + std::string(suffix));
}

QueryExecutor makeTempExecutor(std::string_view prefix, std::string_view suffix) {
    const auto root = makeTempRoot(prefix, suffix);
    std::filesystem::remove_all(root);
    return QueryExecutor{root};
}

void seedEmployees(QueryExecutor &executor, Parser &parser, bool indexId, bool indexSalary) {
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0), (3, \"Cara\", 110000.0);"))
                    .success);
    if (indexId) {
        ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    }
    if (indexSalary) {
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE INDEX idx_salary ON Employees(salary);")).success);
    }
}

} // namespace VertexDB
