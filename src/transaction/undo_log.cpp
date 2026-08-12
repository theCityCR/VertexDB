#include "VertexDB/transaction/undo_log.hpp"

namespace VertexDB {

void UndoLog::push(UndoRecord record) { records_.push_back(std::move(record)); }

void UndoLog::clear() { records_.clear(); }

bool UndoLog::empty() const noexcept { return records_.empty(); }

std::size_t UndoLog::size() const noexcept { return records_.size(); }

const std::vector<UndoRecord> &UndoLog::entries() const noexcept { return records_; }

void UndoLog::rewriteTableName(std::string_view oldName, std::string_view newName) {
    const std::string replacement{newName};
    for (auto &record : records_) {
        if (record.tableName == oldName) {
            record.tableName = replacement;
        }
        if (record.renameTo == oldName) {
            record.renameTo = replacement;
        }
    }
}

std::optional<UndoRecord> UndoLog::pop() {
    if (records_.empty()) {
        return std::nullopt;
    }
    UndoRecord record = std::move(records_.back());
    records_.pop_back();
    return record;
}

} // namespace VertexDB
