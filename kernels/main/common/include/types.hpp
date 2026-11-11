#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace tpx {

using EdgeId = std::uint32_t;
using ConcreteEdgeId = std::uint32_t;
using Vertex = std::uint32_t;
using Time = std::int64_t;
using Clock = std::chrono::steady_clock;
constexpr EdgeId NO_EDGE = std::numeric_limits<EdgeId>::max();
inline bool captureMaterializedPathsForTests = false;

class FlatVertexMap {
public:
    void reserve(std::size_t expected) {
        std::size_t wanted = 8;
        const std::size_t target = expected > (std::numeric_limits<std::size_t>::max() / 2)
            ? std::numeric_limits<std::size_t>::max() : std::max<std::size_t>(8, expected * 2);
        while (wanted < target && wanted <= (std::numeric_limits<std::size_t>::max() >> 1)) wanted <<= 1;
        if (keys_.size() >= wanted) return;
        keys_.assign(wanted, 0);
        values_.assign(wanted, 0);
        stamps_.assign(wanted, 0);
        mask_ = wanted - 1;
        epoch_ = 1;
        size_ = 0;
    }

    void clear() {
        size_ = 0;
        if (keys_.empty()) return;
        ++epoch_;
        if (epoch_ == 0) {
            std::fill(stamps_.begin(), stamps_.end(), 0);
            epoch_ = 1;
        }
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return keys_.size(); }
    std::uint64_t allocatedBytes() const {
        return keys_.capacity() * sizeof(Vertex) +
               values_.capacity() * sizeof(std::uint32_t) +
               stamps_.capacity() * sizeof(std::uint32_t);
    }

    bool find(Vertex key, std::uint32_t& value) const {
        if (keys_.empty()) return false;
        std::size_t slot = bucket(key);
        while (stamps_[slot] == epoch_) {
            if (keys_[slot] == key) {
                value = values_[slot];
                return true;
            }
            slot = (slot + 1) & mask_;
        }
        return false;
    }

    std::optional<std::uint32_t> find(Vertex key) const {
        std::uint32_t value = 0;
        if (!find(key, value)) return std::nullopt;
        return value;
    }

    void insert(Vertex key, std::uint32_t value) {
        if (keys_.empty()) reserve(8);
        if ((size_ + 1) * 10 >= keys_.size() * 7) {
            if (keys_.size() > (std::numeric_limits<std::size_t>::max() >> 1)) {
                throw std::runtime_error("flat vertex map capacity overflow");
            }
            rehash(keys_.size() << 1);
        }
        insertNoGrow(key, value);
    }

private:
    void insertNoGrow(Vertex key, std::uint32_t value) {
        std::size_t slot = bucket(key);
        while (stamps_[slot] == epoch_) {
            if (keys_[slot] == key) {
                values_[slot] = value;
                return;
            }
            slot = (slot + 1) & mask_;
        }
        stamps_[slot] = epoch_;
        keys_[slot] = key;
        values_[slot] = value;
        ++size_;
    }

    void rehash(std::size_t capacity) {
        std::vector<Vertex> oldKeys = std::move(keys_);
        std::vector<std::uint32_t> oldValues = std::move(values_);
        std::vector<std::uint32_t> oldStamps = std::move(stamps_);
        const std::uint32_t oldEpoch = epoch_;
        keys_.assign(capacity, 0);
        values_.assign(capacity, 0);
        stamps_.assign(capacity, 0);
        mask_ = capacity - 1;
        epoch_ = 1;
        size_ = 0;
        for (std::size_t i = 0; i < oldKeys.size(); ++i) {
            if (oldStamps[i] == oldEpoch) insertNoGrow(oldKeys[i], oldValues[i]);
        }
    }

    std::size_t bucket(Vertex key) const {
        std::uint64_t x = static_cast<std::uint64_t>(key) + 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & mask_;
    }
    std::vector<Vertex> keys_;
    std::vector<std::uint32_t> values_;
    std::vector<std::uint32_t> stamps_;
    std::size_t mask_ = 0;
    std::uint32_t epoch_ = 1;
    std::size_t size_ = 0;
};

