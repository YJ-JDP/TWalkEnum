#pragma once

#include "enumeration.hpp"
#include "topology.hpp"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace tpx {

inline std::uint64_t arcKey(Vertex u, Vertex v) {
    return (static_cast<std::uint64_t>(u) << 32) | static_cast<std::uint64_t>(v);
}

inline std::vector<int> boundedBfs(const std::vector<std::vector<Vertex>>& adjacency,
                                   Vertex source, std::uint32_t limit,
                                   Vertex banned = std::numeric_limits<Vertex>::max(),
                                   std::uint64_t* edgeScans = nullptr,
                                   std::uint64_t* bytes = nullptr) {
    std::vector<int> distance(adjacency.size(), -1);
    std::vector<Vertex> queue;
    if (source == banned) return distance;
    distance[source] = 0;
    queue.push_back(source);
    std::size_t head = 0;
    while (head < queue.size()) {
        const Vertex u = queue[head++];
        if (static_cast<std::uint32_t>(distance[u]) >= limit) continue;
        for (Vertex v : adjacency[u]) {
            if (edgeScans) ++*edgeScans;
            if (v == banned || distance[v] >= 0) continue;
            distance[v] = distance[u] + 1;
            queue.push_back(v);
        }
    }
    if (bytes) {
        *bytes = distance.capacity() * sizeof(int) + queue.capacity() * sizeof(Vertex);
    }
    return distance;
}

struct TemporalArcStore {
    std::unordered_map<std::uint64_t, std::vector<EdgeId>> events;
    std::uint64_t intervalEvents = 0;
    std::uint64_t mappedEvents = 0;
    std::uint64_t bytes = 0;
};

inline std::uint64_t unorderedMapApproxBytes(const std::unordered_map<std::uint64_t, std::vector<EdgeId>>& map) {
    std::uint64_t bytes = static_cast<std::uint64_t>(map.bucket_count()) * sizeof(void*);
    bytes += static_cast<std::uint64_t>(map.size()) *
        (sizeof(std::pair<const std::uint64_t, std::vector<EdgeId>>) + 2 * sizeof(void*));
    for (const auto& kv : map) bytes += kv.second.capacity() * sizeof(EdgeId);
    return bytes;
}

inline TemporalArcStore buildTemporalArcStore(const Graph& graph, const Query& query,
                                               const std::unordered_set<std::uint64_t>& allowedArcs,
                                               Deadline& deadline,
                                               std::uint64_t* sourceEdgeScans = nullptr) {
    TemporalArcStore store;
    store.events.reserve(allowedArcs.size() * 2 + 1);
    const auto [begin, end] = graph.globalRange(query.x1, query.x2);
    store.intervalEvents = end - begin;
    std::uint64_t steps = 0;
    for (EdgeId id = begin; id < end; ++id) {
        if ((++steps & 4095ULL) == 0 && deadline.pollNow()) break;
        if (sourceEdgeScans) ++*sourceEdgeScans;
        const auto& edge = graph.edges[id];
        if (!endpointAdmissible(edge, query)) continue;
        const auto key = arcKey(edge.src, edge.dst);
        if (allowedArcs.find(key) == allowedArcs.end()) continue;
        store.events[key].push_back(id);
        ++store.mappedEvents;
    }
    store.bytes = unorderedMapApproxBytes(store.events);
    return store;
}

inline const std::vector<EdgeId>* findArcEvents(const TemporalArcStore& store, Vertex u, Vertex v) {
    const auto it = store.events.find(arcKey(u, v));
    return it == store.events.end() ? nullptr : &it->second;
}

inline std::pair<std::size_t, std::size_t> timeRangeInEdgeIds(const Graph& graph,
                                                              const std::vector<EdgeId>& ids,
                                                              Time low, Time high) {
    if (low > high) return {0, 0};
    auto first = std::lower_bound(ids.begin(), ids.end(), low,
        [&](EdgeId id, Time value) { return graph.edges[id].time < value; });
    auto last = std::upper_bound(first, ids.end(), high,
        [&](Time value, EdgeId id) { return value < graph.edges[id].time; });
    return {static_cast<std::size_t>(first - ids.begin()),
            static_cast<std::size_t>(last - ids.begin())};
}

inline std::uint64_t unorderedSetApproxBytes(const std::unordered_set<std::uint64_t>& set) {
    return static_cast<std::uint64_t>(set.bucket_count()) * sizeof(void*) +
           static_cast<std::uint64_t>(set.size()) * (sizeof(std::uint64_t) + 2 * sizeof(void*));
}

struct PathEnumNeighbor {
    Vertex vertex = 0;
    int distanceToTarget = -1;
};

struct PathEnumQueryIndex {
    std::vector<int> distanceFromSource;
    std::vector<int> distanceToTarget;
    std::vector<std::vector<PathEnumNeighbor>> neighbors;
    std::unordered_set<std::uint64_t> allowedArcs;
    std::uint64_t staticVertices = 0;
    std::uint64_t staticArcs = 0;
    std::uint64_t bfsEdgeScans = 0;
    std::uint64_t bytes = 0;
};

