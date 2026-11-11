#pragma once

#include "preprocess.hpp"

namespace tpx {

struct IndexMetrics {
    std::string level;
    double buildMs = 0.0;
    std::uint64_t bytes = 0;
    std::uint64_t successorReferences = 0;
    std::uint64_t predecessorReferences = 0;
    std::uint64_t groups = 0;
    std::uint64_t annotatedEdges = 0;
    std::uint64_t pools = 0;
    bool timedOut = false;
    bool memoryLimit = false;
};

struct TransitionLayout {
    static constexpr EdgeId NO_RANK = std::numeric_limits<EdgeId>::max();
    std::uint64_t formatVersion = 10;
    std::uint64_t graphHash = 0;
    std::uint64_t edgeCount = 0;
    std::uint64_t vertexCount = 0;
    Time deltaMin = 1;
    Time deltaMax = 1;
    std::uint64_t annotatedEdges = 0;
    std::vector<std::uint32_t> plusOffsets;
    std::vector<std::uint32_t> minusOffsets;
    std::vector<std::uint32_t> plusIds;
    std::vector<std::uint32_t> minusIds;
    std::vector<std::uint64_t> succRanges, predRanges;

    std::vector<std::uint64_t> bridgeWords;
};

inline std::uint64_t packRange32(std::uint32_t left, std::uint32_t right) {
    return (static_cast<std::uint64_t>(left) << 32) | right;
}

inline std::uint32_t rangeLeft(std::uint64_t packed) {
    return static_cast<std::uint32_t>(packed >> 32);
}

inline std::uint32_t rangeRight(std::uint64_t packed) {
    return static_cast<std::uint32_t>(packed);
}

inline bool layoutHasPred(const TransitionLayout& layout, EdgeId id) {
    const auto packed = layout.predRanges[static_cast<std::size_t>(id)];
    return rangeLeft(packed) < rangeRight(packed);
}

inline bool layoutHasSucc(const TransitionLayout& layout, EdgeId id) {
    const auto packed = layout.succRanges[static_cast<std::size_t>(id)];
    return rangeLeft(packed) < rangeRight(packed);
}

inline unsigned trailingZeroCount(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(value));
#else
    unsigned count = 0;
    while ((value & 1ULL) == 0) { value >>= 1; ++count; }
    return count;
#endif
}

inline TransitionLayout buildTransitionLayout(const Graph& graph, Time deltaMin, Time deltaMax,
                                              Deadline& deadline) {
    if (deltaMin < 0 || deltaMax < 0 || deltaMin > deltaMax) {
        throw std::runtime_error("index gap bounds must satisfy 0 <= deltaMin <= deltaMax");
    }
    TransitionLayout layout;
    layout.graphHash = graphFingerprint(graph);
    layout.edgeCount = graph.edges.size();
    layout.vertexCount = graph.vertexCount();
    layout.deltaMin = std::max<Time>(1, deltaMin);
    layout.deltaMax = deltaMax;
    const std::size_t m = graph.edges.size(), n = graph.vertexCount();
    if (m > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Level III index supports at most 2^32-1 edges");
    }
    if (n >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Level III index supports fewer than 2^32-1 vertices");
    }
    if (deadline.pollNow()) return layout;

    layout.plusOffsets.assign(n + 1, 0);
    layout.minusOffsets.assign(n + 1, 0);

    {
        static constexpr std::uint8_t HAS_PRED = 1U;
        static constexpr std::uint8_t HAS_SUCC = 2U;
        std::vector<std::uint8_t> flags(m, 0);

        if (layout.deltaMin <= layout.deltaMax) {
            std::uint64_t processed = 0;
            for (Vertex v = 0; static_cast<std::size_t>(v) < n; ++v) {
                const std::uint64_t inBegin = graph.inOffsets[v], inEnd = graph.inOffsets[v + 1];
                const std::uint64_t outBegin = graph.outOffsets[v], outEnd = graph.outOffsets[v + 1];

                std::uint64_t left = outBegin, right = outBegin;
                for (std::uint64_t p = inBegin; p < inEnd; ++p) {
                    if ((++processed & 8191ULL) == 0 && deadline.pollNow()) return layout;
                    const EdgeId id = graph.inIds[p];
                    const Time low = safeAdd(graph.edges[id].time, layout.deltaMin);
                    const Time high = safeAdd(graph.edges[id].time, layout.deltaMax);
                    while (left < outEnd && graph.edges[graph.outIds[left]].time < low) ++left;
                    if (right < left) right = left;
                    while (right < outEnd && graph.edges[graph.outIds[right]].time <= high) ++right;
                    if (left < right) flags[static_cast<std::size_t>(id)] |= HAS_SUCC;
                }

                left = inBegin; right = inBegin;
                for (std::uint64_t p = outBegin; p < outEnd; ++p) {
                    if ((++processed & 8191ULL) == 0 && deadline.pollNow()) return layout;
                    const EdgeId id = graph.outIds[p];
                    const Time low = safeAdd(graph.edges[id].time, -layout.deltaMax);
                    const Time high = safeAdd(graph.edges[id].time, -layout.deltaMin);
                    while (left < inEnd && graph.edges[graph.inIds[left]].time < low) ++left;
                    if (right < left) right = left;
                    while (right < inEnd && graph.edges[graph.inIds[right]].time <= high) ++right;
                    if (left < right) flags[static_cast<std::size_t>(id)] |= HAS_PRED;
                }
            }
        }

        for (EdgeId id = 0; id < m; ++id) {
            if ((id & 8191ULL) == 0 && deadline.pollNow()) return layout;
            const auto& edge = graph.edges[id];
            const std::uint8_t state = flags[static_cast<std::size_t>(id)];
            const bool hasPred = (state & HAS_PRED) != 0;
            const bool hasSucc = (state & HAS_SUCC) != 0;
            if (hasPred) ++layout.plusOffsets[static_cast<std::size_t>(edge.src) + 1];
            if (hasSucc) ++layout.minusOffsets[static_cast<std::size_t>(edge.dst) + 1];
            layout.annotatedEdges += static_cast<std::uint64_t>(hasPred) + static_cast<std::uint64_t>(hasSucc);
        }
        std::partial_sum(layout.plusOffsets.begin(), layout.plusOffsets.end(), layout.plusOffsets.begin());
        std::partial_sum(layout.minusOffsets.begin(), layout.minusOffsets.end(), layout.minusOffsets.begin());
        layout.plusIds.resize(layout.plusOffsets.back());
        layout.minusIds.resize(layout.minusOffsets.back());
        layout.bridgeWords.assign((m + 63U) / 64U, 0);

        auto plusCursor = layout.plusOffsets;
        auto minusCursor = layout.minusOffsets;
        for (EdgeId id = 0; id < m; ++id) {
            if ((id & 8191ULL) == 0 && deadline.pollNow()) return layout;
            const auto& edge = graph.edges[id];
            const std::uint8_t state = flags[static_cast<std::size_t>(id)];
            const bool hasPred = (state & HAS_PRED) != 0;
            const bool hasSucc = (state & HAS_SUCC) != 0;
            if (hasPred) {
                const std::uint32_t rank = plusCursor[edge.src]++;
                layout.plusIds[rank] = static_cast<std::uint32_t>(id);
            }
            if (hasSucc) {
                const std::uint32_t rank = minusCursor[edge.dst]++;
                layout.minusIds[rank] = static_cast<std::uint32_t>(id);
            }
            if (hasPred && hasSucc) {
                layout.bridgeWords[static_cast<std::size_t>(id >> 6)] |=
                    1ULL << (id & 63ULL);
            }
        }
    }

    layout.succRanges.assign(m, 0);
    layout.predRanges.assign(m, 0);
    std::uint64_t boundaryProcessed = 0;
    for (Vertex v = 0; static_cast<std::size_t>(v) < n; ++v) {
        std::uint32_t left = layout.plusOffsets[v], right = left;
        for (std::uint64_t p = graph.inOffsets[v]; p < graph.inOffsets[v + 1]; ++p) {
            if ((++boundaryProcessed & 8191ULL) == 0 && deadline.pollNow()) return layout;
            const EdgeId id = graph.inIds[p];
            const Time low = safeAdd(graph.edges[id].time, layout.deltaMin);
            const Time high = safeAdd(graph.edges[id].time, layout.deltaMax);
            while (left < layout.plusOffsets[v + 1] && graph.edges[layout.plusIds[left]].time < low) ++left;
            if (right < left) right = left;
            while (right < layout.plusOffsets[v + 1] && graph.edges[layout.plusIds[right]].time <= high) ++right;
            layout.succRanges[id] = packRange32(left, right);
        }
        left = layout.minusOffsets[v]; right = left;
        for (std::uint64_t p = graph.outOffsets[v]; p < graph.outOffsets[v + 1]; ++p) {
            if ((++boundaryProcessed & 8191ULL) == 0 && deadline.pollNow()) return layout;
            const EdgeId id = graph.outIds[p];
            const Time low = safeAdd(graph.edges[id].time, -layout.deltaMax);
            const Time high = safeAdd(graph.edges[id].time, -layout.deltaMin);
            while (left < layout.minusOffsets[v + 1] && graph.edges[layout.minusIds[left]].time < low) ++left;
            if (right < left) right = left;
            while (right < layout.minusOffsets[v + 1] && graph.edges[layout.minusIds[right]].time <= high) ++right;
            layout.predRanges[id] = packRange32(left, right);
        }
    }
    return layout;
}

