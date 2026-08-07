#include "tcrdb_detail.hpp"

#include <istream>
#include <utility>
#include <vector>

namespace VertexDB::tcrdb_detail {
namespace {

void writeBTreeNode(std::ostream &out, const BTreeNode &node) {
    writePodDb(out, static_cast<std::uint64_t>(node.pageId));
    writePodDb(out, static_cast<std::uint8_t>(node.leaf ? 1 : 0));
    writePodDb(out, static_cast<std::uint64_t>(node.keys.size()));
    for (const auto &key : node.keys) {
        writeValue(out, key);
    }
    if (node.leaf) {
        writePodDb(out, static_cast<std::uint64_t>(node.rowIds.size()));
        for (const auto &rowIds : node.rowIds) {
            writePodDb(out, static_cast<std::uint64_t>(rowIds.size()));
            for (const auto rowId : rowIds) {
                writePodDb(out, static_cast<std::uint64_t>(rowId));
            }
        }
        writePodDb(out, static_cast<std::uint8_t>(node.nextLeaf.has_value() ? 1 : 0));
        if (node.nextLeaf) {
            writePodDb(out, static_cast<std::uint64_t>(*node.nextLeaf));
        }
    } else {
        writePodDb(out, static_cast<std::uint64_t>(node.children.size()));
        for (const auto child : node.children) {
            writePodDb(out, static_cast<std::uint64_t>(child));
        }
    }
}

BTreeNode readBTreeNode(std::istream &in) {
    BTreeNode node;
    node.pageId = readPodDb<std::uint64_t>(in);
    node.leaf = readPodDb<std::uint8_t>(in) != 0;
    const auto keyCount = readPodDb<std::uint64_t>(in);
    node.keys.reserve(static_cast<std::size_t>(keyCount));
    for (std::uint64_t i = 0; i < keyCount; ++i) {
        node.keys.push_back(readValue(in));
    }
    if (node.leaf) {
        const auto groupCount = readPodDb<std::uint64_t>(in);
        node.rowIds.reserve(static_cast<std::size_t>(groupCount));
        for (std::uint64_t i = 0; i < groupCount; ++i) {
            const auto rowCount = readPodDb<std::uint64_t>(in);
            std::vector<RowId> rowIds;
            rowIds.reserve(static_cast<std::size_t>(rowCount));
            for (std::uint64_t r = 0; r < rowCount; ++r) {
                rowIds.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
            }
            node.rowIds.push_back(std::move(rowIds));
        }
        if (readPodDb<std::uint8_t>(in) != 0) {
            node.nextLeaf = readPodDb<std::uint64_t>(in);
        }
    } else {
        const auto childCount = readPodDb<std::uint64_t>(in);
        node.children.reserve(static_cast<std::size_t>(childCount));
        for (std::uint64_t i = 0; i < childCount; ++i) {
            node.children.push_back(readPodDb<std::uint64_t>(in));
        }
    }
    return node;
}

void writeBTreeSnapshot(std::ostream &out, const BTreeIndexSnapshot &snapshot) {
    writePodDb(out, static_cast<std::uint64_t>(snapshot.maxKeysPerNode));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.rootPageId));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.nextPageId));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.keyCount));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.freePageIds.size()));
    for (const auto id : snapshot.freePageIds) {
        writePodDb(out, static_cast<std::uint64_t>(id));
    }
    writePodDb(out, static_cast<std::uint64_t>(snapshot.nodes.size()));
    for (const auto &node : snapshot.nodes) {
        writeBTreeNode(out, node);
    }
}

BTreeIndexSnapshot readBTreeSnapshot(std::istream &in) {
    BTreeIndexSnapshot snapshot;
    snapshot.maxKeysPerNode = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    snapshot.rootPageId = readPodDb<std::uint64_t>(in);
    snapshot.nextPageId = readPodDb<std::uint64_t>(in);
    snapshot.keyCount = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    const auto freeCount = readPodDb<std::uint64_t>(in);
    snapshot.freePageIds.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t i = 0; i < freeCount; ++i) {
        snapshot.freePageIds.push_back(readPodDb<std::uint64_t>(in));
    }
    const auto nodeCount = readPodDb<std::uint64_t>(in);
    snapshot.nodes.reserve(static_cast<std::size_t>(nodeCount));
    for (std::uint64_t i = 0; i < nodeCount; ++i) {
        snapshot.nodes.push_back(readBTreeNode(in));
    }
    return snapshot;
}

void writeHashSnapshot(std::ostream &out, const HashIndexSnapshot &snapshot) {
    writePodDb(out, static_cast<std::uint64_t>(snapshot.buckets.size()));
    for (const auto &[key, rowIds] : snapshot.buckets) {
        writeValue(out, key);
        writePodDb(out, static_cast<std::uint64_t>(rowIds.size()));
        for (const auto rowId : rowIds) {
            writePodDb(out, static_cast<std::uint64_t>(rowId));
        }
    }
}