inline PathEnumQueryIndex buildPathEnumQueryIndex(const TopologyIndex& topology,
                                                  const Query& query,
                                                  std::uint32_t hopBound) {
    PathEnumQueryIndex index;
    std::uint64_t bfsBytesA = 0, bfsBytesB = 0;

    index.distanceFromSource = boundedBfs(topology.outgoing, query.source, hopBound,
                                         query.target, &index.bfsEdgeScans, &bfsBytesA);
    index.distanceToTarget = boundedBfs(topology.incoming, query.target, hopBound,
                                       query.source, &index.bfsEdgeScans, &bfsBytesB);
    index.neighbors.assign(topology.outgoing.size(), {});
    index.allowedArcs.reserve(topology.edgeCount / 4 + 1);

    std::vector<unsigned char> seenVertex(topology.outgoing.size(), 0);
    seenVertex[query.source] = 1;
    seenVertex[query.target] = 1;
    for (Vertex u = 0; u < topology.outgoing.size(); ++u) {
        if (u == query.target || index.distanceFromSource[u] < 0) continue;
        auto& row = index.neighbors[u];
        for (Vertex v : topology.outgoing[u]) {
            if (v == query.source || index.distanceToTarget[v] < 0) continue;
            const std::uint64_t lower = static_cast<std::uint64_t>(index.distanceFromSource[u]) + 1ULL +
                                        static_cast<std::uint64_t>(index.distanceToTarget[v]);
            if (lower > hopBound) continue;
            row.push_back({v, index.distanceToTarget[v]});
            index.allowedArcs.insert(arcKey(u, v));
            seenVertex[u] = 1;
            seenVertex[v] = 1;
            ++index.staticArcs;
        }
        std::sort(row.begin(), row.end(), [](const auto& a, const auto& b) {
            return a.distanceToTarget < b.distanceToTarget ||
                   (a.distanceToTarget == b.distanceToTarget && a.vertex < b.vertex);
        });
    }
    for (auto x : seenVertex) index.staticVertices += x != 0;

    index.bytes = bfsBytesA + bfsBytesB;
    index.bytes += index.neighbors.capacity() * sizeof(std::vector<PathEnumNeighbor>);
    for (const auto& row : index.neighbors) index.bytes += row.capacity() * sizeof(PathEnumNeighbor);
    index.bytes += unorderedSetApproxBytes(index.allowedArcs);
    return index;
}

struct PathEnumAdaptMetrics {
    double topologyQueryIndexMs = 0.0;
    double temporalSubgraphMs = 0.0;
    double enumerationMs = 0.0;
    double totalMs = 0.0;
    std::uint32_t hopBound = 0;
    std::uint64_t staticVertices = 0;
    std::uint64_t staticArcs = 0;
    std::uint64_t topologyBfsEdgeScans = 0;
    std::uint64_t intervalEvents = 0;
    std::uint64_t temporalSubgraphEvents = 0;
    std::uint64_t temporalMapEdgeScans = 0;
    std::uint64_t staticNeighborChecks = 0;
    std::uint64_t temporalRangeQueries = 0;
    std::uint64_t temporalEventVisits = 0;
    std::uint64_t dfsStates = 0;
    std::uint64_t gapAcceptedTransitions = 0;
    std::uint64_t queryIndexBytes = 0;
    std::uint64_t temporalSubgraphBytes = 0;
    std::uint64_t dfsScratchPeakBytes = 0;
    std::uint64_t queryPeakBytes = 0;
    bool timedOut = false;
    bool unsupportedHop = false;
    ResultSummary result;
};