inline std::uint64_t layoutBytes(const TransitionLayout& l) {
    return (l.plusOffsets.capacity() + l.minusOffsets.capacity() +
            l.plusIds.capacity() + l.minusIds.capacity()) * sizeof(std::uint32_t) +
           (l.succRanges.capacity() + l.predRanges.capacity() +
            l.bridgeWords.capacity()) * sizeof(std::uint64_t);
}

inline void validateLevel3Shape(const TransitionLayout& layout) {
    const std::size_t m = static_cast<std::size_t>(layout.edgeCount);
    const std::size_t n = static_cast<std::size_t>(layout.vertexCount);
    if (layout.succRanges.size() != m || layout.predRanges.size() != m ||
        layout.plusOffsets.size() != n + 1 || layout.minusOffsets.size() != n + 1 ||
        layout.plusOffsets.empty() || layout.minusOffsets.empty() ||
        layout.plusOffsets.back() != layout.plusIds.size() ||
        layout.minusOffsets.back() != layout.minusIds.size() ||
        layout.bridgeWords.size() != (m + 63U) / 64U) {
        throw std::runtime_error("corrupt or incomplete Level III index layout");
    }
}

struct Level3PoolState {
    std::uint32_t frontier = 0;
    std::uint32_t clip = 0;
    std::uint32_t epoch = 0;
};

struct Level3Index {
    TransitionLayout layout;
    IndexMetrics metrics;

    mutable std::vector<Level3PoolState> queryPoolStates;
    mutable std::uint32_t queryPoolEpoch = 0;
    mutable std::vector<std::uint64_t> forwardWords;
    mutable std::vector<std::uint64_t> backwardWords;
};

inline void initializeLevel3Workspace(const Level3Index& index) {
    if (index.queryPoolStates.size() != static_cast<std::size_t>(index.layout.vertexCount)) {
        index.queryPoolStates.assign(static_cast<std::size_t>(index.layout.vertexCount), Level3PoolState{});
        index.queryPoolEpoch = 0;
    }
}

inline std::uint32_t nextLevel3WorkspaceEpoch(const Level3Index& index) {
    initializeLevel3Workspace(index);
    ++index.queryPoolEpoch;
    if (index.queryPoolEpoch == 0) {
        for (auto& state : index.queryPoolStates) state.epoch = 0;
        index.queryPoolEpoch = 1;
    }
    return index.queryPoolEpoch;
}

inline Level3Index buildLevel3(const Graph& graph, Time dmin, Time dmax, Deadline& deadline) {
    const auto start = Clock::now();
    Level3Index index;
    index.metrics.level = "L3_SharedPoolMonotoneFrontier";
    index.layout = buildTransitionLayout(graph, dmin, dmax, deadline);
    index.metrics.timedOut = deadline.stopped;
    index.metrics.buildMs = elapsedMs(start);
    index.metrics.bytes = layoutBytes(index.layout);
    index.metrics.successorReferences = index.layout.plusIds.size();
    index.metrics.predecessorReferences = index.layout.minusIds.size();
    index.metrics.annotatedEdges = index.layout.annotatedEdges;
    index.metrics.pools = graph.vertexCount() * 2ULL;
    return index;
}

struct Level1Index {
    Time deltaMin = 1, deltaMax = 1;
    std::uint64_t graphHash = 0, edgeCount = 0, vertexCount = 0;
    std::vector<std::uint64_t> succOffsets, predOffsets;
    std::vector<std::uint32_t> succIds, predIds;
    IndexMetrics metrics;
};