inline double elapsedMs(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

inline Time safeAdd(Time value, Time delta) {
    if (delta > 0 && value > std::numeric_limits<Time>::max() - delta) {
        return std::numeric_limits<Time>::max();
    }
    if (delta < 0 && value < std::numeric_limits<Time>::min() - delta) {
        return std::numeric_limits<Time>::min();
    }
    return value + delta;
}

inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

class BigUInt {
public:
    BigUInt() = default;
    explicit BigUInt(std::uint64_t value) : small_(value) {}

    void add(std::uint64_t value) {
        if (!big_) {
            if (value <= std::numeric_limits<std::uint64_t>::max() - small_) {
                small_ += value;
                return;
            }
            promote();
        }
        addDigits(value);
    }

    void increment() { add(1); }

    void add(const BigUInt& other) {
        if (!other.big_) {
            add(other.small_);
            return;
        }
        if (!big_) promote();
        addDigitVector(other.digits_);
    }

    std::string str() const {
        if (!big_) return std::to_string(small_);
        if (digits_.empty()) return "0";
        std::ostringstream out;
        out << digits_.back();
        for (std::size_t i = digits_.size() - 1; i > 0; --i) {
            out << std::setfill('0') << std::setw(9) << digits_[i - 1];
        }
        return out.str();
    }

    bool isZero() const { return big_ ? digits_.empty() : small_ == 0; }
    std::uint64_t allocatedBytes() const {
        return static_cast<std::uint64_t>(digits_.capacity()) * sizeof(std::uint32_t);
    }

    bool operator==(const BigUInt& other) const {
        if (!big_ && !other.big_) return small_ == other.small_;
        return str() == other.str();
    }

private:
    static constexpr std::uint64_t BASE = 1000000000ULL;

    void promote() {
        if (big_) return;
        std::uint64_t value = small_;
        while (value != 0) {
            digits_.push_back(static_cast<std::uint32_t>(value % BASE));
            value /= BASE;
        }
        small_ = 0;
        big_ = true;
    }

    void addDigits(std::uint64_t value) {
        std::uint64_t carry = value;
        std::size_t i = 0;
        while (carry != 0) {
            if (i == digits_.size()) digits_.push_back(0);
            const std::uint64_t sum = static_cast<std::uint64_t>(digits_[i]) + carry % BASE;
            digits_[i] = static_cast<std::uint32_t>(sum % BASE);
            carry = carry / BASE + sum / BASE;
            ++i;
        }
    }

    void addDigitVector(const std::vector<std::uint32_t>& other) {
        std::uint64_t carry = 0;
        const std::size_t size = std::max(digits_.size(), other.size());
        digits_.resize(size, 0);
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint64_t rhs = i < other.size() ? other[i] : 0;
            const std::uint64_t sum = static_cast<std::uint64_t>(digits_[i]) + rhs + carry;
            digits_[i] = static_cast<std::uint32_t>(sum % BASE);
            carry = sum / BASE;
        }
        if (carry) digits_.push_back(static_cast<std::uint32_t>(carry));
    }

    std::uint64_t small_ = 0;
    bool big_ = false;
    std::vector<std::uint32_t> digits_;
};
struct TemporalEdge {
    Vertex src = 0;
    Vertex dst = 0;
    Time time = 0;
    std::uint64_t originalId = 0;
};

struct Query {
    Vertex source = 0;
    Vertex target = 0;
    Time x1 = 0;
    Time x2 = 0;
    Time deltaMin = 1;
    Time deltaMax = 1;
    std::uint64_t queryId = 0;
    std::uint64_t seed = 0;
    double gapRatio = 0.0;
    int placementId = -1;
    std::string generationMode;

    Time effectiveMin() const { return std::max<Time>(1, deltaMin); }
    Time points() const { return x2 >= x1 ? x2 - x1 + 1 : 0; }
    bool syntacticallyValid() const {
        return source != target && x1 <= x2 && deltaMin >= 0 && deltaMax >= 0 &&
               deltaMin <= deltaMax;
    }
    bool multiEdgeFeasible() const { return effectiveMin() <= deltaMax; }
    bool hasMovableWindow() const { return deltaMax > 0 && points() > deltaMax; }
};

inline std::uint32_t temporalHopUpperBound(const Query& query) {
    if (!query.syntacticallyValid()) return 0;
    if (!query.multiEdgeFeasible()) return 1;
    const auto value = static_cast<std::uint64_t>(query.x2 - query.x1) /
        static_cast<std::uint64_t>(query.effectiveMin()) + 1ULL;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(value,
        std::numeric_limits<std::uint32_t>::max()));
}