inline PathEnumAdaptMetrics runPathEnumStrict(const Graph& graph, const TopologyIndex& topology,
                                              const Query& query, Deadline& deadline,
                                              std::uint32_t maximumSupportedHop = 0) {
    PathEnumAdaptMetrics metrics;
    const auto totalStart = Clock::now();
    metrics.hopBound = temporalHopUpperBound(query);
    (void)maximumSupportedHop;
    if (metrics.hopBound == 0) {
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    const auto indexStart = Clock::now();
    PathEnumQueryIndex index = buildPathEnumQueryIndex(topology, query, metrics.hopBound);
    metrics.topologyQueryIndexMs = elapsedMs(indexStart);
    metrics.staticVertices = index.staticVertices;
    metrics.staticArcs = index.staticArcs;
    metrics.topologyBfsEdgeScans = index.bfsEdgeScans;
    metrics.queryIndexBytes = index.bytes;

    const auto temporalStart = Clock::now();
    TemporalArcStore store = buildTemporalArcStore(graph, query, index.allowedArcs,
                                                    deadline, &metrics.temporalMapEdgeScans);
    metrics.temporalSubgraphMs = elapsedMs(temporalStart);
    metrics.intervalEvents = store.intervalEvents;
    metrics.temporalSubgraphEvents = store.mappedEvents;
    metrics.temporalSubgraphBytes = store.bytes;
    if (deadline.stopped) {
        metrics.timedOut = true;
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    const auto searchStart = Clock::now();
    std::uint64_t operations = 0;
    std::function<void(Vertex, Time, std::uint32_t)> dfs = [&](Vertex u, Time last, std::uint32_t used) {
        if (metrics.timedOut || used >= metrics.hopBound) return;
        ++metrics.dfsStates;
        if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            return;
        }
        const int remainingAfterNext = static_cast<int>(metrics.hopBound - used - 1);
        for (const auto& neighbor : index.neighbors[u]) {
            if (neighbor.distanceToTarget > remainingAfterNext) break;
            ++metrics.staticNeighborChecks;
            const auto* ids = findArcEvents(store, u, neighbor.vertex);
            if (!ids) continue;
            Time low = query.x1, high = query.x2;
            if (used > 0) {
                low = safeAdd(last, query.effectiveMin());
                high = safeAdd(last, query.deltaMax);
            }
            ++metrics.temporalRangeQueries;
            const auto [begin, end] = timeRangeInEdgeIds(graph, *ids, low, high);
            for (std::size_t p = begin; p < end; ++p) {
                ++metrics.temporalEventVisits;
                ++metrics.gapAcceptedTransitions;
                if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                    metrics.timedOut = true;
                    return;
                }
                const EdgeId edgeId = (*ids)[p];
                const auto& edge = graph.edges[edgeId];
                const std::uint64_t length = static_cast<std::uint64_t>(used) + 1ULL;
                metrics.dfsScratchPeakBytes = std::max<std::uint64_t>(metrics.dfsScratchPeakBytes,
                    length * (sizeof(Vertex) + sizeof(Time) + sizeof(EdgeId)));
                if (neighbor.vertex == query.target) {
                    metrics.result.addFastRecord(length);
                } else {
                    dfs(neighbor.vertex, edge.time, used + 1);
                    if (metrics.timedOut) return;
                }
            }
        }
    };
    dfs(query.source, 0, 0);
    metrics.enumerationMs = elapsedMs(searchStart);
    metrics.result.finishEmpty();
    metrics.queryPeakBytes = metrics.queryIndexBytes + metrics.temporalSubgraphBytes +
                             metrics.dfsScratchPeakBytes + metrics.result.allocatedBytes();
    metrics.totalMs = elapsedMs(totalStart);
    return metrics;
}

struct CpeStaticSubgraph {
    std::vector<int> distanceFromSource;
    std::vector<int> distanceToTarget;
    std::vector<unsigned char> keep;
    std::vector<std::vector<Vertex>> outgoing;
    std::vector<std::vector<Vertex>> incoming;
    std::unordered_set<std::uint64_t> allowedArcs;
    std::uint64_t vertices = 0;
    std::uint64_t arcs = 0;
    std::uint64_t bfsEdgeScans = 0;
    std::uint64_t inducedEdgeChecks = 0;
    std::uint64_t bytes = 0;
};

inline CpeStaticSubgraph buildCpeStaticSubgraph(const TopologyIndex& topology,
                                                const Query& query,
                                                std::uint32_t hopBound) {
    CpeStaticSubgraph sub;
    const std::uint32_t bfsLimit = hopBound > 0 ? hopBound - 1 : 0;
    std::uint64_t b1 = 0, b2 = 0;
    sub.distanceFromSource = boundedBfs(topology.outgoing, query.source, bfsLimit,
                                        std::numeric_limits<Vertex>::max(), &sub.bfsEdgeScans, &b1);
    sub.distanceToTarget = boundedBfs(topology.incoming, query.target, bfsLimit,
                                      std::numeric_limits<Vertex>::max(), &sub.bfsEdgeScans, &b2);
    sub.keep.assign(topology.outgoing.size(), 0);
    sub.keep[query.source] = 1;
    sub.keep[query.target] = 1;
    for (Vertex v = 0; v < topology.outgoing.size(); ++v) {
        if (v == query.source || v == query.target) continue;
        if (sub.distanceFromSource[v] >= 0 && sub.distanceToTarget[v] >= 0 &&
            static_cast<std::uint64_t>(sub.distanceFromSource[v]) +
            static_cast<std::uint64_t>(sub.distanceToTarget[v]) <= hopBound) {
            sub.keep[v] = 1;
        }
    }
    sub.outgoing.assign(topology.outgoing.size(), {});
    sub.incoming.assign(topology.incoming.size(), {});
    for (Vertex u = 0; u < topology.outgoing.size(); ++u) {
        if (!sub.keep[u]) continue;
        ++sub.vertices;
        for (Vertex v : topology.outgoing[u]) {
            ++sub.inducedEdgeChecks;
            if (!sub.keep[v]) continue;
            sub.outgoing[u].push_back(v);
            sub.incoming[v].push_back(u);
            sub.allowedArcs.insert(arcKey(u, v));
            ++sub.arcs;
        }
    }
    sub.bytes = b1 + b2 + sub.keep.capacity() * sizeof(unsigned char);
    sub.bytes += (sub.outgoing.capacity() + sub.incoming.capacity()) * sizeof(std::vector<Vertex>);
    for (const auto& row : sub.outgoing) sub.bytes += row.capacity() * sizeof(Vertex);
    for (const auto& row : sub.incoming) sub.bytes += row.capacity() * sizeof(Vertex);
    sub.bytes += unorderedSetApproxBytes(sub.allowedArcs);
    return sub;
}

