#include "VertexDB/persistence/page_image_redo.hpp"

#include "VertexDB/common/binary_io.hpp"

#include <cstring>
#include <stdexcept>
#include <variant>
#include <vector>

namespace VertexDB {
namespace {

constexpr std::uint8_t kNullValueType = 255;
constexpr std::uint8_t kCompositeValueType = 254;
constexpr std::string_view kTruncated = "truncated page image redo payload";

void appendBytes(std::vector<std::byte> &bytes, const void *data, std::size_t size) {
    const auto *raw = static_cast<const std::byte *>(data);
    bytes.insert(bytes.end(), raw, raw + size);
}

template <typename T> void appendPod(std::vector<std::byte> &bytes, const T &value) {
    appendBytes(bytes, &value, sizeof(T));
}

void appendString(std::vector<std::byte> &bytes, std::string_view value) {
    appendPod(bytes, static_cast<std::uint64_t>(value.size()));
    appendBytes(bytes, value.data(), value.size());
}

void appendValue(std::vector<std::byte> &bytes, const Value &value) {
    if (value.isNull()) {
        appendPod(bytes, kNullValueType);
        return;
    }
    if (value.isComposite()) {
        appendPod(bytes, kCompositeValueType);
        const auto &parts = value.compositeParts();
        appendPod(bytes, static_cast<std::uint64_t>(parts.size()));
        for (const auto &part : parts) {
            appendValue(bytes, part);
        }
        return;
    }
    appendPod(bytes, static_cast<std::uint8_t>(value.type()));
    switch (value.type()) {
    case ColumnType::Int:
        appendPod(bytes, std::get<std::int64_t>(value.data()));
        break;
    case ColumnType::Double:
        appendPod(bytes, std::get<double>(value.data()));
        break;
    case ColumnType::String:
        appendString(bytes, std::get<std::string>(value.data()));
        break;
    }
}

std::string readString(std::span<const std::byte> &bytes) {
    const auto size = readPod<std::uint64_t>(bytes, kTruncated);
    if (bytes.size() < size) {
        throw std::runtime_error(std::string(kTruncated));
    }
    std::string value(reinterpret_cast<const char *>(bytes.data()), static_cast<std::size_t>(size));
    bytes = bytes.subspan(static_cast<std::size_t>(size));
    return value;
}

Value readValue(std::span<const std::byte> &bytes) {
    const auto encodedType = readPod<std::uint8_t>(bytes, kTruncated);
    if (encodedType == kNullValueType) {
        return Value{};
    }
    if (encodedType == kCompositeValueType) {
        const auto partCount = readPod<std::uint64_t>(bytes, kTruncated);
        std::vector<Value> parts;
        parts.reserve(static_cast<std::size_t>(partCount));
        for (std::uint64_t i = 0; i < partCount; ++i) {
            parts.push_back(readValue(bytes));
        }
        return Value::composite(std::move(parts));
    }
    switch (static_cast<ColumnType>(encodedType)) {
    case ColumnType::Int:
        return Value{readPod<std::int64_t>(bytes, kTruncated)};
    case ColumnType::Double:
        return Value{readPod<double>(bytes, kTruncated)};
    case ColumnType::String:
        return Value{readString(bytes)};
    }
    throw std::runtime_error("unsupported value type in page image redo payload");
}

void appendBTreeNode(std::vector<std::byte> &bytes, const BTreeNode &node) {
    appendPod(bytes, static_cast<std::uint64_t>(node.pageId));
    appendPod(bytes, static_cast<std::uint8_t>(node.leaf ? 1 : 0));
    appendPod(bytes, static_cast<std::uint64_t>(node.keys.size()));
    for (const auto &key : node.keys) {
        appendValue(bytes, key);
    }
    if (node.leaf) {
        appendPod(bytes, static_cast<std::uint64_t>(node.rowIds.size()));
        for (const auto &rowIds : node.rowIds) {
            appendPod(bytes, static_cast<std::uint64_t>(rowIds.size()));
            for (const auto rowId : rowIds) {
                appendPod(bytes, static_cast<std::uint64_t>(rowId));
            }
        }
        appendPod(bytes, static_cast<std::uint8_t>(node.nextLeaf.has_value() ? 1 : 0));
        if (node.nextLeaf) {
            appendPod(bytes, static_cast<std::uint64_t>(*node.nextLeaf));
        }
    } else {
        appendPod(bytes, static_cast<std::uint64_t>(node.children.size()));
        for (const auto child : node.children) {
            appendPod(bytes, static_cast<std::uint64_t>(child));
        }
    }
}

BTreeNode readBTreeNode(std::span<const std::byte> &bytes) {
    BTreeNode node;
    node.pageId = readPod<std::uint64_t>(bytes, kTruncated);
    node.leaf = readPod<std::uint8_t>(bytes, kTruncated) != 0;
    const auto keyCount = readPod<std::uint64_t>(bytes, kTruncated);
    node.keys.reserve(static_cast<std::size_t>(keyCount));
    for (std::uint64_t i = 0; i < keyCount; ++i) {
        node.keys.push_back(readValue(bytes));
    }
    if (node.leaf) {
        const auto groupCount = readPod<std::uint64_t>(bytes, kTruncated);
        node.rowIds.reserve(static_cast<std::size_t>(groupCount));
        for (std::uint64_t i = 0; i < groupCount; ++i) {
            const auto rowCount = readPod<std::uint64_t>(bytes, kTruncated);
            std::vector<RowId> rowIds;
            rowIds.reserve(static_cast<std::size_t>(rowCount));
            for (std::uint64_t r = 0; r < rowCount; ++r) {
                rowIds.push_back(static_cast<RowId>(readPod<std::uint64_t>(bytes, kTruncated)));
            }
            node.rowIds.push_back(std::move(rowIds));
        }
        if (readPod<std::uint8_t>(bytes, kTruncated) != 0) {
            node.nextLeaf = readPod<std::uint64_t>(bytes, kTruncated);
        }
    } else {
        const auto childCount = readPod<std::uint64_t>(bytes, kTruncated);
        node.children.reserve(static_cast<std::size_t>(childCount));
        for (std::uint64_t i = 0; i < childCount; ++i) {
            node.children.push_back(readPod<std::uint64_t>(bytes, kTruncated));
        }
    }
    return node;
}

void appendBTreeSnapshot(std::vector<std::byte> &bytes, const BTreeIndexSnapshot &snapshot) {
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.maxKeysPerNode));
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.rootPageId));
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.nextPageId));
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.keyCount));
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.freePageIds.size()));
    for (const auto id : snapshot.freePageIds) {
        appendPod(bytes, static_cast<std::uint64_t>(id));
    }
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.nodes.size()));
    for (const auto &node : snapshot.nodes) {
        appendBTreeNode(bytes, node);
    }
}