struct Graph {
    std::vector<TemporalEdge> edges;
    std::vector<std::int64_t> externalVertices;
    std::vector<std::uint64_t> outOffsets, inOffsets;
    std::vector<EdgeId> outIds, inIds;
    Time minTime = 0;
    Time maxTime = 0;
    std::uint64_t fingerprint = 0;

    std::size_t vertexCount() const { return externalVertices.size(); }
    std::uint64_t timeSpan() const {
        return edges.empty() ? 0 : static_cast<std::uint64_t>(maxTime - minTime) + 1;
    }
    std::pair<std::uint64_t, std::uint64_t> globalRange(Time low, Time high) const {
        auto first = std::lower_bound(edges.begin(), edges.end(), low,
            [](const TemporalEdge& edge, Time value) { return edge.time < value; });
        auto last = std::upper_bound(first, edges.end(), high,
            [](Time value, const TemporalEdge& edge) { return value < edge.time; });
        return {static_cast<std::uint64_t>(first - edges.begin()),
                static_cast<std::uint64_t>(last - edges.begin())};
    }
    std::pair<std::uint64_t, std::uint64_t> adjacencyRange(
            bool outgoing, Vertex vertex, Time low, Time high) const {
        const auto& offsets = outgoing ? outOffsets : inOffsets;
        const auto& ids = outgoing ? outIds : inIds;
        const std::uint64_t begin = offsets[vertex];
        const std::uint64_t end = offsets[static_cast<std::size_t>(vertex) + 1];
        auto first = std::lower_bound(ids.begin() + static_cast<std::ptrdiff_t>(begin),
            ids.begin() + static_cast<std::ptrdiff_t>(end), low,
            [&](EdgeId id, Time value) { return edges[id].time < value; });
        auto last = std::upper_bound(first,
            ids.begin() + static_cast<std::ptrdiff_t>(end), high,
            [&](Time value, EdgeId id) { return value < edges[id].time; });
        return {static_cast<std::uint64_t>(first - ids.begin()),
                static_cast<std::uint64_t>(last - ids.begin())};
    }
    std::pair<std::uint64_t, std::uint64_t> adjacencyRange(
            const std::vector<EdgeId>& ids, const std::vector<std::uint64_t>& offsets,
            Vertex vertex, Time low, Time high) const {
        const std::uint64_t begin = offsets[vertex];
        const std::uint64_t end = offsets[static_cast<std::size_t>(vertex) + 1];
        auto first = std::lower_bound(ids.begin() + static_cast<std::ptrdiff_t>(begin),
            ids.begin() + static_cast<std::ptrdiff_t>(end), low,
            [&](EdgeId id, Time value) { return edges[id].time < value; });
        auto last = std::upper_bound(first, ids.begin() + static_cast<std::ptrdiff_t>(end), high,
            [&](Time value, EdgeId id) { return value < edges[id].time; });
        return {static_cast<std::uint64_t>(first - ids.begin()),
                static_cast<std::uint64_t>(last - ids.begin())};
    }
    std::uint64_t allocatedBytes() const {
        return edges.capacity() * sizeof(TemporalEdge)
            + externalVertices.capacity() * sizeof(std::int64_t)
            + (outOffsets.capacity() + inOffsets.capacity()) * sizeof(std::uint64_t)
            + (outIds.capacity() + inIds.capacity()) * sizeof(EdgeId);
    }
};

inline std::uint64_t computeGraphFingerprint(const Graph& graph) {
    std::uint64_t hash = splitmix64(graph.edges.size()) ^
        splitmix64(graph.vertexCount() + 0x9e3779b97f4a7c15ULL);
    std::uint64_t position = 1;
    for (const auto& edge : graph.edges) {
        std::uint64_t token = splitmix64(edge.originalId) ^ splitmix64(position++);
        token ^= splitmix64(static_cast<std::uint64_t>(edge.time));
        token ^= splitmix64((static_cast<std::uint64_t>(edge.src) << 32) | edge.dst);
        hash = splitmix64(hash ^ token);
    }
    return hash;
}

inline std::uint64_t graphFingerprint(const Graph& graph) { return graph.fingerprint; }