struct CpePart {
    Vertex endpoint = 0;
    Time boundary = 0;
    std::size_t parent = std::numeric_limits<std::size_t>::max();
    EdgeId edge = std::numeric_limits<EdgeId>::max();
};

struct CpeLevel {
    std::vector<CpePart> paths;
    std::unordered_map<Vertex, std::vector<std::size_t>> byEndpoint;
};

inline std::uint64_t cpeLevelBytes(const CpeLevel& level) {
    std::uint64_t bytes = level.paths.capacity() * sizeof(CpePart);
    bytes += static_cast<std::uint64_t>(level.byEndpoint.bucket_count()) * sizeof(void*);
    bytes += static_cast<std::uint64_t>(level.byEndpoint.size()) *
             (sizeof(std::pair<const Vertex, std::vector<std::size_t>>) + 2 * sizeof(void*));
    for (const auto& kv : level.byEndpoint) bytes += kv.second.capacity() * sizeof(std::size_t);
    return bytes;
}

struct CpeAdaptMetrics {
    double staticSubgraphMs = 0.0;
    double temporalSubgraphMs = 0.0;
    double partialIndexMs = 0.0;
    double joinMs = 0.0;
    double totalMs = 0.0;
    std::uint32_t hopBound = 0;
    std::uint64_t staticVertices = 0;
    std::uint64_t staticArcs = 0;
    std::uint64_t topologyBfsEdgeScans = 0;
    std::uint64_t inducedEdgeChecks = 0;
    std::uint64_t intervalEvents = 0;
    std::uint64_t temporalSubgraphEvents = 0;
    std::uint64_t temporalMapEdgeScans = 0;
    std::uint64_t staticNeighborChecks = 0;
    std::uint64_t temporalRangeQueries = 0;
    std::uint64_t temporalEventVisits = 0;
    std::uint64_t partialPaths = 0;
    std::uint64_t partialVertexSlots = 0;
    std::uint64_t joinChecks = 0;
    std::uint64_t joinGapPass = 0;
    std::uint64_t staticSubgraphBytes = 0;
    std::uint64_t temporalSubgraphBytes = 0;
    std::uint64_t partialStoragePeakBytes = 0;
    std::uint64_t queryPeakBytes = 0;
    bool timedOut = false;
    bool unsupportedHop = false;
    bool resourceCap = false;
    std::vector<std::pair<unsigned, unsigned>> plan;
    ResultSummary result;
};