BTreeIndexSnapshot readBTreeSnapshot(std::span<const std::byte> &bytes) {
    BTreeIndexSnapshot snapshot;
    snapshot.maxKeysPerNode = static_cast<std::size_t>(readPod<std::uint64_t>(bytes, kTruncated));
    snapshot.rootPageId = readPod<std::uint64_t>(bytes, kTruncated);
    snapshot.nextPageId = readPod<std::uint64_t>(bytes, kTruncated);
    snapshot.keyCount = static_cast<std::size_t>(readPod<std::uint64_t>(bytes, kTruncated));
    const auto freeCount = readPod<std::uint64_t>(bytes, kTruncated);
    snapshot.freePageIds.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t i = 0; i < freeCount; ++i) {
        snapshot.freePageIds.push_back(readPod<std::uint64_t>(bytes, kTruncated));
    }
    const auto nodeCount = readPod<std::uint64_t>(bytes, kTruncated);
    snapshot.nodes.reserve(static_cast<std::size_t>(nodeCount));
    for (std::uint64_t i = 0; i < nodeCount; ++i) {
        snapshot.nodes.push_back(readBTreeNode(bytes));
    }
    return snapshot;
}

void appendHashSnapshot(std::vector<std::byte> &bytes, const HashIndexSnapshot &snapshot) {
    appendPod(bytes, static_cast<std::uint8_t>(snapshot.replaceAll ? 1 : 0));
    appendPod(bytes, static_cast<std::uint64_t>(snapshot.buckets.size()));
    for (const auto &[key, rowIds] : snapshot.buckets) {
        appendValue(bytes, key);
        appendPod(bytes, static_cast<std::uint64_t>(rowIds.size()));
        for (const auto rowId : rowIds) {
            appendPod(bytes, static_cast<std::uint64_t>(rowId));
        }
    }
}

HashIndexSnapshot readHashSnapshot(std::span<const std::byte> &bytes) {
    HashIndexSnapshot snapshot;
    snapshot.replaceAll = readPod<std::uint8_t>(bytes, kTruncated) != 0;
    const auto bucketCount = readPod<std::uint64_t>(bytes, kTruncated);
    snapshot.buckets.reserve(static_cast<std::size_t>(bucketCount));
    for (std::uint64_t i = 0; i < bucketCount; ++i) {
        auto key = readValue(bytes);
        const auto rowCount = readPod<std::uint64_t>(bytes, kTruncated);
        std::vector<RowId> rowIds;
        rowIds.reserve(static_cast<std::size_t>(rowCount));
        for (std::uint64_t r = 0; r < rowCount; ++r) {
            rowIds.push_back(static_cast<RowId>(readPod<std::uint64_t>(bytes, kTruncated)));
        }
        snapshot.buckets.emplace_back(std::move(key), std::move(rowIds));
    }
    return snapshot;
}