inline Level1Index buildLevel1(const Graph& graph, Time dmin, Time dmax, Deadline& deadline,
                               std::uint64_t maxBytes) {
    (void)maxBytes;
    if (dmin < 0 || dmax < 0 || dmin > dmax)
        throw std::runtime_error("index gap bounds must satisfy 0 <= deltaMin <= deltaMax");
    const auto start = Clock::now();
    Level1Index index;
    index.deltaMin = std::max<Time>(1, dmin);
    index.deltaMax = dmax;
    index.graphHash = graphFingerprint(graph);
    index.edgeCount = graph.edges.size();
    index.vertexCount = graph.vertexCount();
    index.metrics.level = "L1_ExplicitPerEdge";

    const std::size_t m = graph.edges.size();
    const std::size_t n = graph.vertexCount();
    if (m > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Level I index implementation supports at most 2^32-1 edges");
    }
    index.succOffsets.assign(m + 1, 0);
    index.predOffsets.assign(m + 1, 0);

    if (index.deltaMin <= index.deltaMax) {
        std::uint64_t processed = 0;
        for (Vertex v = 0; static_cast<std::size_t>(v) < n; ++v) {
            const std::uint64_t inBegin = graph.inOffsets[v], inEnd = graph.inOffsets[v + 1];
            const std::uint64_t outBegin = graph.outOffsets[v], outEnd = graph.outOffsets[v + 1];

            std::uint64_t left = outBegin, right = outBegin;
            for (std::uint64_t pIn = inBegin; pIn < inEnd; ++pIn) {
                if ((++processed & 8191ULL) == 0 && deadline.pollNow()) break;
                const EdgeId id = graph.inIds[pIn];
                const Time low = safeAdd(graph.edges[id].time, index.deltaMin);
                const Time high = safeAdd(graph.edges[id].time, index.deltaMax);
                while (left < outEnd && graph.edges[graph.outIds[left]].time < low) ++left;
                if (right < left) right = left;
                while (right < outEnd && graph.edges[graph.outIds[right]].time <= high) ++right;
                index.succOffsets[static_cast<std::size_t>(id) + 1] = right - left;
            }
            if (deadline.stopped) break;

            left = inBegin; right = inBegin;
            for (std::uint64_t pOut = outBegin; pOut < outEnd; ++pOut) {
                if ((++processed & 8191ULL) == 0 && deadline.pollNow()) break;
                const EdgeId id = graph.outIds[pOut];
                const Time low = safeAdd(graph.edges[id].time, -index.deltaMax);
                const Time high = safeAdd(graph.edges[id].time, -index.deltaMin);
                while (left < inEnd && graph.edges[graph.inIds[left]].time < low) ++left;
                if (right < left) right = left;
                while (right < inEnd && graph.edges[graph.inIds[right]].time <= high) ++right;
                index.predOffsets[static_cast<std::size_t>(id) + 1] = right - left;
            }
            if (deadline.stopped) break;
        }
    }

    if (!deadline.stopped) {
        for (std::size_t i = 1; i <= m; ++i) {
            if (index.succOffsets[i] > std::numeric_limits<std::uint64_t>::max() - index.succOffsets[i - 1] ||
                index.predOffsets[i] > std::numeric_limits<std::uint64_t>::max() - index.predOffsets[i - 1]) {
                index.metrics.memoryLimit = true;
                break;
            }
            index.succOffsets[i] += index.succOffsets[i - 1];
            index.predOffsets[i] += index.predOffsets[i - 1];
            if ((i & 8191ULL) == 0 && deadline.pollNow()) break;
        }
    }

    if (!deadline.stopped && !index.metrics.memoryLimit) {
        index.succIds.resize(static_cast<std::size_t>(index.succOffsets.back()));
        index.predIds.resize(static_cast<std::size_t>(index.predOffsets.back()));
        std::uint64_t processed = 0;
        for (Vertex v = 0; static_cast<std::size_t>(v) < n; ++v) {
            const std::uint64_t inBegin = graph.inOffsets[v], inEnd = graph.inOffsets[v + 1];
            const std::uint64_t outBegin = graph.outOffsets[v], outEnd = graph.outOffsets[v + 1];

            std::uint64_t left = outBegin, right = outBegin;
            for (std::uint64_t pIn = inBegin; pIn < inEnd; ++pIn) {
                const EdgeId id = graph.inIds[pIn];
                const Time low = safeAdd(graph.edges[id].time, index.deltaMin);
                const Time high = safeAdd(graph.edges[id].time, index.deltaMax);
                while (left < outEnd && graph.edges[graph.outIds[left]].time < low) ++left;
                if (right < left) right = left;
                while (right < outEnd && graph.edges[graph.outIds[right]].time <= high) ++right;
                std::uint64_t write = index.succOffsets[id];
                for (std::uint64_t q = left; q < right; ++q) {
                    index.succIds[static_cast<std::size_t>(write++)] =
                        static_cast<std::uint32_t>(graph.outIds[q]);
                }
                if ((++processed & 4095ULL) == 0 && deadline.pollNow()) break;
            }
            if (deadline.stopped) break;

            left = inBegin; right = inBegin;
            for (std::uint64_t pOut = outBegin; pOut < outEnd; ++pOut) {
                const EdgeId id = graph.outIds[pOut];
                const Time low = safeAdd(graph.edges[id].time, -index.deltaMax);
                const Time high = safeAdd(graph.edges[id].time, -index.deltaMin);
                while (left < inEnd && graph.edges[graph.inIds[left]].time < low) ++left;
                if (right < left) right = left;
                while (right < inEnd && graph.edges[graph.inIds[right]].time <= high) ++right;
                std::uint64_t write = index.predOffsets[id];
                for (std::uint64_t q = left; q < right; ++q) {
                    index.predIds[static_cast<std::size_t>(write++)] =
                        static_cast<std::uint32_t>(graph.inIds[q]);
                }
                if ((++processed & 4095ULL) == 0 && deadline.pollNow()) break;
            }
            if (deadline.stopped) break;
        }
    }

    index.metrics.timedOut = deadline.stopped;
    index.metrics.buildMs = elapsedMs(start);
    index.metrics.successorReferences = index.succIds.size();
    index.metrics.predecessorReferences = index.predIds.size();
    index.metrics.bytes = (index.succOffsets.capacity() + index.predOffsets.capacity()) * sizeof(std::uint64_t) +
        (index.succIds.capacity() + index.predIds.capacity()) * sizeof(std::uint32_t);
    index.metrics.annotatedEdges = m;
    return index;
}

struct GroupKey {
    Vertex vertex = 0;
    std::uint64_t left = 0, right = 0;
    bool operator==(const GroupKey& other) const {
        return vertex == other.vertex && left == other.left && right == other.right;
    }
};
struct Level2Index {
    Time deltaMin = 1, deltaMax = 1;
    std::uint64_t graphHash = 0, edgeCount = 0, vertexCount = 0;
    std::vector<std::uint32_t> edgeToSuccGroup, edgeToPredGroup;
    std::vector<std::uint64_t> succGroupOffsets, predGroupOffsets;
    std::vector<std::uint32_t> succIds, predIds;
    IndexMetrics metrics;
};