inline CpeAdaptMetrics runCpeStrict(const Graph& graph, const TopologyIndex& topology,
                                    const Query& query, Deadline& deadline,
                                    std::uint32_t maximumSupportedHop = 0,
                                    std::uint64_t partialPathCap = 0) {
    CpeAdaptMetrics metrics;
    const auto totalStart = Clock::now();
    metrics.hopBound = temporalHopUpperBound(query);
    (void)maximumSupportedHop;
    if (metrics.hopBound == 0) {
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    const auto subStart = Clock::now();
    CpeStaticSubgraph sub = buildCpeStaticSubgraph(topology, query, metrics.hopBound);
    metrics.staticSubgraphMs = elapsedMs(subStart);
    metrics.staticVertices = sub.vertices;
    metrics.staticArcs = sub.arcs;
    metrics.topologyBfsEdgeScans = sub.bfsEdgeScans;
    metrics.inducedEdgeChecks = sub.inducedEdgeChecks;
    metrics.staticSubgraphBytes = sub.bytes;

    const auto temporalStart = Clock::now();
    TemporalArcStore store = buildTemporalArcStore(graph, query, sub.allowedArcs,
                                                    deadline, &metrics.temporalMapEdgeScans);
    metrics.temporalSubgraphMs = elapsedMs(temporalStart);
    metrics.intervalEvents = store.intervalEvents;
    metrics.temporalSubgraphEvents = store.mappedEvents;
    metrics.temporalSubgraphBytes = store.bytes;
    if (deadline.stopped) {
        metrics.timedOut = true;
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    if (const auto* direct = findArcEvents(store, query.source, query.target)) {
        for (EdgeId id : *direct) {
            (void)id;
            metrics.result.addFastRecord(1);
        }
    }
    if (metrics.hopBound < 2) {
        metrics.result.finishEmpty();
        metrics.queryPeakBytes = metrics.staticSubgraphBytes + metrics.temporalSubgraphBytes +
                                 metrics.result.allocatedBytes();
        metrics.totalMs = elapsedMs(totalStart);
        return metrics;
    }

    std::vector<CpeLevel> left(metrics.hopBound + 1), right(metrics.hopBound + 1);
    left[0].paths.push_back({query.source, 0, std::numeric_limits<std::size_t>::max(),
                             std::numeric_limits<EdgeId>::max()});
    right[0].paths.push_back({query.target, 0, std::numeric_limits<std::size_t>::max(),
                              std::numeric_limits<EdgeId>::max()});

    std::uint64_t operations = 0;
    (void)partialPathCap;

    auto updatePartialBytes = [&]() {
        std::uint64_t bytes = 0;
        for (const auto& level : left) bytes += cpeLevelBytes(level);
        for (const auto& level : right) bytes += cpeLevelBytes(level);
        metrics.partialStoragePeakBytes = std::max(metrics.partialStoragePeakBytes, bytes);
    };

    auto expandLeft = [&](unsigned depth) -> bool {
        auto& previous = left[depth - 1];
        auto& current = left[depth];
        for (std::size_t pi = 0; pi < previous.paths.size(); ++pi) {
            if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                return false;
            }
            const CpePart parent = previous.paths[pi];
            for (Vertex v : sub.outgoing[parent.endpoint]) {
                ++metrics.staticNeighborChecks;
                if (v == query.source || v == query.target) continue;
                if (sub.distanceToTarget[v] < 0 ||
                    static_cast<std::uint64_t>(depth) +
                    static_cast<std::uint64_t>(sub.distanceToTarget[v]) > metrics.hopBound) continue;
                const auto* ids = findArcEvents(store, parent.endpoint, v);
                if (!ids) continue;
                const Time low = depth == 1 ? query.x1 : safeAdd(parent.boundary, query.effectiveMin());
                const Time high = depth == 1 ? query.x2 : safeAdd(parent.boundary, query.deltaMax);
                ++metrics.temporalRangeQueries;
                const auto [begin, end] = timeRangeInEdgeIds(graph, *ids, low, high);
                for (std::size_t p = begin; p < end; ++p) {
                    ++metrics.temporalEventVisits;
                    const EdgeId id = (*ids)[p];
                    const std::size_t ci = current.paths.size();
                    current.paths.push_back({v, graph.edges[id].time, pi, id});
                    current.byEndpoint[v].push_back(ci);
                    ++metrics.partialPaths;
                    metrics.partialVertexSlots += static_cast<std::uint64_t>(depth) + 1ULL;
                    if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                        metrics.timedOut = true;
                        return false;
                    }
                }
            }
        }
        updatePartialBytes();
        return true;
    };

    auto expandRight = [&](unsigned depth) -> bool {
        auto& previous = right[depth - 1];
        auto& current = right[depth];
        for (std::size_t pi = 0; pi < previous.paths.size(); ++pi) {
            if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                return false;
            }
            const CpePart parent = previous.paths[pi];
            for (Vertex v : sub.incoming[parent.endpoint]) {
                ++metrics.staticNeighborChecks;
                if (v == query.source || v == query.target) continue;
                if (sub.distanceFromSource[v] < 0 ||
                    static_cast<std::uint64_t>(depth) +
                    static_cast<std::uint64_t>(sub.distanceFromSource[v]) > metrics.hopBound) continue;
                const auto* ids = findArcEvents(store, v, parent.endpoint);
                if (!ids) continue;
                const Time low = depth == 1 ? query.x1 : safeAdd(parent.boundary, -query.deltaMax);
                const Time high = depth == 1 ? query.x2 : safeAdd(parent.boundary, -query.effectiveMin());
                ++metrics.temporalRangeQueries;
                const auto [begin, end] = timeRangeInEdgeIds(graph, *ids, low, high);
                for (std::size_t p = begin; p < end; ++p) {
                    ++metrics.temporalEventVisits;
                    const EdgeId id = (*ids)[p];
                    const std::size_t ci = current.paths.size();
                    current.paths.push_back({v, graph.edges[id].time, pi, id});
                    current.byEndpoint[v].push_back(ci);
                    ++metrics.partialPaths;
                    metrics.partialVertexSlots += static_cast<std::uint64_t>(depth) + 1ULL;
                    if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                        metrics.timedOut = true;
                        return false;
                    }
                }
            }
        }
        updatePartialBytes();
        return true;
    };

    const auto partialStart = Clock::now();
    unsigned i = 1, j = 1;
    if (!expandLeft(i) || !expandRight(j)) {
        metrics.partialIndexMs = elapsedMs(partialStart);
        metrics.result.finishEmpty();
        metrics.queryPeakBytes = metrics.staticSubgraphBytes + metrics.temporalSubgraphBytes +
                                 metrics.partialStoragePeakBytes + metrics.result.allocatedBytes();
        metrics.totalMs = elapsedMs(totalStart);
        return metrics;
    }
    metrics.plan.push_back({i, j});
    while (i + j < metrics.hopBound && !metrics.timedOut && !metrics.resourceCap) {
        if (left[i].paths.size() <= right[j].paths.size()) {
            ++i;
            if (!expandLeft(i)) break;
        } else {
            ++j;
            if (!expandRight(j)) break;
        }
        metrics.plan.push_back({i, j});
    }
    metrics.partialIndexMs = elapsedMs(partialStart);

    if (!metrics.timedOut && !metrics.resourceCap) {
        const auto joinStart = Clock::now();
        for (const auto& [li, rj] : metrics.plan) {
            auto& leftBuckets = left[li].byEndpoint;
            auto& rightBuckets = right[rj].byEndpoint;
            const bool swap = leftBuckets.size() > rightBuckets.size();
            auto& small = swap ? rightBuckets : leftBuckets;
            auto& large = swap ? leftBuckets : rightBuckets;
            for (const auto& kv : small) {
                auto it = large.find(kv.first);
                if (it == large.end()) continue;
                const auto& leftIds = swap ? it->second : kv.second;
                const auto& rightIds = swap ? kv.second : it->second;
                for (std::size_t a : leftIds) {
                    const Time leftTime = left[li].paths[a].boundary;
                    for (std::size_t b : rightIds) {
                        ++metrics.joinChecks;
                        const Time rightTime = right[rj].paths[b].boundary;
                        const Time gap = safeAdd(rightTime, -leftTime);
                        if (gap >= query.effectiveMin() && gap <= query.deltaMax) {
                            ++metrics.joinGapPass;
                            metrics.result.addFastRecord(static_cast<std::uint64_t>(li) + rj);
                        }
                        if ((++operations & 1048575ULL) == 0 && deadline.pollNow()) {
                            metrics.timedOut = true;
                            break;
                        }
                    }
                    if (metrics.timedOut) break;
                }
                if (metrics.timedOut) break;
            }
            if (metrics.timedOut) break;
        }
        metrics.joinMs = elapsedMs(joinStart);
    }

    metrics.result.finishEmpty();
    metrics.queryPeakBytes = metrics.staticSubgraphBytes + metrics.temporalSubgraphBytes +
                             metrics.partialStoragePeakBytes + metrics.result.allocatedBytes();
    metrics.totalMs = elapsedMs(totalStart);
    return metrics;
}

