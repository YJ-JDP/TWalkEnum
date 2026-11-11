#pragma once

#include "types.hpp"

namespace tpx {

struct FilterMetrics {
    double totalMs = 0.0;
    double rangeLocateMs = 0.0;
    double forwardMs = 0.0;
    double backwardMs = 0.0;
    double intersectMs = 0.0;
    std::uint64_t rangeBegin = 0;
    std::uint64_t rangeEnd = 0;
    std::uint64_t rangeEdges = 0;
    std::uint64_t forwardScanned = 0;
    std::uint64_t backwardScanned = 0;
    std::uint64_t forwardAttempts = 0;
    std::uint64_t backwardAttempts = 0;
    std::uint64_t forwardSuccess = 0;
    std::uint64_t backwardSuccess = 0;
    std::uint64_t forwardEdges = 0;
    std::uint64_t backwardEdges = 0;
    std::uint64_t tightEdges = 0;
    std::uint64_t intervalActivations = 0;
    std::uint64_t logicalCandidateReferences = 0;
    std::uint64_t distinctPoolPositionsScanned = 0;
    std::uint8_t maxForwardPoolVisitCount = 0;
    std::uint8_t maxBackwardPoolVisitCount = 0;
    std::uint64_t emptyActivations = 0;
    std::uint64_t workingBytes = 0;
    std::uint64_t queueBytes = 0;
    std::uint64_t outputBytes = 0;
    std::uint64_t peakBytes = 0;
    std::string tightEdgeChecksum;
    bool timedOut = false;
};

inline std::string tightEdgeFingerprint(const Graph& graph, const std::vector<EdgeId>& ids) {
    std::uint64_t hash1 = splitmix64(ids.size());
    std::uint64_t hash2 = splitmix64(ids.size() ^ 0x9e3779b97f4a7c15ULL);
    std::uint64_t position = 0;
    for (EdgeId id : ids) {
        const std::uint64_t original = graph.edges[id].originalId;
        hash1 = splitmix64(hash1 ^ splitmix64(original + (++position * 0x9e3779b97f4a7c15ULL)));
        hash2 += splitmix64(original ^ (position * 0xbf58476d1ce4e5b9ULL));
    }
    std::ostringstream out;
    out << ids.size() << ':' << std::hex << hash1 << ':' << hash2;
    return out.str();
}

struct FilterOutput {
    std::vector<EdgeId> edgeIds;
    FilterMetrics metrics;
};

inline unsigned popcount64(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_popcountll(value));
#else
    unsigned count = 0;
    while (value) { value &= value - 1; ++count; }
    return count;
#endif
}

inline unsigned ctz64(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(value));
#else
    unsigned count = 0;
    while ((value & 1ULL) == 0) { value >>= 1; ++count; }
    return count;
#endif
}

struct WitnessState {
    static constexpr std::uint32_t NONE = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t head = NONE;
    std::uint32_t tail = NONE;
    std::uint32_t cursor = NONE;
};

struct WitnessArena {
    FlatVertexMap vertexToState;
    std::vector<WitnessState> states;
    std::vector<Time> times;
    std::vector<std::uint32_t> next;

    explicit WitnessArena(std::size_t expectedVertices = 0, std::size_t expectedNodes = 0) {
        vertexToState.reserve(std::min<std::size_t>(expectedVertices, 1U << 18));
        states.reserve(expectedVertices);
        times.reserve(expectedNodes);
        next.reserve(expectedNodes);
    }

    void reset() {
        vertexToState.clear();
        states.clear();
        times.clear();
        next.clear();
    }

    WitnessState* find(Vertex vertex) {
        std::uint32_t index = 0;
        if (!vertexToState.find(vertex, index)) return nullptr;
        return &states[index];
    }

    WitnessState& touch(Vertex vertex) {
        std::uint32_t index = 0;
        if (vertexToState.find(vertex, index)) return states[index];
        if (states.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("witness state count exceeds uint32_t");
        }
        index = static_cast<std::uint32_t>(states.size());
        vertexToState.insert(vertex, index);
        states.emplace_back();
        return states.back();
    }

    void append(Vertex vertex, Time time) {
        if (times.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("witness arena exceeds uint32_t");
        }
        WitnessState& state = touch(vertex);
        const std::uint32_t node = static_cast<std::uint32_t>(times.size());
        times.push_back(time);
        next.push_back(WitnessState::NONE);
        if (state.tail == WitnessState::NONE) {
            state.head = node;
        } else {
            next[state.tail] = node;
        }
        if (state.cursor == WitnessState::NONE) state.cursor = node;
        state.tail = node;
    }