inline Level2Index buildLevel2(const Graph& graph, Time dmin, Time dmax, Deadline& deadline,
                               std::uint64_t maxBytes) {
    (void)maxBytes;
    if (dmin < 0 || dmax < 0 || dmin > dmax)
        throw std::runtime_error("index gap bounds must satisfy 0 <= deltaMin <= deltaMax");
    const auto start = Clock::now();
    Level2Index index;
    index.deltaMin = std::max<Time>(1, dmin);
    index.deltaMax = dmax;
    index.graphHash = graphFingerprint(graph);
    index.edgeCount = graph.edges.size();
    index.vertexCount = graph.vertexCount();
    index.metrics.level = "L2_GroupedExplicit";

    const std::size_t m = graph.edges.size();
    const std::size_t n = graph.vertexCount();
    if (m > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Level II index implementation supports at most 2^32-1 edges");
    }
    index.edgeToSuccGroup.resize(m);
    index.edgeToPredGroup.resize(m);
    std::vector<GroupKey> succKeys, predKeys;
    succKeys.reserve(std::min<std::size_t>(m, 1U << 20));
    predKeys.reserve(std::min<std::size_t>(m, 1U << 20));

    if (index.deltaMin <= index.deltaMax) {
        std::uint64_t processed = 0;
        for (Vertex v = 0; static_cast<std::size_t>(v) < n; ++v) {
            const std::uint64_t inBegin = graph.inOffsets[v], inEnd = graph.inOffsets[v + 1];
            const std::uint64_t outBegin = graph.outOffsets[v], outEnd = graph.outOffsets[v + 1];

            std::uint64_t left = outBegin, right = outBegin;
            bool haveGroup = false;
            std::uint32_t group = 0;
            std::uint64_t previousLeft = 0, previousRight = 0;
            for (std::uint64_t pIn = inBegin; pIn < inEnd; ++pIn) {
                if ((++processed & 8191ULL) == 0 && deadline.pollNow()) break;
                const EdgeId id = graph.inIds[pIn];
                const Time low = safeAdd(graph.edges[id].time, index.deltaMin);
                const Time high = safeAdd(graph.edges[id].time, index.deltaMax);
                while (left < outEnd && graph.edges[graph.outIds[left]].time < low) ++left;
                if (right < left) right = left;
                while (right < outEnd && graph.edges[graph.outIds[right]].time <= high) ++right;
                if (!haveGroup || left != previousLeft || right != previousRight) {
                    group = static_cast<std::uint32_t>(succKeys.size());
                    succKeys.push_back({v, left, right});
                    previousLeft = left; previousRight = right; haveGroup = true;
                }
                index.edgeToSuccGroup[id] = group;
            }
            if (deadline.stopped) break;

            left = inBegin; right = inBegin; haveGroup = false;
            for (std::uint64_t pOut = outBegin; pOut < outEnd; ++pOut) {
                if ((++processed & 8191ULL) == 0 && deadline.pollNow()) break;
                const EdgeId id = graph.outIds[pOut];
                const Time low = safeAdd(graph.edges[id].time, -index.deltaMax);
                const Time high = safeAdd(graph.edges[id].time, -index.deltaMin);
                while (left < inEnd && graph.edges[graph.inIds[left]].time < low) ++left;
                if (right < left) right = left;
                while (right < inEnd && graph.edges[graph.inIds[right]].time <= high) ++right;
                if (!haveGroup || left != previousLeft || right != previousRight) {
                    group = static_cast<std::uint32_t>(predKeys.size());
                    predKeys.push_back({v, left, right});
                    previousLeft = left; previousRight = right; haveGroup = true;
                }
                index.edgeToPredGroup[id] = group;
            }
            if (deadline.stopped) break;
        }
    } else {

        for (Vertex v = 0; static_cast<std::size_t>(v) < n; ++v) {
            if (graph.inOffsets[v] != graph.inOffsets[v + 1]) {
                const std::uint32_t group = static_cast<std::uint32_t>(succKeys.size());
                succKeys.push_back({v, graph.outOffsets[v], graph.outOffsets[v]});
                for (std::uint64_t pIn = graph.inOffsets[v]; pIn < graph.inOffsets[v + 1]; ++pIn)
                    index.edgeToSuccGroup[graph.inIds[pIn]] = group;
            }
            if (graph.outOffsets[v] != graph.outOffsets[v + 1]) {
                const std::uint32_t group = static_cast<std::uint32_t>(predKeys.size());
                predKeys.push_back({v, graph.inOffsets[v], graph.inOffsets[v]});
                for (std::uint64_t pOut = graph.outOffsets[v]; pOut < graph.outOffsets[v + 1]; ++pOut)
                    index.edgeToPredGroup[graph.outIds[pOut]] = group;
            }
        }
    }

    index.succGroupOffsets.assign(succKeys.size() + 1, 0);
    index.predGroupOffsets.assign(predKeys.size() + 1, 0);
    for (std::size_t i = 0; i < succKeys.size(); ++i) {
        index.succGroupOffsets[i + 1] = index.succGroupOffsets[i] + succKeys[i].right - succKeys[i].left;
    }
    for (std::size_t i = 0; i < predKeys.size(); ++i) {
        index.predGroupOffsets[i + 1] = index.predGroupOffsets[i] + predKeys[i].right - predKeys[i].left;
    }

    if (!deadline.stopped && !index.metrics.memoryLimit) {
        index.succIds.resize(static_cast<std::size_t>(index.succGroupOffsets.back()));
        index.predIds.resize(static_cast<std::size_t>(index.predGroupOffsets.back()));
        for (std::size_t i = 0; i < succKeys.size(); ++i) {
            std::uint64_t write = index.succGroupOffsets[i];
            for (std::uint64_t q = succKeys[i].left; q < succKeys[i].right; ++q) {
                index.succIds[static_cast<std::size_t>(write++)] =
                    static_cast<std::uint32_t>(graph.outIds[q]);
            }
            if ((i & 4095ULL) == 0 && deadline.pollNow()) break;
        }
        if (!deadline.stopped) {
            for (std::size_t i = 0; i < predKeys.size(); ++i) {
                std::uint64_t write = index.predGroupOffsets[i];
                for (std::uint64_t q = predKeys[i].left; q < predKeys[i].right; ++q) {
                    index.predIds[static_cast<std::size_t>(write++)] =
                        static_cast<std::uint32_t>(graph.inIds[q]);
                }
                if ((i & 4095ULL) == 0 && deadline.pollNow()) break;
            }
        }
    }

    index.metrics.timedOut = deadline.stopped;
    index.metrics.buildMs = elapsedMs(start);
    index.metrics.groups = succKeys.size() + predKeys.size();
    index.metrics.successorReferences = index.succIds.size();
    index.metrics.predecessorReferences = index.predIds.size();
    index.metrics.bytes =
        (index.edgeToSuccGroup.capacity() + index.edgeToPredGroup.capacity()) * sizeof(std::uint32_t) +
        (index.succGroupOffsets.capacity() + index.predGroupOffsets.capacity()) * sizeof(std::uint64_t) +
        (index.succIds.capacity() + index.predIds.capacity()) * sizeof(std::uint32_t);
    index.metrics.annotatedEdges = m;
    return index;
}

inline void requireIndexMatch(Time indexMin, Time indexMax, std::uint64_t indexGraphHash,
                              std::uint64_t edgeCount, std::uint64_t vertexCount,
                              const Graph& graph, const Query& query) {
    if (indexMin != query.effectiveMin() || indexMax != query.deltaMax ||
        indexGraphHash != graphFingerprint(graph) || edgeCount != graph.edges.size() ||
        vertexCount != graph.vertexCount()) {
        throw std::runtime_error("index metadata does not match graph or gap interval");
    }
}

