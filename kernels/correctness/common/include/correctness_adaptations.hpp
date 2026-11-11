

#pragma once
#include "strict_adaptations.hpp"
namespace tpx {
inline PathEnumAdaptMetrics runPathEnumCaptured(const Graph& graph, const TopologyIndex& topology,
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
    std::vector<EdgeId> answerPath;
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
                answerPath.push_back(edgeId);
                if (neighbor.vertex == query.target) {
                    metrics.result.emitPath(graph, answerPath);
                } else {
                    dfs(neighbor.vertex, edge.time, used + 1);
                    if (metrics.timedOut) return;
                }
                answerPath.pop_back();
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

inline CpeAdaptMetrics runCpeCaptured(const Graph& graph, const TopologyIndex& topology,
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
            metrics.result.emitPath(graph, std::vector<EdgeId>{id});
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
                            std::vector<EdgeId> answerPath;
                            answerPath.reserve(li + rj);
                            auto leftId = a;
                            for (unsigned depth = li; depth > 0; --depth) {
                                const auto& part = left[depth].paths[leftId];
                                answerPath.push_back(part.edge); leftId = part.parent;
                            }
                            std::reverse(answerPath.begin(), answerPath.end());
                            auto rightId = b;
                            for (unsigned depth = rj; depth > 0; --depth) {
                                const auto& part = right[depth].paths[rightId];
                                answerPath.push_back(part.edge); rightId = part.parent;
                            }
                            metrics.result.emitPath(graph, answerPath);
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

inline TspgAdaptMetrics runTspgCaptured(const Graph& graph, const Query& query, Deadline& deadline) {
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
    std::vector<EdgeId> answerPath;
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
            answerPath.push_back(*it);
            if (edge.dst == query.target) metrics.result.emitPath(graph, answerPath);
            else {
                dfs(edge.dst, edge.time, depth + 1);
                if (metrics.timedOut) return;
            }
            answerPath.pop_back();
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
}