inline std::string cpePlanString(const std::vector<std::pair<unsigned, unsigned>>& plan) {
    std::ostringstream out;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (i) out << ';';
        out << plan[i].first << '+' << plan[i].second;
    }
    return out.str();
}

struct TspgUpperGraph {
    std::vector<EdgeId> edgeIds;
    std::vector<std::vector<EdgeId>> outgoing;
    std::uint64_t bytes = 0;
};

struct TspgAdaptMetrics {
    double forwardMs = 0.0;
    double backwardMs = 0.0;
    double verificationMs = 0.0;
    double upperMaterializeMs = 0.0;
    double enumerationMs = 0.0;
    double totalMs = 0.0;
    std::uint64_t intervalEvents = 0;
    std::uint64_t forwardStates = 0;
    std::uint64_t backwardStates = 0;
    std::uint64_t forwardEdgeScans = 0;
    std::uint64_t backwardEdgeScans = 0;
    std::uint64_t propagationGapChecks = 0;
    std::uint64_t verifyEdgeScans = 0;
    std::uint64_t timestampProbes = 0;
    std::uint64_t supportEvents = 0;
    std::uint64_t dfsStates = 0;
    std::uint64_t dfsEdgeScans = 0;
    std::uint64_t dfsGapChecks = 0;
    std::uint64_t timestampStateBytes = 0;
    std::uint64_t upperGraphBytes = 0;
    std::uint64_t dfsScratchPeakBytes = 0;
    std::uint64_t queryPeakBytes = 0;
    bool timedOut = false;
    ResultSummary result;
};

inline std::uint64_t timeSetApproxBytes(const std::vector<std::unordered_set<Time>>& sets) {
    std::uint64_t bytes = sets.capacity() * sizeof(std::unordered_set<Time>);
    for (const auto& set : sets) {
        bytes += static_cast<std::uint64_t>(set.bucket_count()) * sizeof(void*);
        bytes += static_cast<std::uint64_t>(set.size()) * (sizeof(Time) + 2 * sizeof(void*));
    }
    return bytes;
}

inline bool sequentialTimestampExists(const std::vector<Time>& times, Time low, Time high,
                                      std::uint64_t& probes) {
    if (low > high) return false;
    for (Time value : times) {
        ++probes;
        if (value >= low && value <= high) return true;
    }
    return false;
}