template <class SuccRange, class PredRange>
inline FilterOutput explicitIndexedFilter(const Graph& graph, const Query& query, Deadline& deadline,
                                          SuccRange&& successors, PredRange&& predecessors) {
    FilterOutput output; auto& metrics = output.metrics; const auto totalStart = Clock::now();
    const auto [begin, end] = graph.globalRange(query.x1, query.x2);
    metrics.rangeBegin = begin; metrics.rangeEnd = end; metrics.rangeEdges = end - begin;
    std::vector<std::uint8_t> forward(end - begin, 0), backward(end - begin, 0);
    std::vector<EdgeId> queue;
    std::size_t queueHead = 0;
    const auto fstart = Clock::now();
    const auto [sb, se] = graph.adjacencyRange(graph.outIds, graph.outOffsets, query.source, query.x1, query.x2);
    for (std::uint64_t p = sb; p < se; ++p) {
        EdgeId id = graph.outIds[p]; if (!endpointAdmissible(graph.edges[id], query)) continue;
        forward[id - begin] = 1; queue.push_back(id); ++metrics.forwardSuccess;
    }
    while (queueHead < queue.size() && !metrics.timedOut) {
        EdgeId id = queue[queueHead++]; ++metrics.forwardScanned;
        if (graph.edges[id].dst == query.target) continue;
        const auto [l, r] = successors(id);
        for (std::uint64_t p = l; p < r; ++p) {
            EdgeId next = successors.value(p); ++metrics.forwardAttempts;
            if (next < begin || next >= end || forward[next - begin] || !endpointAdmissible(graph.edges[next], query)) continue;
            forward[next - begin] = 1; queue.push_back(next); ++metrics.forwardSuccess;
        }
        if ((metrics.forwardScanned & 4095ULL) == 0 && deadline.pollNow()) metrics.timedOut = true;
    }
    metrics.forwardMs = elapsedMs(fstart);
    queue.clear(); queueHead = 0;
    const auto bstart = Clock::now();
    if (!metrics.timedOut) {
        const auto [tb, te] = graph.adjacencyRange(graph.inIds, graph.inOffsets, query.target, query.x1, query.x2);
        for (std::uint64_t p = tb; p < te; ++p) {
            EdgeId id = graph.inIds[p]; if (!endpointAdmissible(graph.edges[id], query)) continue;
            backward[id - begin] = 1; queue.push_back(id); ++metrics.backwardSuccess;
        }
        while (queueHead < queue.size() && !metrics.timedOut) {
            EdgeId id = queue[queueHead++]; ++metrics.backwardScanned;
            if (graph.edges[id].src == query.source) continue;
            const auto [l, r] = predecessors(id);
            for (std::uint64_t p = l; p < r; ++p) {
                EdgeId prev = predecessors.value(p); ++metrics.backwardAttempts;
                if (prev < begin || prev >= end || backward[prev - begin] || !endpointAdmissible(graph.edges[prev], query)) continue;
                backward[prev - begin] = 1; queue.push_back(prev); ++metrics.backwardSuccess;
            }
            if ((metrics.backwardScanned & 4095ULL) == 0 && deadline.pollNow()) metrics.timedOut = true;
        }
    }
    metrics.backwardMs = elapsedMs(bstart);
    const auto istart = Clock::now();
    if (!metrics.timedOut) for (std::uint64_t i = 0; i < end - begin; ++i) {
        metrics.forwardEdges += forward[i] != 0; metrics.backwardEdges += backward[i] != 0;
        if (forward[i] && backward[i]) output.edgeIds.push_back(begin + i);
    }
    metrics.tightEdges = output.edgeIds.size();
    metrics.intersectMs = elapsedMs(istart);
    metrics.queueBytes = queue.capacity() * sizeof(EdgeId);
    metrics.workingBytes = (forward.capacity() + backward.capacity()) * sizeof(std::uint8_t) + metrics.queueBytes;
    metrics.outputBytes = output.edgeIds.capacity() * sizeof(EdgeId);
    metrics.peakBytes = metrics.workingBytes + metrics.outputBytes;
    metrics.totalMs = elapsedMs(totalStart); return output;
}

struct L1SuccAccessor { const Level1Index& i; std::pair<std::uint64_t,std::uint64_t> operator()(EdgeId e) const { return {i.succOffsets[e],i.succOffsets[e+1]}; } EdgeId value(std::uint64_t p) const { return static_cast<EdgeId>(i.succIds[static_cast<std::size_t>(p)]); } };
struct L1PredAccessor { const Level1Index& i; std::pair<std::uint64_t,std::uint64_t> operator()(EdgeId e) const { return {i.predOffsets[e],i.predOffsets[e+1]}; } EdgeId value(std::uint64_t p) const { return static_cast<EdgeId>(i.predIds[static_cast<std::size_t>(p)]); } };
struct L2SuccAccessor { const Level2Index& i; std::pair<std::uint64_t,std::uint64_t> operator()(EdgeId e) const { const auto g=i.edgeToSuccGroup[static_cast<std::size_t>(e)]; return {i.succGroupOffsets[g],i.succGroupOffsets[static_cast<std::size_t>(g)+1]}; } EdgeId value(std::uint64_t p) const { return static_cast<EdgeId>(i.succIds[static_cast<std::size_t>(p)]); } };
struct L2PredAccessor { const Level2Index& i; std::pair<std::uint64_t,std::uint64_t> operator()(EdgeId e) const { const auto g=i.edgeToPredGroup[static_cast<std::size_t>(e)]; return {i.predGroupOffsets[g],i.predGroupOffsets[static_cast<std::size_t>(g)+1]}; } EdgeId value(std::uint64_t p) const { return static_cast<EdgeId>(i.predIds[static_cast<std::size_t>(p)]); } };

inline FilterOutput level1Filter(const Graph& graph, const Query& q, const Level1Index& i, Deadline& d) {
    requireIndexMatch(i.deltaMin, i.deltaMax, i.graphHash, i.edgeCount, i.vertexCount, graph, q);
    return explicitIndexedFilter(graph, q, d, L1SuccAccessor{i}, L1PredAccessor{i});
}
inline FilterOutput level2Filter(const Graph& graph, const Query& q, const Level2Index& i, Deadline& d) {
    requireIndexMatch(i.deltaMin, i.deltaMax, i.graphHash, i.edgeCount, i.vertexCount, graph, q);
    return explicitIndexedFilter(graph, q, d, L2SuccAccessor{i}, L2PredAccessor{i});
}