struct LoadOptions {
    char delimiter = ' ';
    int srcColumn = 0;
    int dstColumn = 1;
    int timeColumn = 2;
    int idColumn = -1;
    bool skipHeader = false;
    std::uint64_t maxBadRows = 0;
};

inline std::vector<std::string> splitFields(const std::string& line, char delimiter) {
    std::vector<std::string> fields;
    if (delimiter == ' ') {
        std::istringstream in(line);
        std::string value;
        while (in >> value) fields.push_back(value);
    } else {
        std::string value;
        std::istringstream in(line);
        while (std::getline(in, value, delimiter)) fields.push_back(value);
    }
    return fields;
}

inline void buildAdjacency(Graph& graph) {
    const std::size_t n = graph.vertexCount();
    graph.outOffsets.assign(n + 1, 0);
    graph.inOffsets.assign(n + 1, 0);
    for (const auto& edge : graph.edges) {
        ++graph.outOffsets[static_cast<std::size_t>(edge.src) + 1];
        ++graph.inOffsets[static_cast<std::size_t>(edge.dst) + 1];
    }
    std::partial_sum(graph.outOffsets.begin(), graph.outOffsets.end(), graph.outOffsets.begin());
    std::partial_sum(graph.inOffsets.begin(), graph.inOffsets.end(), graph.inOffsets.begin());
    graph.outIds.resize(graph.edges.size());
    graph.inIds.resize(graph.edges.size());
    auto outCursor = graph.outOffsets;
    auto inCursor = graph.inOffsets;
    for (EdgeId id = 0; id < graph.edges.size(); ++id) {
        const auto& edge = graph.edges[id];
        graph.outIds[outCursor[edge.src]++] = id;
        graph.inIds[inCursor[edge.dst]++] = id;
    }
}

inline Graph loadTextGraph(const std::string& path, const LoadOptions& options) {
    if (options.srcColumn < 0 || options.dstColumn < 0 || options.timeColumn < 0) {
        throw std::runtime_error("source, destination, and time columns must be nonnegative");
    }
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open graph: " + path);
    struct Raw { std::int64_t src, dst; Time time; std::uint64_t original; };
    std::vector<Raw> raw;
    std::unordered_map<std::int64_t, Vertex> compressed;
    std::vector<std::int64_t> external;
    std::string line;
    if (options.skipHeader) std::getline(input, line);
    std::uint64_t lineId = 0;
    std::uint64_t badRows = 0;
    const int maxColumn = std::max({options.srcColumn, options.dstColumn,
        options.timeColumn, options.idColumn});
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto fields = splitFields(line, options.delimiter);
        if (static_cast<int>(fields.size()) <= maxColumn) {
            if (++badRows > options.maxBadRows) throw std::runtime_error("too many malformed graph rows");
            continue;
        }
        try {
            Raw item;
            item.src = std::stoll(fields[static_cast<std::size_t>(options.srcColumn)]);
            item.dst = std::stoll(fields[static_cast<std::size_t>(options.dstColumn)]);
            item.time = std::stoll(fields[static_cast<std::size_t>(options.timeColumn)]);
            item.original = options.idColumn >= 0
                ? static_cast<std::uint64_t>(std::stoull(fields[static_cast<std::size_t>(options.idColumn)])) : lineId;
            raw.push_back(item);
            ++lineId;
        } catch (const std::exception&) {
            if (++badRows > options.maxBadRows) throw std::runtime_error("too many malformed graph rows");
        }
    }
    if (raw.empty()) throw std::runtime_error("graph contains no parseable edges");
    std::unordered_set<std::uint64_t> originalIds;
    originalIds.reserve(raw.size());
    for (const auto& item : raw) {
        if (!originalIds.insert(item.original).second) {
            throw std::runtime_error("input edge IDs are not globally unique");
        }
    }
    auto vertex = [&](std::int64_t id) {
        auto it = compressed.find(id);
        if (it != compressed.end()) return it->second;
        if (external.size() >= std::numeric_limits<Vertex>::max()) {
            throw std::runtime_error("vertex count exceeds uint32_t");
        }
        Vertex value = static_cast<Vertex>(external.size());
        compressed.emplace(id, value);
        external.push_back(id);
        return value;
    };
    if (raw.size() >= static_cast<std::size_t>(std::numeric_limits<EdgeId>::max())) {
        throw std::runtime_error("edge count exceeds 32-bit internal edge-ID capacity");
    }
    Graph graph;
    graph.edges.reserve(raw.size());
    for (const auto& item : raw) {
        graph.edges.push_back({vertex(item.src), vertex(item.dst), item.time, item.original});
    }
    graph.externalVertices = std::move(external);
    std::stable_sort(graph.edges.begin(), graph.edges.end(), [](const auto& a, const auto& b) {
        if (a.time != b.time) return a.time < b.time;
        return a.originalId < b.originalId;
    });
    graph.minTime = graph.edges.front().time;
    graph.maxTime = graph.edges.back().time;
    buildAdjacency(graph);
    graph.fingerprint = computeGraphFingerprint(graph);
    return graph;
}