void appendRecord(std::vector<std::byte> &bytes, const PageImageRedoRecord &record) {
    appendString(bytes, record.tableName);
    appendPod(bytes, static_cast<std::uint8_t>(record.hasHeapMeta ? 1 : 0));
    if (record.hasHeapMeta) {
        appendPod(bytes, record.capacity);
        appendPod(bytes, static_cast<std::uint64_t>(record.freeList.size()));
        for (const auto rowId : record.freeList) {
            appendPod(bytes, static_cast<std::uint64_t>(rowId));
        }
    }
    appendPod(bytes, static_cast<std::uint64_t>(record.heapPages.size()));
    for (const auto &page : record.heapPages) {
        appendPod(bytes, static_cast<std::uint64_t>(page.pageId));
        appendPod(bytes, static_cast<std::uint64_t>(page.bytes.size()));
        if (!page.bytes.empty()) {
            appendBytes(bytes, page.bytes.data(), page.bytes.size());
        }
    }
    appendPod(bytes, static_cast<std::uint64_t>(record.btreeIndexes.size()));
    for (const auto &[name, snapshot] : record.btreeIndexes) {
        appendString(bytes, name);
        appendPod(bytes, static_cast<std::uint8_t>(IndexPageKind::BTree));
        appendBTreeSnapshot(bytes, snapshot);
    }
    appendPod(bytes, static_cast<std::uint64_t>(record.hashIndexes.size()));
    for (const auto &[name, snapshot] : record.hashIndexes) {
        appendString(bytes, name);
        appendPod(bytes, static_cast<std::uint8_t>(IndexPageKind::Hash));
        appendHashSnapshot(bytes, snapshot);
    }
}

PageImageRedoRecord readRecord(std::span<const std::byte> &bytes) {
    PageImageRedoRecord record;
    record.tableName = readString(bytes);
    record.hasHeapMeta = readPod<std::uint8_t>(bytes, kTruncated) != 0;
    if (record.hasHeapMeta) {
        record.capacity = readPod<std::uint64_t>(bytes, kTruncated);
        const auto freeCount = readPod<std::uint64_t>(bytes, kTruncated);
        record.freeList.reserve(static_cast<std::size_t>(freeCount));
        for (std::uint64_t i = 0; i < freeCount; ++i) {
            record.freeList.push_back(static_cast<RowId>(readPod<std::uint64_t>(bytes, kTruncated)));
        }
    }
    const auto heapCount = readPod<std::uint64_t>(bytes, kTruncated);
    record.heapPages.reserve(static_cast<std::size_t>(heapCount));
    for (std::uint64_t i = 0; i < heapCount; ++i) {
        HeapPageImage page;
        page.pageId = static_cast<PageId>(readPod<std::uint64_t>(bytes, kTruncated));
        const auto byteLength = static_cast<std::size_t>(readPod<std::uint64_t>(bytes, kTruncated));
        page.bytes.resize(byteLength);
        if (byteLength > 0) {
            if (bytes.size() < byteLength) {
                throw std::runtime_error(std::string(kTruncated));
            }
            std::memcpy(page.bytes.data(), bytes.data(), byteLength);
            bytes = bytes.subspan(byteLength);
        }
        record.heapPages.push_back(std::move(page));
    }
    const auto btreeCount = readPod<std::uint64_t>(bytes, kTruncated);
    record.btreeIndexes.reserve(static_cast<std::size_t>(btreeCount));
    for (std::uint64_t i = 0; i < btreeCount; ++i) {
        auto name = readString(bytes);
        const auto kind = static_cast<IndexPageKind>(readPod<std::uint8_t>(bytes, kTruncated));
        if (kind != IndexPageKind::BTree) {
            throw std::runtime_error("expected btree index page kind in page image redo");
        }
        record.btreeIndexes.emplace_back(std::move(name), readBTreeSnapshot(bytes));
    }
    const auto hashCount = readPod<std::uint64_t>(bytes, kTruncated);
    record.hashIndexes.reserve(static_cast<std::size_t>(hashCount));
    for (std::uint64_t i = 0; i < hashCount; ++i) {
        auto name = readString(bytes);
        const auto kind = static_cast<IndexPageKind>(readPod<std::uint8_t>(bytes, kTruncated));
        if (kind != IndexPageKind::Hash) {
            throw std::runtime_error("expected hash index page kind in page image redo");
        }
        record.hashIndexes.emplace_back(std::move(name), readHashSnapshot(bytes));
    }
    return record;
}

} // namespace

std::string encodePageImageRedos(std::span<const PageImageRedoRecord> records) {
    if (records.empty()) {
        throw std::invalid_argument("page image redo batch must be non-empty");
    }
    std::vector<std::byte> bytes;
    appendPod(bytes, static_cast<std::uint64_t>(records.size()));
    for (const auto &record : records) {
        appendRecord(bytes, record);
    }
    std::string payload(bytes.size(), '\0');
    std::memcpy(payload.data(), bytes.data(), bytes.size());
    return payload;
}

std::vector<PageImageRedoRecord> decodePageImageRedos(std::string_view payload) {
    std::vector<std::byte> storage(payload.size());
    std::memcpy(storage.data(), payload.data(), payload.size());
    std::span<const std::byte> bytes{storage};

    const auto count = readPod<std::uint64_t>(bytes, kTruncated);
    if (count == 0) {
        throw std::runtime_error("empty page image redo batch in WAL payload");
    }
    std::vector<PageImageRedoRecord> records;
    records.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        records.push_back(readRecord(bytes));
    }
    if (!bytes.empty()) {
        throw std::runtime_error("trailing bytes in page image redo payload");
    }
    return records;
}

} // namespace VertexDB