inline FilterOutput level3Filter(const Graph& graph, const Query& query, const Level3Index& index,
                                 Deadline& deadline, bool debugPoolVisits = false) {
    requireIndexMatch(index.layout.deltaMin, index.layout.deltaMax, index.layout.graphHash,
        index.layout.edgeCount, index.layout.vertexCount, graph, query);
    validateLevel3Shape(index.layout);
    FilterOutput output;
    auto& metrics = output.metrics;
    const auto totalStart = Clock::now();
    if (deadline.pollNow()) {
        metrics.timedOut = true;
        metrics.totalMs = elapsedMs(totalStart);
        return output;
    }

    const auto rangeStart = Clock::now();
    const auto queryRange = graph.globalRange(query.x1, query.x2);
    const EdgeId begin = queryRange.first;
    const EdgeId end = queryRange.second;
    metrics.rangeLocateMs = elapsedMs(rangeStart);
    metrics.rangeBegin = begin;
    metrics.rangeEnd = end;
    metrics.rangeEdges = end - begin;
    if (!query.syntacticallyValid() || begin == end) {
        metrics.totalMs = elapsedMs(totalStart);
        return output;
    }

    const std::size_t queryWordCount = static_cast<std::size_t>((end - begin + 63) / 64);
    index.forwardWords.resize(queryWordCount);
    index.backwardWords.resize(queryWordCount);
    std::fill(index.forwardWords.begin(), index.forwardWords.end(), 0);
    std::fill(index.backwardWords.begin(), index.backwardWords.end(), 0);
    auto& forward = index.forwardWords;
    auto& backward = index.backwardWords;
    const auto isMarked = [&](const std::vector<std::uint64_t>& bits, EdgeId id) {
        const std::uint64_t local = id - begin;
        return (bits[local >> 6] & (1ULL << (local & 63ULL))) != 0;
    };
    const auto mark = [&](std::vector<std::uint64_t>& bits, EdgeId id) {
        const std::uint64_t local = id - begin;
        const std::uint64_t mask = 1ULL << (local & 63ULL);
        auto& word = bits[local >> 6];
        const bool inserted = (word & mask) == 0;
        word |= mask;
        return inserted;
    };

    std::vector<std::uint8_t> forwardPoolVisits, backwardPoolVisits;
    if (debugPoolVisits) {
        forwardPoolVisits.assign(index.layout.plusIds.size(), 0);
        backwardPoolVisits.assign(index.layout.minusIds.size(), 0);
    }

    const std::uint32_t forwardEpoch = nextLevel3WorkspaceEpoch(index);
    auto poolIsActive = [&](Vertex vertex, std::uint32_t epoch) {
        return index.queryPoolStates[vertex].epoch == epoch;
    };
    auto activateForwardPool = [&](Vertex vertex) -> Level3PoolState& {
        auto& state = index.queryPoolStates[vertex];
        state.epoch = forwardEpoch;
        state.frontier = index.layout.plusOffsets[vertex];
        state.clip = index.layout.plusOffsets[vertex + 1];
        return state;
    };

    const auto fstart = Clock::now();
    const auto [sourceBegin, sourceEnd] = graph.adjacencyRange(
        graph.outIds, graph.outOffsets, query.source, query.x1, query.x2);
    std::uint64_t sourcePosition = sourceBegin;
    EdgeId forwardBridgeCursor = begin;
    const auto nextForwardBridge = [&]() -> EdgeId {
        while (forwardBridgeCursor < end) {
            const std::size_t wordIndex = static_cast<std::size_t>(forwardBridgeCursor >> 6);
            const unsigned bitOffset = static_cast<unsigned>(forwardBridgeCursor & 63ULL);
            std::uint64_t word = index.layout.bridgeWords[wordIndex] & (~0ULL << bitOffset);
            if (word) {
                const EdgeId id = (static_cast<EdgeId>(wordIndex) << 6) + ctz64(word);
                forwardBridgeCursor = id + 1;
                return id < end ? id : end;
            }
            forwardBridgeCursor = (static_cast<EdgeId>(wordIndex) + 1) << 6;
        }
        return end;
    };
    EdgeId indexedId = nextForwardBridge();

    while (!metrics.timedOut) {
        const EdgeId sourceId = sourcePosition < sourceEnd ? graph.outIds[sourcePosition] : end;
        if (indexedId == end && sourceId == end) break;
        const EdgeId id = std::min(indexedId, sourceId);
        if (indexedId == id) indexedId = nextForwardBridge();
        if (sourceId == id) ++sourcePosition;

        ++metrics.forwardScanned;
        const auto& edge = graph.edges[id];
        if (!endpointAdmissible(edge, query)) continue;
        if (edge.src == query.source &&
            (edge.dst == query.target || layoutHasSucc(index.layout, id)) &&
            mark(forward, id)) {
            ++metrics.forwardSuccess;
        }
        if (!isMarked(forward, id) || edge.dst == query.target) continue;

        ++metrics.intervalActivations;
        const Vertex pool = edge.dst;
        Level3PoolState* statePtr = nullptr;
        if (!poolIsActive(pool, forwardEpoch)) statePtr = &activateForwardPool(pool);
        else statePtr = &index.queryPoolStates[pool];
        auto& state = *statePtr;

        const std::uint64_t succPacked = index.layout.succRanges[id];
        const std::uint32_t clippedLeft = rangeLeft(succPacked);
        const std::uint32_t clippedRight = std::min(rangeRight(succPacked), state.clip);
        const std::uint32_t left = std::max(state.frontier, clippedLeft);
        std::uint32_t right = clippedRight;

        std::uint64_t processed = 0;
        for (std::uint32_t k = left; k < right; ++k) {
            const EdgeId next = index.layout.plusIds[k];
            const auto& nextEdge = graph.edges[next];
            if (nextEdge.time > query.x2) {

                state.clip = k;
                right = k;
                break;
            }
            ++processed;
            if (debugPoolVisits) {
                auto& visits = forwardPoolVisits[k];
                if (visits != std::numeric_limits<std::uint8_t>::max()) ++visits;
                metrics.maxForwardPoolVisitCount = std::max(metrics.maxForwardPoolVisitCount, visits);
            }
            if (nextEdge.dst != query.source &&
                (layoutHasSucc(index.layout, next) || nextEdge.dst == query.target) &&
                mark(forward, next)) {
                ++metrics.forwardSuccess;
            }
            if ((processed & 8191ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                break;
            }
        }
        metrics.logicalCandidateReferences += right > clippedLeft ? right - clippedLeft : 0;
        metrics.emptyActivations += left >= right;
        metrics.forwardAttempts += processed;
        metrics.distinctPoolPositionsScanned += processed;
        state.frontier = std::max(state.frontier, right);
        if ((metrics.forwardScanned & 8191ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            break;
        }
    }
    metrics.forwardMs = elapsedMs(fstart);

    const auto bstart = Clock::now();
    const std::uint32_t backwardEpoch = nextLevel3WorkspaceEpoch(index);
    auto activateBackwardPool = [&](Vertex vertex) -> Level3PoolState& {
        auto& state = index.queryPoolStates[vertex];
        state.epoch = backwardEpoch;
        state.frontier = index.layout.minusOffsets[vertex + 1];
        state.clip = index.layout.minusOffsets[vertex];
        return state;
    };

    const auto [targetBegin, targetEnd] = graph.adjacencyRange(
        graph.inIds, graph.inOffsets, query.target, query.x1, query.x2);
    std::uint64_t targetPosition = targetEnd;
    EdgeId backwardBridgeCursor = end;
    const auto previousBackwardBridge = [&]() -> EdgeId {
        while (backwardBridgeCursor > begin) {
            const EdgeId last = backwardBridgeCursor - 1;
            const std::size_t wordIndex = static_cast<std::size_t>(last >> 6);
            const unsigned bitOffset = static_cast<unsigned>(last & 63ULL);
            const std::uint64_t mask = bitOffset == 63U
                ? ~0ULL : ((1ULL << (bitOffset + 1U)) - 1ULL);
            const std::uint64_t word = index.layout.bridgeWords[wordIndex] & mask;
            if (word) {
#if defined(__GNUC__) || defined(__clang__)
                const unsigned bit = 63U - static_cast<unsigned>(__builtin_clzll(word));
#else
                unsigned bit = 0;
                std::uint64_t copy = word;
                while (copy >>= 1) ++bit;
#endif
                const EdgeId id = (static_cast<EdgeId>(wordIndex) << 6) + bit;
                if (id < begin) return TransitionLayout::NO_RANK;
                backwardBridgeCursor = id;
                return id;
            }
            backwardBridgeCursor = static_cast<EdgeId>(wordIndex) << 6;
        }
        return TransitionLayout::NO_RANK;
    };
    indexedId = previousBackwardBridge();

    while (!metrics.timedOut) {
        const EdgeId targetId = targetPosition > targetBegin
            ? graph.inIds[targetPosition - 1] : TransitionLayout::NO_RANK;
        if (indexedId == TransitionLayout::NO_RANK && targetId == TransitionLayout::NO_RANK) break;
        const EdgeId id = indexedId == TransitionLayout::NO_RANK ? targetId :
            (targetId == TransitionLayout::NO_RANK ? indexedId : std::max(indexedId, targetId));
        if (indexedId == id) indexedId = previousBackwardBridge();
        if (targetId == id) --targetPosition;

        ++metrics.backwardScanned;
        const auto& edge = graph.edges[id];
        if (!endpointAdmissible(edge, query)) continue;
        if (edge.dst == query.target &&
            (edge.src == query.source || layoutHasPred(index.layout, id)) &&
            mark(backward, id)) {
            ++metrics.backwardSuccess;
        }
        if (!isMarked(backward, id) || edge.src == query.source) continue;

        ++metrics.intervalActivations;
        const Vertex pool = edge.src;
        Level3PoolState* statePtr = nullptr;
        if (!poolIsActive(pool, backwardEpoch)) statePtr = &activateBackwardPool(pool);
        else statePtr = &index.queryPoolStates[pool];
        auto& state = *statePtr;

        const std::uint64_t predPacked = index.layout.predRanges[id];
        std::uint32_t left = std::max(rangeLeft(predPacked), state.clip);
        const std::uint32_t right = std::min(state.frontier, rangeRight(predPacked));

        std::uint64_t processed = 0;
        for (std::uint32_t k = right; k-- > left;) {
            const EdgeId previous = index.layout.minusIds[k];
            const auto& previousEdge = graph.edges[previous];
            if (previousEdge.time < query.x1) {

                state.clip = k + 1;
                left = k + 1;
                break;
            }
            ++processed;
            if (debugPoolVisits) {
                auto& visits = backwardPoolVisits[k];
                if (visits != std::numeric_limits<std::uint8_t>::max()) ++visits;
                metrics.maxBackwardPoolVisitCount = std::max(metrics.maxBackwardPoolVisitCount, visits);
            }
            if (previousEdge.src != query.target &&
                (layoutHasPred(index.layout, previous) || previousEdge.src == query.source) &&
                mark(backward, previous)) {
                ++metrics.backwardSuccess;
            }
            if ((processed & 8191ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                break;
            }
        }
        metrics.logicalCandidateReferences += right > left ? right - left : 0;
        metrics.emptyActivations += left >= right;
        metrics.backwardAttempts += processed;
        metrics.distinctPoolPositionsScanned += processed;
        state.frontier = std::min(state.frontier, left);
        if ((metrics.backwardScanned & 8191ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            break;
        }
    }
    metrics.backwardMs = elapsedMs(bstart);

    const auto istart = Clock::now();
    metrics.forwardEdges = metrics.forwardSuccess;
    metrics.backwardEdges = metrics.backwardSuccess;
    if (!metrics.timedOut) {
        output.edgeIds.reserve(std::min(metrics.forwardSuccess, metrics.backwardSuccess));
        for (std::size_t wordIndex = 0; wordIndex < queryWordCount; ++wordIndex) {
            std::uint64_t common = forward[wordIndex] & backward[wordIndex];
            while (common) {
                const unsigned bit = trailingZeroCount(common);
                const EdgeId id = begin + (static_cast<EdgeId>(wordIndex) << 6) + bit;
                if (id < end) output.edgeIds.push_back(id);
                common &= common - 1;
            }
        }
    }
    metrics.tightEdges = output.edgeIds.size();
    metrics.intersectMs = elapsedMs(istart);

    metrics.workingBytes = (forward.capacity() + backward.capacity()) * sizeof(std::uint64_t);
    metrics.outputBytes = output.edgeIds.capacity() * sizeof(EdgeId);
    metrics.peakBytes = metrics.workingBytes + metrics.outputBytes;
    metrics.totalMs = elapsedMs(totalStart);
    return output;
}

inline void addIndexRecord(Record& record, const IndexMetrics& m) {
    record.set("index_level", m.level); record.setDouble("index_build_ms", m.buildMs);
    record.set("index_bytes", m.bytes); record.set("index_successor_references", m.successorReferences);
    record.set("index_predecessor_references", m.predecessorReferences); record.set("index_groups", m.groups);
    record.set("index_annotated_edges", m.annotatedEdges); record.set("index_pools", m.pools);
    record.set("index_build_timeout", m.timedOut ? 1 : 0); record.set("index_memory_limit", m.memoryLimit ? 1 : 0);
}

inline void saveLevel3(const Level3Index& index, const std::string& path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create L3 index: " + path);
    const std::uint64_t magic = 0x5450584c33494459ULL;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&index.layout.formatVersion), sizeof(std::uint64_t));
    out.write(reinterpret_cast<const char*>(&index.layout.graphHash), sizeof(std::uint64_t));
    out.write(reinterpret_cast<const char*>(&index.layout.edgeCount), sizeof(std::uint64_t));
    out.write(reinterpret_cast<const char*>(&index.layout.vertexCount), sizeof(std::uint64_t));
    out.write(reinterpret_cast<const char*>(&index.layout.deltaMin), sizeof(Time));
    out.write(reinterpret_cast<const char*>(&index.layout.deltaMax), sizeof(Time));
    out.write(reinterpret_cast<const char*>(&index.layout.annotatedEdges), sizeof(std::uint64_t));
    writeVector(out, index.layout.plusOffsets); writeVector(out, index.layout.minusOffsets);
    writeVector(out, index.layout.plusIds); writeVector(out, index.layout.minusIds);
    writeVector(out, index.layout.succRanges); writeVector(out, index.layout.predRanges);
    writeVector(out, index.layout.bridgeWords);
    if (!out) throw std::runtime_error("failed while writing L3 index: " + path);
}

inline Level3Index loadLevel3(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open L3 index: " + path);
    std::uint64_t magic = 0; in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x5450584c33494459ULL) throw std::runtime_error("invalid or obsolete L3 index");
    Level3Index index; index.metrics.level = "L3_SharedPoolMonotoneFrontier";
    in.read(reinterpret_cast<char*>(&index.layout.formatVersion), sizeof(std::uint64_t));
    in.read(reinterpret_cast<char*>(&index.layout.graphHash), sizeof(std::uint64_t));
    in.read(reinterpret_cast<char*>(&index.layout.edgeCount), sizeof(std::uint64_t));
    in.read(reinterpret_cast<char*>(&index.layout.vertexCount), sizeof(std::uint64_t));
    if (index.layout.formatVersion != 10) throw std::runtime_error("unsupported L3 index version");
    in.read(reinterpret_cast<char*>(&index.layout.deltaMin), sizeof(Time));
    in.read(reinterpret_cast<char*>(&index.layout.deltaMax), sizeof(Time));
    in.read(reinterpret_cast<char*>(&index.layout.annotatedEdges), sizeof(std::uint64_t));
    readVector(in, index.layout.plusOffsets); readVector(in, index.layout.minusOffsets);
    readVector(in, index.layout.plusIds); readVector(in, index.layout.minusIds);
    readVector(in, index.layout.succRanges); readVector(in, index.layout.predRanges);
    readVector(in, index.layout.bridgeWords);
    if (!in) throw std::runtime_error("truncated L3 index: " + path);
    validateLevel3Shape(index.layout);
    index.metrics.bytes = layoutBytes(index.layout);
    index.metrics.successorReferences = index.layout.plusIds.size();
    index.metrics.predecessorReferences = index.layout.minusIds.size();
    index.metrics.annotatedEdges = index.layout.annotatedEdges;
    index.metrics.pools = index.layout.vertexCount * 2ULL;
    initializeLevel3Workspace(index);
    return index;
}

inline void saveLevel1(const Level1Index& index, const std::string& path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create L1 index: " + path);
    const std::uint64_t magic = 0x5450584c31494459ULL;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    const std::uint64_t version = 3; out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&index.graphHash), sizeof(index.graphHash));
    out.write(reinterpret_cast<const char*>(&index.edgeCount), sizeof(index.edgeCount));
    out.write(reinterpret_cast<const char*>(&index.vertexCount), sizeof(index.vertexCount));
    out.write(reinterpret_cast<const char*>(&index.deltaMin), sizeof(Time));
    out.write(reinterpret_cast<const char*>(&index.deltaMax), sizeof(Time));
    writeVector(out, index.succOffsets); writeVector(out, index.predOffsets);
    writeVector(out, index.succIds); writeVector(out, index.predIds);
}