template <class T>
inline void writeVector(std::ofstream& out, const std::vector<T>& values) {
    const std::uint64_t size = values.size();
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    if (size) out.write(reinterpret_cast<const char*>(values.data()),
                        static_cast<std::streamsize>(size * sizeof(T)));
}

template <class T>
inline void readVector(std::ifstream& in, std::vector<T>& values) {
    std::uint64_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (size > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
        throw std::runtime_error("corrupt vector length");
    }
    values.resize(static_cast<std::size_t>(size));
    if (size) in.read(reinterpret_cast<char*>(values.data()),
                      static_cast<std::streamsize>(size * sizeof(T)));
    if (!in) throw std::runtime_error("truncated binary cache");
}

inline void saveGraphCache(const Graph& graph, const std::string& path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create graph cache: " + path);
    const std::uint64_t magic = 0x5450584752415033ULL;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&graph.minTime), sizeof(graph.minTime));
    out.write(reinterpret_cast<const char*>(&graph.maxTime), sizeof(graph.maxTime));
    out.write(reinterpret_cast<const char*>(&graph.fingerprint), sizeof(graph.fingerprint));
    writeVector(out, graph.edges);
    writeVector(out, graph.externalVertices);
    writeVector(out, graph.outOffsets);
    writeVector(out, graph.inOffsets);
    writeVector(out, graph.outIds);
    writeVector(out, graph.inIds);
}

inline Graph loadGraphCache(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open graph cache: " + path);
    std::uint64_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x5450584752415033ULL) throw std::runtime_error("invalid or obsolete graph cache");
    Graph graph;
    in.read(reinterpret_cast<char*>(&graph.minTime), sizeof(graph.minTime));
    in.read(reinterpret_cast<char*>(&graph.maxTime), sizeof(graph.maxTime));
    in.read(reinterpret_cast<char*>(&graph.fingerprint), sizeof(graph.fingerprint));
    readVector(in, graph.edges);
    readVector(in, graph.externalVertices);
    readVector(in, graph.outOffsets);
    readVector(in, graph.inOffsets);
    readVector(in, graph.outIds);
    readVector(in, graph.inIds);
    return graph;
}

inline bool endpointAdmissible(const TemporalEdge& edge, const Query& query) {
    return edge.dst != query.source && edge.src != query.target;
}

struct Deadline {
    explicit Deadline(double seconds = 0.0)
        : enabled(seconds > 0.0), end(Clock::now() + std::chrono::duration_cast<Clock::duration>(
              std::chrono::duration<double>(std::max(0.0, seconds)))) {}
    bool pollNow() {
        if (stopped) return true;
        if (enabled && Clock::now() >= end) {
            stopped = true;
        }
        return stopped;
    }
    bool pollSampled() {
        if (stopped) return true;
        if ((++checks & 4095ULL) == 0ULL) return pollNow();
        return false;
    }
    double remainingSeconds() const {
        if (!enabled) return 0.0;
        return std::max(0.0, std::chrono::duration<double>(end - Clock::now()).count());
    }
    bool enabled = false;
    Clock::time_point end;
    std::uint64_t checks = 0;
    bool stopped = false;
};

struct EdgeSpan {
    const ConcreteEdgeId* data = nullptr;
    std::uint32_t length = 0;
};

struct StoredSpanRef {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
};

struct PathView {
    EdgeSpan first;
    EdgeSpan second;

    std::uint64_t length() const {
        return static_cast<std::uint64_t>(first.length) + second.length;
    }
};