    bool containsAscending(Vertex vertex, Time low, Time high) {
        WitnessState* state = find(vertex);
        if (!state) return false;
        while (state->cursor != WitnessState::NONE && times[state->cursor] < low) {
            state->cursor = next[state->cursor];
        }
        return state->cursor != WitnessState::NONE && times[state->cursor] <= high;
    }

    bool containsDescending(Vertex vertex, Time low, Time high) {
        WitnessState* state = find(vertex);
        if (!state) return false;
        while (state->cursor != WitnessState::NONE && times[state->cursor] > high) {
            state->cursor = next[state->cursor];
        }
        return state->cursor != WitnessState::NONE && times[state->cursor] >= low;
    }

    std::uint64_t allocatedBytes() const {
        return vertexToState.allocatedBytes() +
               states.capacity() * sizeof(WitnessState) +
               times.capacity() * sizeof(Time) +
               next.capacity() * sizeof(std::uint32_t);
    }
};

inline FilterOutput slidingTightGraph(const Graph& graph, const Query& query, Deadline& deadline) {
    FilterOutput output;
    auto& metrics = output.metrics;
    const auto totalStart = Clock::now();
    const auto rangeStart = Clock::now();
    const auto queryRange = graph.globalRange(query.x1, query.x2);
    const EdgeId begin = queryRange.first;
    const EdgeId end = queryRange.second;
    metrics.rangeLocateMs = elapsedMs(rangeStart);
    metrics.rangeBegin = begin;
    metrics.rangeEnd = end;
    metrics.rangeEdges = end - begin;
    if (begin == end || !query.syntacticallyValid()) {
        metrics.totalMs = elapsedMs(totalStart);
        return output;
    }

    const std::size_t m = static_cast<std::size_t>(end - begin);
    const std::size_t wordCount = (m + 63U) / 64U;
    std::vector<std::uint64_t> forward(wordCount, 0), backward(wordCount, 0);
    const auto setMark = [begin](std::vector<std::uint64_t>& bits, EdgeId id) {
        const std::uint64_t local = id - begin;
        bits[static_cast<std::size_t>(local >> 6)] |= 1ULL << (local & 63ULL);
    };

    const std::size_t expectedVertices = std::min<std::size_t>(m, graph.vertexCount());
    WitnessArena witnessTimes(expectedVertices, m);

    const auto forwardStart = Clock::now();
    for (EdgeId id = begin; id < end; ++id) {
        if ((metrics.forwardScanned++ & 4095ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            break;
        }
        const auto& edge = graph.edges[id];
        if (!endpointAdmissible(edge, query)) continue;
        bool reachable = edge.src == query.source;
        if (!reachable) {
            ++metrics.forwardAttempts;
            reachable = witnessTimes.containsAscending(
                edge.src, safeAdd(edge.time, -query.deltaMax),
                safeAdd(edge.time, -query.effectiveMin()));
        }
        if (!reachable) continue;
        setMark(forward, id);
        ++metrics.forwardSuccess;
        if (edge.dst != query.target) witnessTimes.append(edge.dst, edge.time);
    }
    metrics.forwardMs = elapsedMs(forwardStart);
    if (!metrics.timedOut) {
        witnessTimes.reset();
        const auto backwardStart = Clock::now();
        for (EdgeId id = end; id-- > begin;) {
            if ((metrics.backwardScanned++ & 4095ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                break;
            }
            const auto& edge = graph.edges[id];
            if (!endpointAdmissible(edge, query)) continue;
            bool reachable = edge.dst == query.target;
            if (!reachable) {
                ++metrics.backwardAttempts;
                const Time low = safeAdd(edge.time, query.effectiveMin());
                const Time high = safeAdd(edge.time, query.deltaMax);
                reachable = witnessTimes.containsDescending(edge.dst, low, high);
            }
            if (!reachable) continue;
            setMark(backward, id);
            ++metrics.backwardSuccess;
            if (edge.src != query.source) witnessTimes.append(edge.src, edge.time);
        }
        metrics.backwardMs = elapsedMs(backwardStart);
    }

    const auto intersectStart = Clock::now();
    if (!metrics.timedOut) {
        output.edgeIds.reserve(std::min(metrics.forwardSuccess, metrics.backwardSuccess));
        for (std::size_t wordIndex = 0; wordIndex < wordCount; ++wordIndex) {
            const std::uint64_t fword = forward[wordIndex];
            const std::uint64_t bword = backward[wordIndex];
            metrics.forwardEdges += popcount64(fword);
            metrics.backwardEdges += popcount64(bword);
            std::uint64_t common = fword & bword;
            while (common) {
                const unsigned bit = ctz64(common);
                const std::uint64_t local = (static_cast<std::uint64_t>(wordIndex) << 6) + bit;
                if (local < m) output.edgeIds.push_back(begin + local);
                common &= common - 1;
            }
        }
    }
    metrics.tightEdges = output.edgeIds.size();
    metrics.intersectMs = elapsedMs(intersectStart);
    metrics.workingBytes = (forward.capacity() + backward.capacity()) * sizeof(std::uint64_t) +
        witnessTimes.allocatedBytes();
    metrics.outputBytes = output.edgeIds.capacity() * sizeof(EdgeId);
    metrics.peakBytes = metrics.workingBytes + metrics.outputBytes;
    metrics.totalMs = elapsedMs(totalStart);
    return output;
}

template <class Visitor>
inline void scanAdjacencyIntervalSequential(
        const Graph& graph,
        const std::vector<EdgeId>& ids,
        const std::vector<std::uint64_t>& offsets,
        Vertex vertex,
        Time low,
        Time high,
        Deadline& deadline,
        Visitor&& visitor,
        std::uint64_t& scanned) {
    const std::uint64_t begin = offsets[vertex];
    const std::uint64_t end = offsets[static_cast<std::size_t>(vertex) + 1];
    for (std::uint64_t p = begin; p < end; ++p) {
        ++scanned;
        if ((scanned & 8191ULL) == 0 && deadline.pollNow()) return;
        const EdgeId id = ids[p];
        const Time time = graph.edges[id].time;
        if (time < low) continue;
        if (time > high) break;
        visitor(id);
        if (deadline.stopped) return;
    }
}

inline FilterOutput bfsTightGraph(const Graph& graph, const Query& query, Deadline& deadline) {
    FilterOutput output;
    auto& metrics = output.metrics;
    const auto totalStart = Clock::now();

    const auto rangeStart = Clock::now();
    const auto queryRange = graph.globalRange(query.x1, query.x2);
    const EdgeId begin = queryRange.first;
    const EdgeId end = queryRange.second;
    metrics.rangeLocateMs = elapsedMs(rangeStart);
    metrics.rangeBegin = begin;
    metrics.rangeEnd = end;
    metrics.rangeEdges = end >= begin ? end - begin : 0;
    if (deadline.stopped) {
        metrics.timedOut = true;
        metrics.totalMs = elapsedMs(totalStart);
        return output;
    }
    if (begin == end || !query.syntacticallyValid()) {
        metrics.totalMs = elapsedMs(totalStart);
        return output;
    }

    const std::size_t m = static_cast<std::size_t>(end - begin);
    std::vector<std::uint8_t> forward(m, 0), backward(m, 0);

    std::vector<EdgeId> queue;
    std::size_t queueHead = 0;

    const auto forwardStart = Clock::now();
    std::uint64_t forwardAdjScanned = 0;
    scanAdjacencyIntervalSequential(
        graph, graph.outIds, graph.outOffsets, query.source,
        query.x1, query.x2, deadline,
        [&](EdgeId id) {
            if (id < begin || id >= end) return;
            const auto& edge = graph.edges[id];
            if (!endpointAdmissible(edge, query)) return;
            if (!forward[id - begin]) {
                forward[id - begin] = 1;
                queue.push_back(id);
                ++metrics.forwardSuccess;
            }
        },
        forwardAdjScanned);

    while (queueHead < queue.size() && !deadline.stopped) {
        const EdgeId current = queue[queueHead++];
        ++metrics.forwardScanned;
        if ((metrics.forwardScanned & 2047ULL) == 0 && deadline.pollNow()) break;
        const auto& edge = graph.edges[current];
        if (edge.dst == query.target) continue;
        const Time low = safeAdd(edge.time, query.effectiveMin());
        const Time high = safeAdd(edge.time, query.deltaMax);
        scanAdjacencyIntervalSequential(
            graph, graph.outIds, graph.outOffsets, edge.dst,
            low, high, deadline,
            [&](EdgeId next) {
                ++metrics.forwardAttempts;
                if (next < begin || next >= end || forward[next - begin] ||
                    !endpointAdmissible(graph.edges[next], query)) return;
                forward[next - begin] = 1;
                queue.push_back(next);
                ++metrics.forwardSuccess;
            },
            forwardAdjScanned);
    }
    if (deadline.stopped) metrics.timedOut = true;
    metrics.forwardScanned += forwardAdjScanned;
    metrics.forwardMs = elapsedMs(forwardStart);

    queue.clear();
    queueHead = 0;
    const auto backwardStart = Clock::now();
    std::uint64_t backwardAdjScanned = 0;
    if (!metrics.timedOut) {
        scanAdjacencyIntervalSequential(
            graph, graph.inIds, graph.inOffsets, query.target,
            query.x1, query.x2, deadline,
            [&](EdgeId id) {
                if (id < begin || id >= end) return;
                const auto& edge = graph.edges[id];
                if (!endpointAdmissible(edge, query)) return;
                if (!backward[id - begin]) {
                    backward[id - begin] = 1;
                    queue.push_back(id);
                    ++metrics.backwardSuccess;
                }
            },
            backwardAdjScanned);

        while (queueHead < queue.size() && !deadline.stopped) {
            const EdgeId current = queue[queueHead++];
            ++metrics.backwardScanned;
            if ((metrics.backwardScanned & 2047ULL) == 0 && deadline.pollNow()) break;
            const auto& edge = graph.edges[current];
            if (edge.src == query.source) continue;
            const Time low = safeAdd(edge.time, -query.deltaMax);
            const Time high = safeAdd(edge.time, -query.effectiveMin());
            scanAdjacencyIntervalSequential(
                graph, graph.inIds, graph.inOffsets, edge.src,
                low, high, deadline,
                [&](EdgeId previous) {
                    ++metrics.backwardAttempts;
                    if (previous < begin || previous >= end || backward[previous - begin] ||
                        !endpointAdmissible(graph.edges[previous], query)) return;
                    backward[previous - begin] = 1;
                    queue.push_back(previous);
                    ++metrics.backwardSuccess;
                },
                backwardAdjScanned);
        }
        if (deadline.stopped) metrics.timedOut = true;
    }
    metrics.backwardScanned += backwardAdjScanned;
    metrics.backwardMs = elapsedMs(backwardStart);

    const auto intersectStart = Clock::now();
    if (!metrics.timedOut) {
        output.edgeIds.reserve(std::min(metrics.forwardSuccess, metrics.backwardSuccess));
        for (std::size_t i = 0; i < m; ++i) {
            metrics.forwardEdges += forward[i] != 0;
            metrics.backwardEdges += backward[i] != 0;
            if (forward[i] && backward[i]) output.edgeIds.push_back(begin + i);
        }
    }
    metrics.tightEdges = output.edgeIds.size();
    metrics.intersectMs = elapsedMs(intersectStart);
    metrics.queueBytes = queue.capacity() * sizeof(EdgeId);
    metrics.workingBytes = (forward.capacity() + backward.capacity()) * sizeof(std::uint8_t) +
        metrics.queueBytes;
    metrics.outputBytes = output.edgeIds.capacity() * sizeof(EdgeId);
    metrics.peakBytes = metrics.workingBytes + metrics.outputBytes;
    metrics.totalMs = elapsedMs(totalStart);
    return output;
}

struct TightGraph {
    const Graph* graph = nullptr;
    Query query;
    std::vector<EdgeId> edgeIds;

    FlatVertexMap localVertex;
    std::vector<std::uint64_t> outOffsets;
    std::vector<std::uint64_t> inOffsets;
    std::vector<EdgeId> outIds;
    std::vector<EdgeId> inIds;
    std::uint64_t bytes = 0;

    std::size_t vertexCount() const { return localVertex.size(); }
    std::optional<std::uint32_t> localOf(Vertex vertex) const {
        return localVertex.find(vertex);
    }
    std::pair<std::uint64_t, std::uint64_t> outRange(Vertex vertex, Time low, Time high) const {
        const auto local = localOf(vertex);
        return local ? graph->adjacencyRange(outIds, outOffsets, *local, low, high)
                     : std::pair<std::uint64_t, std::uint64_t>{0, 0};
    }
    std::pair<std::uint64_t, std::uint64_t> inRange(Vertex vertex, Time low, Time high) const {
        const auto local = localOf(vertex);
        return local ? graph->adjacencyRange(inIds, inOffsets, *local, low, high)
                     : std::pair<std::uint64_t, std::uint64_t>{0, 0};
    }
};

inline TightGraph makeTightGraph(const Graph& graph, const Query& query, std::vector<EdgeId> ids) {
    TightGraph tight;
    tight.graph = &graph; tight.query = query; tight.edgeIds = std::move(ids);
    const std::size_t maxTouchedByEdges = tight.edgeIds.size() > std::numeric_limits<std::size_t>::max() / 2
        ? std::numeric_limits<std::size_t>::max() : tight.edgeIds.size() * 2;
    const std::size_t vertexReserve = std::min(graph.vertexCount(), maxTouchedByEdges);
    tight.localVertex.reserve(std::min<std::size_t>(vertexReserve, 1U << 20));
    std::vector<std::uint64_t> outDegree;
    std::vector<std::uint64_t> inDegree;
    outDegree.reserve(vertexReserve);
    inDegree.reserve(vertexReserve);
    auto touch = [&](Vertex vertex) {
        std::uint32_t existing = 0;
        if (tight.localVertex.find(vertex, existing)) return existing;
        if (tight.localVertex.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("tight graph local vertex count exceeds uint32_t");
        }
        const auto local = static_cast<std::uint32_t>(tight.localVertex.size());
        tight.localVertex.insert(vertex, local);
        outDegree.push_back(0);
        inDegree.push_back(0);
        return local;
    };

    for (EdgeId id : tight.edgeIds) {
        const auto& edge = graph.edges[id];
        ++outDegree[touch(edge.src)];
        ++inDegree[touch(edge.dst)];
    }
    const std::size_t n = tight.localVertex.size();
    tight.outOffsets.assign(n + 1, 0); tight.inOffsets.assign(n + 1, 0);
    for (std::size_t local = 0; local < n; ++local) {
        tight.outOffsets[local + 1] = outDegree[local];
        tight.inOffsets[local + 1] = inDegree[local];
    }
    std::partial_sum(tight.outOffsets.begin(), tight.outOffsets.end(), tight.outOffsets.begin());
    std::partial_sum(tight.inOffsets.begin(), tight.inOffsets.end(), tight.inOffsets.begin());
    std::vector<std::uint64_t>().swap(outDegree);
    std::vector<std::uint64_t>().swap(inDegree);
    tight.outIds.resize(tight.edgeIds.size()); tight.inIds.resize(tight.edgeIds.size());
    {
        auto outCursor = tight.outOffsets, inCursor = tight.inOffsets;
        for (EdgeId id : tight.edgeIds) {
            const auto& edge = graph.edges[id];
            tight.outIds[outCursor[*tight.localVertex.find(edge.src)]++] = id;
            tight.inIds[inCursor[*tight.localVertex.find(edge.dst)]++] = id;
        }
    }
    tight.bytes = tight.edgeIds.capacity() * sizeof(EdgeId) +
        tight.localVertex.allocatedBytes() +
        tight.outOffsets.capacity() * sizeof(std::uint64_t) + tight.inOffsets.capacity() * sizeof(std::uint64_t) +
        tight.outIds.capacity() * sizeof(EdgeId) + tight.inIds.capacity() * sizeof(EdgeId);
    return tight;
}

inline void addFilterRecord(Record& record, const FilterMetrics& m) {
    record.setDouble("preprocess_ms", m.totalMs);
    record.setDouble("range_locate_ms", m.rangeLocateMs);
    record.setDouble("forward_ms", m.forwardMs);
    record.setDouble("backward_ms", m.backwardMs);
    record.setDouble("intersection_ms", m.intersectMs);
    record.set("range_edges", m.rangeEdges);
    record.set("scan_forward", m.forwardScanned); record.set("scan_backward", m.backwardScanned);
    record.set("attempt_forward", m.forwardAttempts); record.set("attempt_backward", m.backwardAttempts);
    record.set("success_forward", m.forwardSuccess); record.set("success_backward", m.backwardSuccess);
    record.set("forward_edges", m.forwardEdges); record.set("backward_edges", m.backwardEdges);
    record.set("tight_edges", m.tightEdges); record.set("tight_edge_checksum", m.tightEdgeChecksum);
    record.set("query_nonempty", m.tightEdges > 0 ? 1 : 0);
    record.set("preprocess_working_bytes", m.workingBytes);
    record.set("preprocess_queue_bytes", m.queueBytes);
    record.set("preprocess_output_bytes", m.outputBytes);
    record.set("preprocess_peak_bytes", m.peakBytes);
    record.set("interval_activations", m.intervalActivations);
    record.set("logical_candidate_references", m.logicalCandidateReferences);
    record.set("distinct_pool_positions_scanned", m.distinctPoolPositionsScanned);
    record.set("empty_activations", m.emptyActivations);
    const double reduction = m.logicalCandidateReferences == 0 ? 0.0 :
        1.0 - static_cast<double>(m.distinctPoolPositionsScanned) /
              static_cast<double>(m.logicalCandidateReferences);
    record.setDouble("overlap_reduction_ratio", reduction);
}

}