inline Level1Index loadLevel1(const std::string& path) {
    std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("cannot open L1 index");
    std::uint64_t magic = 0; in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x5450584c31494459ULL) throw std::runtime_error("invalid or obsolete L1 index");
    Level1Index index; index.metrics.level = "L1_ExplicitPerEdge";
    std::uint64_t version = 0; in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 3) throw std::runtime_error("unsupported L1 index version");
    in.read(reinterpret_cast<char*>(&index.graphHash), sizeof(index.graphHash));
    in.read(reinterpret_cast<char*>(&index.edgeCount), sizeof(index.edgeCount));
    in.read(reinterpret_cast<char*>(&index.vertexCount), sizeof(index.vertexCount));
    in.read(reinterpret_cast<char*>(&index.deltaMin), sizeof(Time)); in.read(reinterpret_cast<char*>(&index.deltaMax), sizeof(Time));
    readVector(in, index.succOffsets); readVector(in, index.predOffsets); readVector(in, index.succIds); readVector(in, index.predIds);
    index.metrics.bytes = (index.succOffsets.capacity() + index.predOffsets.capacity()) * sizeof(std::uint64_t) +
        (index.succIds.capacity() + index.predIds.capacity()) * sizeof(std::uint32_t);
    return index;
}

inline void saveLevel2(const Level2Index& index, const std::string& path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc); if (!out) throw std::runtime_error("cannot create L2 index");
    const std::uint64_t magic = 0x5450584c32494459ULL;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    const std::uint64_t version = 3; out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&index.graphHash), sizeof(index.graphHash));
    out.write(reinterpret_cast<const char*>(&index.edgeCount), sizeof(index.edgeCount));
    out.write(reinterpret_cast<const char*>(&index.vertexCount), sizeof(index.vertexCount));
    out.write(reinterpret_cast<const char*>(&index.deltaMin), sizeof(Time)); out.write(reinterpret_cast<const char*>(&index.deltaMax), sizeof(Time));
    writeVector(out, index.edgeToSuccGroup); writeVector(out, index.edgeToPredGroup);
    writeVector(out, index.succGroupOffsets); writeVector(out, index.predGroupOffsets);
    writeVector(out, index.succIds); writeVector(out, index.predIds);
}