inline TspgAdaptMetrics runTspgStrict(const Graph& graph, const Query& query, Deadline& deadline) {
    TspgAdaptMetrics metrics;
    const auto totalStart = Clock::now();
    const auto [rangeBegin64, rangeEnd64] = graph.globalRange(query.x1, query.x2);
    const EdgeId rangeBegin = static_cast<EdgeId>(rangeBegin64);
    const EdgeId rangeEnd = static_cast<EdgeId>(rangeEnd64);
    metrics.intervalEvents = rangeEnd64 - rangeBegin64;
    const std::size_t n = graph.vertexCount();

    std::vector<std::vector<Time>> forwardTimes(n), backwardTimes(n);
    std::vector<std::unordered_set<Time>> forwardSeen(n), backwardSeen(n);
    std::deque<std::pair<Vertex, Time>> forwardQueue, backwardQueue;

    auto addForward = [&](Vertex vertex, Time time) {
        if (vertex == query.target) return;
        if (forwardSeen[vertex].insert(time).second) {
            forwardTimes[vertex].push_back(time);
            forwardQueue.emplace_back(vertex, time);
            ++metrics.forwardStates;
        }
    };
    auto addBackward = [&](Vertex vertex, Time time) {
        if (vertex == query.source) return;
        if (backwardSeen[vertex].insert(time).second) {
            backwardTimes[vertex].push_back(time);
            backwardQueue.emplace_back(vertex, time);
            ++metrics.backwardStates;
        }
    };

    const auto forwardStart = Clock::now();
    {
        const auto [begin, end] = graph.adjacencyRange(true, query.source, query.x1, query.x2);
        for (std::uint64_t p = begin; p < end; ++p) {
            ++metrics.forwardEdgeScans;
            const EdgeId id = graph.outIds[p];
            const auto& edge = graph.edges[id];
            if (!endpointAdmissible(edge, query)) continue;
            addForward(edge.dst, edge.time);
        }
        std::uint64_t operations = 0;
        while (!forwardQueue.empty()) {
            if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                break;
            }
            const auto [u, arrival] = forwardQueue.front();
            forwardQueue.pop_front();
            const auto [begin, end] = graph.adjacencyRange(true, u, query.x1, query.x2);
            for (std::uint64_t p = begin; p < end; ++p) {
                ++metrics.forwardEdgeScans;
                ++metrics.propagationGapChecks;
                const EdgeId id = graph.outIds[p];
                const auto& edge = graph.edges[id];
                if (!endpointAdmissible(edge, query)) continue;
                const Time gap = safeAdd(edge.time, -arrival);
                if (gap < query.effectiveMin() || gap > query.deltaMax) continue;
                addForward(edge.dst, edge.time);
            }
        }
    }
    metrics.forwardMs = elapsedMs(forwardStart);
    if (metrics.timedOut) {
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    const auto backwardStart = Clock::now();
    {
        const auto [begin, end] = graph.adjacencyRange(false, query.target, query.x1, query.x2);
        for (std::uint64_t p = begin; p < end; ++p) {
            ++metrics.backwardEdgeScans;
            const EdgeId id = graph.inIds[p];
            const auto& edge = graph.edges[id];
            if (!endpointAdmissible(edge, query)) continue;
            addBackward(edge.src, edge.time);
        }
        std::uint64_t operations = 0;
        while (!backwardQueue.empty()) {
            if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
                metrics.timedOut = true;
                break;
            }
            const auto [v, departure] = backwardQueue.front();
            backwardQueue.pop_front();
            const auto [begin, end] = graph.adjacencyRange(false, v, query.x1, query.x2);
            for (std::uint64_t p = begin; p < end; ++p) {
                ++metrics.backwardEdgeScans;
                ++metrics.propagationGapChecks;
                const EdgeId id = graph.inIds[p];
                const auto& edge = graph.edges[id];
                if (!endpointAdmissible(edge, query)) continue;
                const Time gap = safeAdd(departure, -edge.time);
                if (gap < query.effectiveMin() || gap > query.deltaMax) continue;
                addBackward(edge.src, edge.time);
            }
        }
    }
    metrics.backwardMs = elapsedMs(backwardStart);
    if (metrics.timedOut) {
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    metrics.timestampStateBytes =
        forwardTimes.capacity() * sizeof(std::vector<Time>) +
        backwardTimes.capacity() * sizeof(std::vector<Time>) +
        forwardQueue.size() * sizeof(std::pair<Vertex, Time>) +
        backwardQueue.size() * sizeof(std::pair<Vertex, Time>);
    for (const auto& row : forwardTimes) metrics.timestampStateBytes += row.capacity() * sizeof(Time);
    for (const auto& row : backwardTimes) metrics.timestampStateBytes += row.capacity() * sizeof(Time);
    metrics.timestampStateBytes += timeSetApproxBytes(forwardSeen) + timeSetApproxBytes(backwardSeen);

    const auto verifyStart = Clock::now();
    std::vector<EdgeId> support;
    support.reserve(static_cast<std::size_t>(metrics.intervalEvents));
    std::uint64_t operations = 0;
    for (EdgeId id = rangeBegin; id < rangeEnd; ++id) {
        ++metrics.verifyEdgeScans;
        if ((++operations & 4095ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            break;
        }
        const auto& edge = graph.edges[id];
        if (!endpointAdmissible(edge, query)) continue;
        bool prefix = edge.src == query.source;
        if (!prefix) {
            prefix = sequentialTimestampExists(forwardTimes[edge.src],
                safeAdd(edge.time, -query.deltaMax),
                safeAdd(edge.time, -query.effectiveMin()), metrics.timestampProbes);
        }
        bool suffix = edge.dst == query.target;
        if (!suffix) {
            suffix = sequentialTimestampExists(backwardTimes[edge.dst],
                safeAdd(edge.time, query.effectiveMin()),
                safeAdd(edge.time, query.deltaMax), metrics.timestampProbes);
        }
        if (prefix && suffix) support.push_back(id);
    }
    metrics.verificationMs = elapsedMs(verifyStart);
    metrics.supportEvents = support.size();
    if (metrics.timedOut) {
        metrics.totalMs = elapsedMs(totalStart);
        metrics.result.finishEmpty();
        return metrics;
    }

    const auto materializeStart = Clock::now();
    TspgUpperGraph upper;
    upper.edgeIds = support;
    upper.outgoing.assign(n, {});
    for (EdgeId id : support) upper.outgoing[graph.edges[id].src].push_back(id);
    upper.bytes = upper.edgeIds.capacity() * sizeof(EdgeId) +
                  upper.outgoing.capacity() * sizeof(std::vector<EdgeId>);
    for (const auto& row : upper.outgoing) upper.bytes += row.capacity() * sizeof(EdgeId);
    metrics.upperGraphBytes = upper.bytes;
    metrics.upperMaterializeMs = elapsedMs(materializeStart);

    const auto searchStart = Clock::now();
    std::uint64_t dfsOps = 0;
    std::function<void(Vertex, Time, std::uint32_t)> dfs = [&](Vertex u, Time last, std::uint32_t depth) {
        if (metrics.timedOut) return;
        ++metrics.dfsStates;
        if ((++dfsOps & 4095ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            return;
        }
        const auto& row = upper.outgoing[u];
        Time low = query.x1, high = query.x2;
        if (depth > 0) {
            low = safeAdd(last, query.effectiveMin());
            high = safeAdd(last, query.deltaMax);
        }
        auto first = std::lower_bound(row.begin(), row.end(), low,
            [&](EdgeId id, Time value) { return graph.edges[id].time < value; });
        auto lastIt = std::upper_bound(first, row.end(), high,
            [&](Time value, EdgeId id) { return value < graph.edges[id].time; });
        for (auto it = first; it != lastIt; ++it) {
            ++metrics.dfsEdgeScans;
            if (depth > 0) ++metrics.dfsGapChecks;
            const auto& edge = graph.edges[*it];
            const std::uint64_t length = static_cast<std::uint64_t>(depth) + 1ULL;
            metrics.dfsScratchPeakBytes = std::max<std::uint64_t>(metrics.dfsScratchPeakBytes,
                length * (sizeof(Vertex) + sizeof(Time) + sizeof(EdgeId)));
            if (edge.dst == query.target) metrics.result.addFastRecord(length);
            else {
                dfs(edge.dst, edge.time, depth + 1);
                if (metrics.timedOut) return;
            }
        }
    };
    dfs(query.source, 0, 0);
    metrics.enumerationMs = elapsedMs(searchStart);
    metrics.result.finishEmpty();
    metrics.queryPeakBytes = metrics.timestampStateBytes + metrics.upperGraphBytes +
                             metrics.dfsScratchPeakBytes + metrics.result.allocatedBytes();
    metrics.totalMs = elapsedMs(totalStart);
    return metrics;
}

struct TWalkMetrics {
    FilterMetrics filter;
    double tightMaterializeMs = 0.0;
    std::uint64_t tightGraphBytes = 0;
    std::uint64_t tightVertices = 0;
    SuccessorStats successor;
    WindowChoice window;
    SearchMetrics search;
    double totalMs = 0.0;
    std::uint64_t queryPeakBytes = 0;
    bool timedOut = false;
};

inline TWalkMetrics runTWalkEnum(const Graph& graph, const Query& query, Deadline& deadline,
                                 std::uint64_t workers = 1,
                                 std::uint64_t prefixBudgetNodes = 0) {
    TWalkMetrics metrics;
    const auto totalStart = Clock::now();
    FilterOutput filter = slidingTightGraph(graph, query, deadline);
    metrics.filter = filter.metrics;
    if (filter.metrics.timedOut || deadline.stopped) {
        metrics.timedOut = true;
        metrics.totalMs = elapsedMs(totalStart);
        metrics.queryPeakBytes = filter.metrics.peakBytes;
        return metrics;
    }
    const auto tightStart = Clock::now();
    TightGraph tight = makeTightGraph(graph, query, std::move(filter.edgeIds));
    metrics.tightMaterializeMs = elapsedMs(tightStart);
    metrics.tightGraphBytes = tight.bytes;
    metrics.tightVertices = tight.vertexCount();
    metrics.successor = buildSuccessorStats(tight, deadline);
    if (deadline.stopped) {
        metrics.timedOut = true;
        metrics.totalMs = elapsedMs(totalStart);
        metrics.queryPeakBytes = std::max(filter.metrics.peakBytes, tight.bytes + metrics.successor.bytes);
        return metrics;
    }
    metrics.window = equationWindow(tight, metrics.successor, workers);
    metrics.search = pivotDfs(tight, metrics.successor, metrics.window,
                              deadline, prefixBudgetNodes, workers);
    metrics.timedOut = deadline.stopped || metrics.search.timedOut;
    const std::uint64_t enumerationResident = tight.bytes + metrics.successor.bytes +
                                              metrics.search.searchWorkspacePeakBytes;
    metrics.queryPeakBytes = std::max(filter.metrics.peakBytes, enumerationResident);
    metrics.totalMs = elapsedMs(totalStart);
    return metrics;
}

}