struct ResponseTargetReached {};
struct ResponseProbe {
    std::uint64_t target = 0;
    std::uint64_t seen = 0;
    bool reached = false;
    bool stopAtTarget = false;
    Clock::time_point begin = Clock::now();
    double firstTargetMs = -1.0;

    void onAnswer() {
        if (reached || target == 0) return;
        ++seen;
        if (seen == target) {
            firstTargetMs = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            reached = true;

            std::cout << "TPATH_RESPONSE_V1\t" << target << "\t"
                      << std::fixed << std::setprecision(6) << firstTargetMs << '\n';
            std::cout.flush();
            if (stopAtTarget) throw ResponseTargetReached{};
        }
    }
};
inline thread_local ResponseProbe* activeResponseProbe = nullptr;

struct ResultSummary {
    BigUInt count;
    BigUInt outputVolume;
    std::uint64_t minLength = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxLength = 0;

    std::uint64_t deliveryXor = 0;
    std::uint64_t deliverySum = 0;
    std::vector<std::vector<std::uint64_t>> materializedPaths;
    std::uint64_t pendingCount = 0;
    std::uint64_t pendingVolume = 0;

    static inline std::uint64_t spanBoundaryToken(const EdgeSpan& span,
                                                   std::uint64_t salt) {
        if (span.length == 0 || span.data == nullptr) return salt;
        const std::uint64_t first = span.data[0];
        const std::uint64_t last = span.data[span.length - 1];
        return salt ^ (first * 0x9e3779b185ebca87ULL) ^
               (last * 0xc2b2ae3d27d4eb4fULL) ^
               (static_cast<std::uint64_t>(span.length) * 0x165667b19e3779f9ULL);
    }

    inline void addFastRecord(std::uint64_t length) {
        if (activeResponseProbe) activeResponseProbe->onAnswer();
        if (pendingCount == std::numeric_limits<std::uint64_t>::max()) {
            count.add(pendingCount);
            pendingCount = 0;
        }
        ++pendingCount;
        if (length > std::numeric_limits<std::uint64_t>::max() - pendingVolume) {
            outputVolume.add(pendingVolume);
            pendingVolume = 0;
        }
        pendingVolume += length;
        minLength = std::min(minLength, length);
        maxLength = std::max(maxLength, length);
    }

    inline void flushPending() {
        if (pendingCount) { count.add(pendingCount); pendingCount = 0; }
        if (pendingVolume) { outputVolume.add(pendingVolume); pendingVolume = 0; }
    }

    inline void emitView(const PathView& view) {
        const std::uint64_t length = view.length();
        if (captureMaterializedPathsForTests) {
            std::vector<std::uint64_t> materialized;
            materialized.reserve(static_cast<std::size_t>(length));
            for (std::uint32_t i = 0; i < view.first.length; ++i)
                materialized.push_back(view.first.data[i]);
            for (std::uint32_t i = 0; i < view.second.length; ++i)
                materialized.push_back(view.second.data[i]);
            materializedPaths.push_back(std::move(materialized));
        }

        std::uint64_t token = length * 0x27d4eb2f165667c5ULL;
        token ^= spanBoundaryToken(view.first, 0x13198a2e03707344ULL);
        token ^= spanBoundaryToken(view.second, 0xa4093822299f31d0ULL);
        deliveryXor ^= token;
        deliverySum += token;

        addFastRecord(length);
        minLength = std::min(minLength, length);
        maxLength = std::max(maxLength, length);
    }