HashIndexSnapshot readHashSnapshot(std::istream &in) {
    HashIndexSnapshot snapshot;
    snapshot.replaceAll = true;
    const auto bucketCount = readPodDb<std::uint64_t>(in);
    snapshot.buckets.reserve(static_cast<std::size_t>(bucketCount));
    for (std::uint64_t i = 0; i < bucketCount; ++i) {
        auto key = readValue(in);
        const auto rowCount = readPodDb<std::uint64_t>(in);
        std::vector<RowId> rowIds;
        rowIds.reserve(static_cast<std::size_t>(rowCount));
        for (std::uint64_t r = 0; r < rowCount; ++r) {
            rowIds.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
        }
        snapshot.buckets.emplace_back(std::move(key), std::move(rowIds));
    }
    return snapshot;
}

void writeHistogramBucket(std::ostream &out, const HistogramBucket &bucket) {
    writeValue(out, bucket.lower);
    writeValue(out, bucket.upper);
    writePodDb(out, bucket.rowCount);
    writePodDb(out, bucket.distinctCount);
}

HistogramBucket readHistogramBucket(std::istream &in) {
    HistogramBucket bucket;
    bucket.lower = readValue(in);
    bucket.upper = readValue(in);
    bucket.rowCount = readPodDb<std::uint64_t>(in);
    bucket.distinctCount = readPodDb<std::uint64_t>(in);
    return bucket;
}

} // namespace

void writeIndexPages(std::ostream &out, const Table &table) {
    const auto snapshot = table.exportIndexPages();
    writePodDb(out, static_cast<std::uint64_t>(snapshot.indexes.size()));
    for (const auto &entry : snapshot.indexes) {
        writeString(out, entry.name);
        writeString(out, entry.column);
        writeBTreeSnapshot(out, entry.btree);
        writeHashSnapshot(out, entry.hash);
    }
}

void loadIndexPages(Table &table, std::istream &in) {
    TableIndexStoreSnapshot snapshot;
    const auto indexCount = readPodDb<std::uint64_t>(in);
    snapshot.indexes.reserve(static_cast<std::size_t>(indexCount));
    for (std::uint64_t i = 0; i < indexCount; ++i) {
        IndexStoreSnapshot entry;
        entry.name = readString(in);
        entry.column = readString(in);
        entry.btree = readBTreeSnapshot(in);
        entry.hash = readHashSnapshot(in);
        snapshot.indexes.push_back(std::move(entry));
    }
    table.replaceIndexPages(std::move(snapshot));
}

void writeColumnHistograms(std::ostream &out, const Table &table) {
    writeBytesDb(out, kHistogramMagic.data(), kHistogramMagic.size());
    const auto histograms = table.columnHistograms();
    writePodDb(out, static_cast<std::uint64_t>(histograms.size()));
    for (const auto &histogram : histograms) {
        writeString(out, histogram.column);
        writePodDb(out, histogram.rowCount);
        writePodDb(out, histogram.distinctCount);
        writePodDb(out, static_cast<std::uint64_t>(histogram.buckets.size()));
        for (const auto &bucket : histogram.buckets) {
            writeHistogramBucket(out, bucket);
        }
    }
}

bool tryLoadColumnHistograms(Table &table, std::istream &in) {
    if (!in.good() || in.peek() == std::char_traits<char>::eof()) {
        return false;
    }
    const auto pos = in.tellg();
    std::string magic(kHistogramMagic.size(), '\0');
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != kHistogramMagic) {
        in.clear();
        in.seekg(pos);
        return false;
    }
    const auto histogramCount = readPodDb<std::uint64_t>(in);
    std::vector<ColumnHistogram> histograms;
    histograms.reserve(static_cast<std::size_t>(histogramCount));
    for (std::uint64_t i = 0; i < histogramCount; ++i) {
        ColumnHistogram histogram;
        histogram.column = readString(in);
        histogram.rowCount = readPodDb<std::uint64_t>(in);
        histogram.distinctCount = readPodDb<std::uint64_t>(in);
        const auto bucketCount = readPodDb<std::uint64_t>(in);
        histogram.buckets.reserve(static_cast<std::size_t>(bucketCount));
        for (std::uint64_t b = 0; b < bucketCount; ++b) {
            histogram.buckets.push_back(readHistogramBucket(in));
        }
        histograms.push_back(std::move(histogram));
    }
    table.replaceColumnHistograms(std::move(histograms));
    return true;
}

} // namespace VertexDB::tcrdb_detail