inline Level2Index loadLevel2(const std::string& path) {
    std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("cannot open L2 index");
    std::uint64_t magic = 0; in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x5450584c32494459ULL) throw std::runtime_error("invalid or obsolete L2 index");
    Level2Index index; index.metrics.level = "L2_GroupedExplicit";
    std::uint64_t version = 0; in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 3) throw std::runtime_error("unsupported L2 index version");
    in.read(reinterpret_cast<char*>(&index.graphHash), sizeof(index.graphHash));
    in.read(reinterpret_cast<char*>(&index.edgeCount), sizeof(index.edgeCount));
    in.read(reinterpret_cast<char*>(&index.vertexCount), sizeof(index.vertexCount));
    in.read(reinterpret_cast<char*>(&index.deltaMin), sizeof(Time)); in.read(reinterpret_cast<char*>(&index.deltaMax), sizeof(Time));
    readVector(in, index.edgeToSuccGroup); readVector(in, index.edgeToPredGroup);
    readVector(in, index.succGroupOffsets); readVector(in, index.predGroupOffsets);
    readVector(in, index.succIds); readVector(in, index.predIds);
    index.metrics.bytes =
        (index.edgeToSuccGroup.capacity() + index.edgeToPredGroup.capacity()) * sizeof(std::uint32_t) +
        (index.succGroupOffsets.capacity() + index.predGroupOffsets.capacity()) * sizeof(std::uint64_t) +
        (index.succIds.capacity() + index.predIds.capacity()) * sizeof(std::uint32_t);
    return index;
}

}