    inline void emitOriginalPath(const ConcreteEdgeId* ids, std::size_t lengthSize) {
        if (lengthSize > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("path length exceeds PathView uint32 limit");
        const std::uint32_t length = static_cast<std::uint32_t>(lengthSize);
        if (captureMaterializedPathsForTests) {
            std::vector<std::uint64_t> materialized;
            materialized.reserve(length);
            for (std::uint32_t i = 0; i < length; ++i) materialized.push_back(ids[i]);
            materializedPaths.push_back(std::move(materialized));
        }
        if (captureMaterializedPathsForTests) {
            const EdgeSpan first{ids, length};
            std::uint64_t token = static_cast<std::uint64_t>(length) * 0x27d4eb2f165667c5ULL;
            token ^= spanBoundaryToken(first, 0x13198a2e03707344ULL);
            token ^= 0xa4093822299f31d0ULL;
            deliveryXor ^= token;
            deliverySum += token;
        }
        addFastRecord(length);
        minLength = std::min<std::uint64_t>(minLength, length);
        maxLength = std::max<std::uint64_t>(maxLength, length);
    }

    inline void emitOriginalPath(const std::vector<ConcreteEdgeId>& path) {
        emitOriginalPath(path.data(), path.size());
    }

    inline void emitJoinedOriginalSpans(const ConcreteEdgeId* prefix, std::size_t prefixLength,
                                        const ConcreteEdgeId* suffixTail, std::size_t suffixTailLength) {
        if (prefixLength > std::numeric_limits<std::uint32_t>::max() ||
            suffixTailLength > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("joined path span exceeds PathView uint32 limit");
        }
        emitView(PathView{
            {prefix, static_cast<std::uint32_t>(prefixLength)},
            {suffixTail, static_cast<std::uint32_t>(suffixTailLength)}
        });
    }

    inline void emitJoinedRange(const ConcreteEdgeId* prefixArena,
                                const StoredSpanRef* prefixes, std::uint64_t prefixCount,
                                const ConcreteEdgeId* suffixTail, std::uint32_t suffixTailLength) {
        if (prefixCount == 0) return;
        if (captureMaterializedPathsForTests) {
            for (std::uint64_t i = 0; i < prefixCount; ++i) {
                const auto& p = prefixes[i];
                emitJoinedOriginalSpans(prefixArena + p.offset, p.length,
                                        suffixTail, suffixTailLength);
            }
            return;
        }

        std::uint64_t localVolume = 0;
        std::uint64_t localMin = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t localMax = 0;

        for (std::uint64_t i = 0; i < prefixCount; ++i) {
            const auto& p = prefixes[i];
            if (activeResponseProbe) activeResponseProbe->onAnswer();
            const std::uint64_t length = static_cast<std::uint64_t>(p.length) + suffixTailLength;
            if (length > std::numeric_limits<std::uint64_t>::max() - localVolume) {
                outputVolume.add(localVolume);
                localVolume = 0;
            }
            localVolume += length;
            localMin = std::min(localMin, length);
            localMax = std::max(localMax, length);
        }

        minLength = std::min(minLength, localMin);
        maxLength = std::max(maxLength, localMax);

        if (pendingCount > std::numeric_limits<std::uint64_t>::max() - prefixCount) {
            count.add(pendingCount);
            pendingCount = 0;
        }
        pendingCount += prefixCount;
        if (localVolume > std::numeric_limits<std::uint64_t>::max() - pendingVolume) {
            outputVolume.add(pendingVolume);
            pendingVolume = 0;
        }
        pendingVolume += localVolume;
    }

    void merge(const ResultSummary& other) {
        count.add(other.count);
        count.add(other.pendingCount);
        outputVolume.add(other.outputVolume);
        outputVolume.add(other.pendingVolume);
        deliveryXor ^= other.deliveryXor;
        deliverySum += other.deliverySum;
        if (!other.materializedPaths.empty()) {
            materializedPaths.insert(materializedPaths.end(),
                other.materializedPaths.begin(), other.materializedPaths.end());
        }
        if (!other.count.isZero() && other.minLength != std::numeric_limits<std::uint64_t>::max()) {
            minLength = std::min(minLength, other.minLength);
        }
        maxLength = std::max(maxLength, other.maxLength);
    }

    void emitPath(const Graph& graph, const std::vector<EdgeId>& path) {
        std::vector<ConcreteEdgeId> original;
        original.reserve(path.size());
        for (EdgeId id : path)
            original.push_back(static_cast<ConcreteEdgeId>(graph.edges[id].originalId));
        emitOriginalPath(original);
    }

    std::uint64_t allocatedBytes() const {
        std::uint64_t bytes = sizeof(ResultSummary) + count.allocatedBytes() + outputVolume.allocatedBytes();
        bytes += static_cast<std::uint64_t>(materializedPaths.capacity()) * sizeof(std::vector<std::uint64_t>);
        for (const auto& path : materializedPaths)
            bytes += static_cast<std::uint64_t>(path.capacity()) * sizeof(std::uint64_t);
        return bytes;
    }

    void finishEmpty() {
        flushPending();
        if (count.isZero()) minLength = 0;
    }
    std::string fingerprint() const {
        BigUInt finalCount = count;
        BigUInt finalVolume = outputVolume;
        finalCount.add(pendingCount);
        finalVolume.add(pendingVolume);
        std::ostringstream out;
        out << finalCount.str() << ':' << finalVolume.str() << ':' << std::hex
            << deliveryXor << ':' << deliverySum;
        return out.str();
    }
};
inline std::uint64_t peakRssBytes() {
#if defined(__linux__)
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
    }
#endif
    return 0;
}

struct Record {
    std::map<std::string, std::string> values;
    template <class T> void set(const std::string& key, const T& value) {
        std::ostringstream out; out << value; values[key] = out.str();
    }
    void setDouble(const std::string& key, double value) {
        std::ostringstream out; out << std::fixed << std::setprecision(6) << value;
        values[key] = out.str();
    }
    void print() const {
        std::cout << "TPATH_RESULT_V1\n";
        for (const auto& [key, value] : values) {
            std::string clean = value;
            std::replace(clean.begin(), clean.end(), '\n', ' ');
            std::replace(clean.begin(), clean.end(), '\t', ' ');
            std::cout << key << '\t' << clean << '\n';
        }
        std::cout.flush();
    }
};

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key.rfind("--", 0) != 0) continue;
            key = key.substr(2);
            std::string value = "1";
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                value = argv[++i];
            }
            values_[key] = value;
        }
    }
    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto it = values_.find(key); return it == values_.end() ? fallback : it->second;
    }
    std::int64_t i64(const std::string& key, std::int64_t fallback = 0) const {
        const auto value = get(key); return value.empty() ? fallback : std::stoll(value);
    }
    std::uint64_t u64(const std::string& key, std::uint64_t fallback = 0) const {
        const auto value = get(key); return value.empty() ? fallback : std::stoull(value);
    }
    double real(const std::string& key, double fallback = 0.0) const {
        const auto value = get(key); return value.empty() ? fallback : std::stod(value);
    }
    bool flag(const std::string& key, bool fallback = false) const {
        const auto value = get(key); if (value.empty()) return fallback;
        return value == "1" || value == "true" || value == "yes";
    }
private:
    std::unordered_map<std::string, std::string> values_;
};

inline Query queryFromArgs(const Args& args) {
    Query q;
    q.source = static_cast<Vertex>(args.u64("source"));
    q.target = static_cast<Vertex>(args.u64("target"));
    q.x1 = args.i64("x1"); q.x2 = args.i64("x2");
    q.deltaMin = args.i64("delta-min", 1); q.deltaMax = args.i64("delta-max", 1);
    q.queryId = args.u64("query-id"); q.seed = args.u64("query-seed");
    q.gapRatio = args.real("gap-ratio"); q.placementId = static_cast<int>(args.i64("placement-id", -1));
    q.generationMode = args.get("generation-mode");
    return q;
}

inline void addGraphRecord(Record& record, const Graph& graph) {
    record.set("graph_fingerprint", graphFingerprint(graph));
    record.set("graph_vertex_count", graph.vertexCount());
    record.set("graph_edge_count", graph.edges.size());
    record.set("graph_min_time", graph.minTime);
    record.set("graph_max_time", graph.maxTime);
    record.set("graph_time_span", graph.timeSpan());
    record.set("base_graph_bytes", graph.allocatedBytes());
}

inline void addQueryRecord(Record& record, const Graph& graph, const Query& query) {
    record.set("query_id", query.queryId); record.set("query_seed", query.seed);
    record.set("generation_mode", query.generationMode);
    record.set("source_internal_id", query.source); record.set("target_internal_id", query.target);
    record.set("source_external_id", graph.externalVertices.at(query.source));
    record.set("target_external_id", graph.externalVertices.at(query.target));
    record.set("x1", query.x1); record.set("x2", query.x2);
    record.set("query_time_span", query.points());
    record.set("delta_min", query.deltaMin); record.set("delta_max", query.deltaMax);
    record.set("delta_width", std::max<Time>(0, query.deltaMax - query.effectiveMin() + 1));
    record.setDouble("gap_ratio", query.gapRatio); record.set("gap_placement_id", query.placementId);
}

}
