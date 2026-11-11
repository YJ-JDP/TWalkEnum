#pragma once

#include "preprocess.hpp"

namespace tpx {

struct SuccessorStats {
    EdgeId base = 0;
    std::vector<std::uint64_t> ranges;
    std::uint64_t expandableEdges = 0;
    std::uint64_t transitionCount = 0;
    double logBranching = 0.0;
    double branching = 1.0;
    double meanGap = 1.0;
    double logExpansionPerTime = 0.0;
    double buildMs = 0.0;
    std::uint64_t bytes = 0;

    std::pair<std::uint32_t, std::uint32_t> rangeOf(EdgeId id) const {
        const std::uint64_t packed = ranges[static_cast<std::size_t>(id - base)];
        return {static_cast<std::uint32_t>(packed >> 32),
                static_cast<std::uint32_t>(packed)};
    }
};

inline double cachedLogCount(std::uint64_t count) {
    static const std::array<double, 4097> table = [] {
        std::array<double, 4097> values{};
        for (std::size_t i = 1; i < values.size(); ++i) values[i] = std::log(static_cast<double>(i));
        return values;
    }();
    return count < table.size() ? table[static_cast<std::size_t>(count)]
                                : std::log(static_cast<double>(count));
}

inline SuccessorStats buildSuccessorStats(const TightGraph& tight, Deadline& deadline) {
    const auto start = Clock::now();
    SuccessorStats stats;
    const auto queryRange = tight.graph->globalRange(tight.query.x1, tight.query.x2);
    stats.base = queryRange.first;
    const std::uint64_t rangeSize64 = queryRange.second - queryRange.first;
    if (rangeSize64 > std::numeric_limits<std::uint32_t>::max() ||
        tight.edgeIds.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("query/tight graph exceeds 32-bit successor-range implementation limit");
    }
    const std::size_t rangeSize = static_cast<std::size_t>(rangeSize64);
    stats.ranges.assign(rangeSize, 0);

    long double logSum = 0.0L;
    long double gapSum = 0.0L;
    std::uint64_t processed = 0;
    for (std::size_t vertex = 0; vertex < tight.vertexCount(); ++vertex) {
        const std::uint64_t inBegin = tight.inOffsets[vertex];
        const std::uint64_t inEnd = tight.inOffsets[vertex + 1];
        const std::uint64_t outBegin = tight.outOffsets[vertex];
        const std::uint64_t outEnd = tight.outOffsets[vertex + 1];
        if (inBegin == inEnd) continue;

        std::uint64_t left = outBegin;
        std::uint64_t right = outBegin;
        long double windowTimeSum = 0.0L;
        for (std::uint64_t p = inBegin; p < inEnd; ++p) {
            if ((++processed & 4095ULL) == 0 && deadline.pollNow()) {
                stats.bytes = stats.ranges.capacity() * sizeof(std::uint64_t);
                stats.buildMs = elapsedMs(start);
                return stats;
            }
            const EdgeId incoming = tight.inIds[p];
            const Time time = tight.graph->edges[incoming].time;
            const Time low = safeAdd(time, tight.query.effectiveMin());
            const Time high = safeAdd(time, tight.query.deltaMax);

            while (left < outEnd && tight.graph->edges[tight.outIds[left]].time < low) {
                if (left < right) windowTimeSum -= tight.graph->edges[tight.outIds[left]].time;
                ++left;
            }
            if (right < left) right = left;
            while (right < outEnd && tight.graph->edges[tight.outIds[right]].time <= high) {
                windowTimeSum += tight.graph->edges[tight.outIds[right]].time;
                ++right;
            }

            const std::size_t offset = static_cast<std::size_t>(incoming - stats.base);
            const std::uint32_t packedLeft = static_cast<std::uint32_t>(left);
            const std::uint32_t packedRight = static_cast<std::uint32_t>(right);
            stats.ranges[offset] = (static_cast<std::uint64_t>(packedLeft) << 32) | packedRight;
            const std::uint64_t count = right - left;
            stats.transitionCount += count;
            if (count > 0) {
                ++stats.expandableEdges;
                logSum += static_cast<long double>(cachedLogCount(count));
                gapSum += windowTimeSum / static_cast<long double>(count) - time;
            }
        }
    }

    if (stats.expandableEdges > 0) {
        stats.logBranching = static_cast<double>(logSum / stats.expandableEdges);
        stats.branching = std::exp(stats.logBranching);
        stats.meanGap = std::max(1.0, static_cast<double>(gapSum / stats.expandableEdges));
        stats.logExpansionPerTime = stats.logBranching / stats.meanGap;
    }
    stats.bytes = stats.ranges.capacity() * sizeof(std::uint64_t);
    stats.buildMs = elapsedMs(start);
    return stats;
}

struct WindowChoice {
    Time left = 0;
    Time right = 0;
    Time offset = 0;
    Time T = 0;
    Time W = 0;
    Time D = 0;
    std::uint64_t sourceEdges = 0;
    double rawExpectedPivotEdges = 0.0;
    double expectedPivotEdges = 0.0;
    double effectiveParallelism = 1.0;
    double branching = 1.0;
    double meanGap = 1.0;
    double logR = 0.0;
    double selectionMs = 0.0;
    std::string branch = "regular";
};

// GapBalance uses the dominant exponential term to balance prefix and parallel suffix work.
// Evaluating the balance point in log space avoids large powers and keeps planning lightweight.
inline WindowChoice equationWindow(const TightGraph& tight, const SuccessorStats& stats,
                                   std::uint64_t workers = 1) {
    const auto start = Clock::now();
    WindowChoice choice;
    choice.T = tight.query.points();
    choice.W = std::min<Time>(std::max<Time>(1, tight.query.deltaMax), choice.T);
    choice.D = std::max<Time>(0, choice.T - choice.W);
    choice.branching = stats.branching;
    choice.meanGap = stats.meanGap;
    // Exponential growth per timestamp: log(d) / mean successor gap.
    choice.logR = stats.logExpansionPerTime;
    if (const auto localSource = tight.localOf(tight.query.source)) {
        choice.sourceEdges = tight.outOffsets[*localSource + 1] - tight.outOffsets[*localSource];
    }
    const double M = static_cast<double>(tight.edgeIds.size());
    if (tight.edgeIds.empty() || choice.sourceEdges == 0) {
        choice.branch = "no-solution";
        choice.offset = 0;
        choice.left = tight.query.x1;
        choice.right = choice.left;
        choice.selectionMs = elapsedMs(start);
        return choice;
    }
    const double S = static_cast<double>(choice.sourceEdges);
    // Estimate window entries from event density; entries bound the useful worker count.
    const double rawH = M * static_cast<double>(choice.W) / static_cast<double>(choice.T);
    const double effectiveWorkers = std::min(
        static_cast<double>(std::max<std::uint64_t>(1, workers)), std::max(1.0, rawH));
    // Per-worker entry weight; this H is not the depth scale used in the paper.
    const double H = rawH / effectiveWorkers;
    choice.rawExpectedPivotEdges = rawH;
    choice.effectiveParallelism = effectiveWorkers;
    choice.expectedPivotEdges = H;
    if (choice.D == 0) {
        choice.offset = 0;
        choice.branch = "fixed";
    } else if (choice.branching == 1.0) {
        choice.branch = "chain";
        if (S > H) choice.offset = 0;
        else if (S < H) choice.offset = choice.D;
        else choice.offset = choice.D / 2;
    } else {
        // Balance S * exp(logR * a) against H * exp(logR * (D - 1 - a)).
        const double raw = (static_cast<double>(choice.D) - 1.0 +
            (std::log(H) - std::log(S)) / choice.logR) / 2.0;
        // Map the continuous balance point to a feasible integer window offset.
        const double clipped = std::clamp(std::ceil(raw), 0.0, static_cast<double>(choice.D));
        choice.offset = static_cast<Time>(clipped);
    }
    choice.left = safeAdd(tight.query.x1, choice.offset);
    choice.right = safeAdd(choice.left, choice.W - 1);
    choice.selectionMs = elapsedMs(start);
    return choice;
}

inline WindowChoice randomWindow(const TightGraph& tight, std::uint64_t seed) {
    const auto start = Clock::now();
    WindowChoice choice;
    choice.T = tight.query.points();
    choice.W = std::min<Time>(std::max<Time>(1, tight.query.deltaMax), choice.T);
    choice.D = std::max<Time>(0, choice.T - choice.W);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Time> distribution(0, choice.D);
    choice.offset = distribution(rng);
    choice.left = safeAdd(tight.query.x1, choice.offset);
    choice.right = safeAdd(choice.left, choice.W - 1);
    choice.selectionMs = elapsedMs(start);
    choice.branch = "random";
    return choice;
}

struct SearchMetrics {
    double totalMs = 0.0;
    double prefixMs = 0.0;
    double suffixMs = 0.0;
    double preflightMs = 0.0;
    double selectionMs = 0.0;
    std::uint64_t states = 0;
    std::uint64_t preflightStates = 0;
    std::uint64_t requiredPrefixNodes = 0;
    std::uint64_t prefixBudgetNodes = 0;
    std::uint64_t prefixStates = 0;
    std::uint64_t suffixStates = 0;
    std::uint64_t activePivots = 0;
    std::uint64_t processedPivots = 0;
    std::uint64_t storedPrefixes = 0;
    std::uint64_t storedPrefixEdgeIds = 0;
    std::uint64_t prefixPayloadBytes = 0;
    std::uint64_t prefixCapacityBytes = 0;
    std::uint64_t prefixBucketBytes = 0;
    std::uint64_t prefixArenaNodes = 0;
    std::uint64_t prefixArenaAllocatedBytes = 0;
    std::uint64_t pivotHandleCount = 0;
    std::uint64_t pivotHandleAllocatedBytes = 0;
    std::uint64_t activePivotBytes = 0;
    std::uint64_t directDfsScratchBytes = 0;
    std::uint64_t prefixDfsScratchBytes = 0;
    std::uint64_t suffixDfsScratchBytes = 0;
    std::uint64_t searchWorkspacePeakBytes = 0;
    std::uint64_t resultSinkBytes = 0;
    std::uint64_t maxPathLength = 0;
    bool fallbackToDirect = false;

    bool prefixStorageSaturated = false;
    std::string fallbackReason;
    bool timedOut = false;
    std::uint64_t workersUsed = 1;
    std::uint64_t parallelTasks = 0;
    ResultSummary result;
};

struct DfsFrame {
    std::uint64_t next = 0;
    std::uint64_t end = 0;
};

inline std::size_t dfsReserveHint(const TightGraph& tight) {
    const std::uint64_t hop = temporalHopUpperBound(tight.query);
    return static_cast<std::size_t>(std::min<std::uint64_t>(hop, tight.edgeIds.size()));
}

template <class Stats>
inline SearchMetrics directDfs(const TightGraph& tight, const Stats& stats,
                               Deadline& deadline) {
    SearchMetrics metrics;
    const auto start = Clock::now();

    std::vector<ConcreteEdgeId> pathOriginal;
    std::vector<DfsFrame> frames;
    const std::size_t reserveHint = dfsReserveHint(tight);
    pathOriginal.reserve(reserveHint);
    frames.reserve(reserveHint + 1);
    const auto [sourceBegin, sourceEnd] = tight.outRange(
        tight.query.source, tight.query.x1, tight.query.x2);
    metrics.parallelTasks = sourceEnd - sourceBegin;
    frames.push_back({sourceBegin, sourceEnd});

    while (!frames.empty()) {
        if ((metrics.states & 4095ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            break;
        }
        auto& frame = frames.back();
        if (frame.next >= frame.end) {
            frames.pop_back();
            if (!pathOriginal.empty()) pathOriginal.pop_back();
            continue;
        }
        const EdgeId id = tight.outIds[frame.next++];
        const auto& edge = tight.graph->edges[id];
        pathOriginal.push_back(static_cast<ConcreteEdgeId>(edge.originalId));
        ++metrics.states;
        metrics.maxPathLength = std::max<std::uint64_t>(metrics.maxPathLength, pathOriginal.size());
        if (edge.dst == tight.query.target) {
            metrics.result.emitOriginalPath(pathOriginal);
            pathOriginal.pop_back();
            continue;
        }
        const auto [left, right] = stats.rangeOf(id);
        if (left < right) frames.push_back({left, right});
        else pathOriginal.pop_back();
    }
    metrics.result.finishEmpty();
    metrics.directDfsScratchBytes =
        pathOriginal.capacity() * sizeof(ConcreteEdgeId) +
        frames.capacity() * sizeof(DfsFrame);
    metrics.resultSinkBytes = metrics.result.allocatedBytes();
    metrics.searchWorkspacePeakBytes = metrics.directDfsScratchBytes + metrics.resultSinkBytes;
    metrics.totalMs = elapsedMs(start);
    return metrics;
}

template <class Stats>
inline SearchMetrics parallelDirectDfs(const TightGraph& tight, const Stats& stats,
                                       Deadline& deadline, std::uint64_t requestedWorkers) {
    if (requestedWorkers <= 1) return directDfs(tight, stats, deadline);
    SearchMetrics combined;
    const auto start = Clock::now();
    const auto sourceRange = tight.outRange(
        tight.query.source, tight.query.x1, tight.query.x2);
    const std::uint64_t sourceBegin = sourceRange.first;
    const std::uint64_t sourceEnd = sourceRange.second;
    const std::uint64_t taskCount = sourceEnd - sourceBegin;
    if (taskCount == 0) {
        combined.result.finishEmpty();
        combined.resultSinkBytes = combined.result.allocatedBytes();
        combined.searchWorkspacePeakBytes = combined.resultSinkBytes;
        combined.totalMs = elapsedMs(start);
        return combined;
    }
    const std::uint64_t workers = std::min<std::uint64_t>(
        std::max<std::uint64_t>(1, requestedWorkers), taskCount);
    if (workers == 1) return directDfs(tight, stats, deadline);
    combined.workersUsed = workers;
    combined.parallelTasks = taskCount;
    std::atomic<std::uint64_t> nextTask{0};
    std::vector<SearchMetrics> local(static_cast<std::size_t>(workers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (std::uint64_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            Deadline localDeadline = deadline;
            auto& metrics = local[static_cast<std::size_t>(worker)];
            std::vector<ConcreteEdgeId> pathOriginal;
            std::vector<DfsFrame> frames;
            const std::size_t reserveHint = dfsReserveHint(tight);
            pathOriginal.reserve(reserveHint);
            frames.reserve(reserveHint + 1);
            constexpr std::uint64_t TASK_BATCH = 8;
            while (true) {
                const std::uint64_t batchBegin = nextTask.fetch_add(TASK_BATCH, std::memory_order_relaxed);
                if (batchBegin >= taskCount || localDeadline.pollNow()) break;
                const std::uint64_t batchEnd = std::min<std::uint64_t>(taskCount, batchBegin + TASK_BATCH);
                for (std::uint64_t task = batchBegin; task < batchEnd; ++task) {
                    const EdgeId first = tight.outIds[sourceBegin + task];
                    const auto& firstEdge = tight.graph->edges[first];
                    pathOriginal.assign(1, static_cast<ConcreteEdgeId>(firstEdge.originalId));
                    frames.clear();
                    ++metrics.states;
                    metrics.maxPathLength = std::max<std::uint64_t>(metrics.maxPathLength, 1);
                    if (firstEdge.dst == tight.query.target) {
                        metrics.result.emitOriginalPath(pathOriginal);
                        continue;
                    }
                    const auto [left, right] = stats.rangeOf(first);
                    if (left < right) frames.push_back({left, right});
                    while (!frames.empty()) {
                        if ((metrics.states & 4095ULL) == 0 && localDeadline.pollNow()) break;
                        auto& frame = frames.back();
                        if (frame.next >= frame.end) {
                            frames.pop_back();
                            if (pathOriginal.size() > 1) pathOriginal.pop_back();
                            continue;
                        }
                        const EdgeId id = tight.outIds[frame.next++];
                        const auto& edge = tight.graph->edges[id];
                        pathOriginal.push_back(static_cast<ConcreteEdgeId>(edge.originalId));
                        ++metrics.states;
                        metrics.maxPathLength = std::max<std::uint64_t>(metrics.maxPathLength, pathOriginal.size());
                        if (edge.dst == tight.query.target) {
                            metrics.result.emitOriginalPath(pathOriginal);
                            pathOriginal.pop_back();
                            continue;
                        }
                        const auto [childLeft, childRight] = stats.rangeOf(id);
                        if (childLeft < childRight) frames.push_back({childLeft, childRight});
                        else pathOriginal.pop_back();
                    }
                    if (localDeadline.stopped) break;
                }
                if (localDeadline.stopped) break;
            }
            metrics.timedOut = localDeadline.stopped;
            metrics.result.finishEmpty();
            metrics.directDfsScratchBytes =
                pathOriginal.capacity() * sizeof(ConcreteEdgeId) +
                frames.capacity() * sizeof(DfsFrame);
            metrics.resultSinkBytes = metrics.result.allocatedBytes();
            metrics.searchWorkspacePeakBytes = metrics.directDfsScratchBytes + metrics.resultSinkBytes;
        });
    }
    for (auto& thread : threads) thread.join();
    std::uint64_t concurrentScratchBytes = 0;
    std::uint64_t concurrentSinkBytes = 0;
    for (const auto& metrics : local) {
        combined.states += metrics.states;
        combined.maxPathLength = std::max(combined.maxPathLength, metrics.maxPathLength);
        combined.timedOut = combined.timedOut || metrics.timedOut;
        combined.directDfsScratchBytes += metrics.directDfsScratchBytes;
        concurrentScratchBytes += metrics.directDfsScratchBytes;
        concurrentSinkBytes += metrics.resultSinkBytes;
        combined.result.merge(metrics.result);
    }
    combined.result.finishEmpty();
    combined.resultSinkBytes = combined.result.allocatedBytes();
    combined.searchWorkspacePeakBytes = concurrentScratchBytes + concurrentSinkBytes +
        local.capacity() * sizeof(SearchMetrics) + threads.capacity() * sizeof(std::thread);
    combined.totalMs = elapsedMs(start);
    return combined;
}

using FlatPrefixHandle = StoredSpanRef;

template <class Stats>
inline SearchMetrics pivotDfs(const TightGraph& tight, const Stats& stats,
                              const WindowChoice& window, Deadline& deadline,
                              std::uint64_t prefixBudgetNodes = 0,
                              std::uint64_t requestedWorkers = 1) {
    SearchMetrics metrics;
    metrics.selectionMs = window.selectionMs;
    metrics.prefixBudgetNodes = 0;
    (void)prefixBudgetNodes;
    const auto totalStart = Clock::now();

    if (!tight.query.multiEdgeFeasible() || tight.query.deltaMax == 0) {
        SearchMetrics direct = parallelDirectDfs(tight, stats, deadline, requestedWorkers);
        direct.selectionMs = window.selectionMs;
        direct.prefixBudgetNodes = 0;
        direct.fallbackToDirect = true;
        direct.fallbackReason = "single_edge_only";
        return direct;
    }

    const auto prefixStart = Clock::now();

    std::vector<ConcreteEdgeId> flatPrefixes;

    const auto [windowEdgeBegin, windowEdgeEnd] = tight.graph->globalRange(window.left, window.right);
    std::vector<std::int32_t> pivotSlot(static_cast<std::size_t>(windowEdgeEnd - windowEdgeBegin), -1);
    std::vector<EdgeId> activePivots;

    std::vector<std::vector<FlatPrefixHandle>> pivotHandles;
    std::vector<ConcreteEdgeId> pathOriginal;
    std::vector<DfsFrame> frames;
    const std::size_t reserveHint = dfsReserveHint(tight);
    pathOriginal.reserve(reserveHint);
    frames.reserve(reserveHint + 1);
    activePivots.reserve(std::min<std::size_t>(pivotSlot.size(), 4096));
    pivotHandles.reserve(std::min<std::size_t>(pivotSlot.size(), 4096));
    std::uint64_t largestRequiredPrefixPayload = 0;

    const auto [sourceBegin, sourceEnd] = tight.outRange(
        tight.query.source, tight.query.x1, tight.query.x2);
    frames.push_back({sourceBegin, sourceEnd});

    while (!frames.empty()) {
        if ((metrics.prefixStates & 4095ULL) == 0 && deadline.pollNow()) {
            metrics.timedOut = true;
            break;
        }
        auto& frame = frames.back();
        if (frame.next >= frame.end) {
            frames.pop_back();
            if (!pathOriginal.empty()) pathOriginal.pop_back();
            continue;
        }

        const EdgeId id = tight.outIds[frame.next++];
        const auto& edge = tight.graph->edges[id];
        pathOriginal.push_back(static_cast<ConcreteEdgeId>(edge.originalId));
        ++metrics.prefixStates;
        metrics.maxPathLength = std::max<std::uint64_t>(metrics.maxPathLength, pathOriginal.size());

        if (edge.dst == tight.query.target) {
            metrics.result.emitOriginalPath(pathOriginal);
            pathOriginal.pop_back();
            continue;
        }

        const bool inWindow = id >= windowEdgeBegin && id < windowEdgeEnd;
        if (inWindow) {
            const std::uint64_t required = static_cast<std::uint64_t>(flatPrefixes.size()) + pathOriginal.size();
            largestRequiredPrefixPayload = std::max(largestRequiredPrefixPayload, required);
            if (pathOriginal.size() > std::numeric_limits<std::uint32_t>::max() ||
                pathOriginal.size() > flatPrefixes.max_size() - flatPrefixes.size()) {
                throw std::length_error("prefix storage exceeds addressable representation");
            }
            const std::size_t local = static_cast<std::size_t>(id - windowEdgeBegin);
            std::int32_t slot = pivotSlot[local];
            if (slot < 0) {
                if (activePivots.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                    throw std::runtime_error("active pivot count exceeds int32_t slot representation");
                }
                slot = static_cast<std::int32_t>(activePivots.size());
                pivotSlot[local] = slot;
                activePivots.push_back(id);
                pivotHandles.emplace_back();
            }
            FlatPrefixHandle handle;
            handle.offset = static_cast<std::uint64_t>(flatPrefixes.size());
            handle.length = static_cast<std::uint32_t>(pathOriginal.size());
            const std::size_t oldSize = flatPrefixes.size();
            flatPrefixes.resize(oldSize + pathOriginal.size());
            std::memcpy(flatPrefixes.data() + oldSize, pathOriginal.data(),
                        pathOriginal.size() * sizeof(ConcreteEdgeId));
            pivotHandles[static_cast<std::size_t>(slot)].push_back(handle);
            ++metrics.storedPrefixes;
            metrics.storedPrefixEdgeIds += pathOriginal.size();
            pathOriginal.pop_back();
            continue;
        }

        const auto [left, right] = stats.rangeOf(id);
        if (left < right) frames.push_back({left, right});
        else pathOriginal.pop_back();
    }

    metrics.prefixMs = elapsedMs(prefixStart);
    metrics.requiredPrefixNodes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(flatPrefixes.size()), largestRequiredPrefixPayload);
    metrics.activePivots = activePivots.size();
    metrics.parallelTasks = activePivots.size();
    metrics.activePivotBytes = activePivots.capacity() * sizeof(EdgeId);
    metrics.prefixPayloadBytes = flatPrefixes.size() * sizeof(ConcreteEdgeId);
    metrics.prefixArenaNodes = 0;
    metrics.prefixArenaAllocatedBytes = flatPrefixes.capacity() * sizeof(ConcreteEdgeId);
    metrics.prefixCapacityBytes = metrics.prefixArenaAllocatedBytes;
    metrics.pivotHandleCount = metrics.storedPrefixes;
    metrics.pivotHandleAllocatedBytes = pivotHandles.capacity() * sizeof(std::vector<FlatPrefixHandle>);
    for (const auto& handles : pivotHandles)
        metrics.pivotHandleAllocatedBytes += handles.capacity() * sizeof(FlatPrefixHandle);
    metrics.prefixCapacityBytes += metrics.pivotHandleAllocatedBytes;
    metrics.prefixBucketBytes = pivotSlot.capacity() * sizeof(std::int32_t);
    metrics.prefixDfsScratchBytes =
        pathOriginal.capacity() * sizeof(ConcreteEdgeId) +
        frames.capacity() * sizeof(DfsFrame);
    const std::uint64_t prefixPersistentBytes = metrics.prefixArenaAllocatedBytes +
        metrics.pivotHandleAllocatedBytes + metrics.prefixBucketBytes + metrics.activePivotBytes;
    metrics.resultSinkBytes = metrics.result.allocatedBytes();
    metrics.searchWorkspacePeakBytes = prefixPersistentBytes + metrics.prefixDfsScratchBytes +
        metrics.resultSinkBytes;

    if (metrics.timedOut) {
        metrics.states = metrics.prefixStates;
        metrics.result.finishEmpty();
        metrics.resultSinkBytes = metrics.result.allocatedBytes();
        metrics.searchWorkspacePeakBytes = std::max(metrics.searchWorkspacePeakBytes,
            prefixPersistentBytes + metrics.prefixDfsScratchBytes + metrics.resultSinkBytes);
        metrics.totalMs = elapsedMs(totalStart);
        return metrics;
    }

    const auto suffixStart = Clock::now();
    if (!activePivots.empty()) {
        const std::uint64_t workers = std::min<std::uint64_t>(
            std::max<std::uint64_t>(1, requestedWorkers), activePivots.size());
        metrics.workersUsed = workers;

        auto processPivot = [&](std::size_t slot, SearchMetrics& part, Deadline& localDeadline,
                                std::vector<ConcreteEdgeId>& suffixOriginal,
                                std::vector<DfsFrame>& suffixFrames) {
            const EdgeId pivot = activePivots[slot];
            const auto& prefixes = pivotHandles[slot];
            suffixOriginal.clear();
            suffixFrames.clear();
            suffixOriginal.push_back(static_cast<ConcreteEdgeId>(tight.graph->edges[pivot].originalId));
            ++part.suffixStates;
            ++part.processedPivots;

            const auto emitAll = [&]() {
                const auto* suffixTail = suffixOriginal.data() + 1;
                const std::uint32_t suffixTailLength = static_cast<std::uint32_t>(suffixOriginal.size() - 1);
                if (prefixes.empty()) return true;
                const std::uint64_t prefixCount = static_cast<std::uint64_t>(prefixes.size());

                part.result.emitJoinedRange(
                    flatPrefixes.data(), prefixes.data(), prefixCount,
                    suffixTail, suffixTailLength);

                part.maxPathLength = std::max(part.maxPathLength, part.result.maxLength);
                return !localDeadline.pollNow();
            };

            const auto [left, right] = stats.rangeOf(pivot);
            if (left < right) suffixFrames.push_back({left, right});

            while (!suffixFrames.empty() && !localDeadline.stopped) {
                if ((part.suffixStates & 4095ULL) == 0 && localDeadline.pollNow()) break;
                auto& frame = suffixFrames.back();
                if (frame.next >= frame.end) {
                    suffixFrames.pop_back();
                    if (suffixOriginal.size() > 1) suffixOriginal.pop_back();
                    continue;
                }
                const EdgeId id = tight.outIds[frame.next++];
                const auto& edge = tight.graph->edges[id];
                suffixOriginal.push_back(static_cast<ConcreteEdgeId>(edge.originalId));
                ++part.suffixStates;
                if (edge.dst == tight.query.target) {
                    if (!emitAll()) break;
                    suffixOriginal.pop_back();
                    continue;
                }
                const auto [childLeft, childRight] = stats.rangeOf(id);
                if (childLeft < childRight) suffixFrames.push_back({childLeft, childRight});
                else suffixOriginal.pop_back();
            }
        };

        if (workers == 1) {

            Deadline localDeadline = deadline;
            SearchMetrics part;
            std::vector<ConcreteEdgeId> suffixOriginal;
            std::vector<DfsFrame> suffixFrames;
            suffixOriginal.reserve(reserveHint);
            suffixFrames.reserve(reserveHint + 1);
            for (std::size_t slot = 0; slot < activePivots.size(); ++slot) {
                if (localDeadline.pollNow()) break;
                processPivot(slot, part, localDeadline, suffixOriginal, suffixFrames);
                if (localDeadline.stopped) break;
            }
            part.result.finishEmpty();
            part.suffixDfsScratchBytes =
                suffixOriginal.capacity() * sizeof(ConcreteEdgeId) +
                suffixFrames.capacity() * sizeof(DfsFrame);
            part.resultSinkBytes = part.result.allocatedBytes();
            metrics.suffixDfsScratchBytes = part.suffixDfsScratchBytes;
            metrics.suffixStates = part.suffixStates;
            metrics.processedPivots = part.processedPivots;
            metrics.maxPathLength = std::max(metrics.maxPathLength, part.maxPathLength);
            metrics.timedOut = localDeadline.stopped;
            metrics.result.merge(part.result);
        } else {
            std::atomic<std::uint64_t> nextPivot{0};
            std::vector<SearchMetrics> local(static_cast<std::size_t>(workers));
            std::vector<std::thread> threads;
            threads.reserve(static_cast<std::size_t>(workers));
            for (std::uint64_t worker = 0; worker < workers; ++worker) {
                threads.emplace_back([&, worker]() {
                    Deadline localDeadline = deadline;
                    auto& part = local[static_cast<std::size_t>(worker)];
                    std::vector<ConcreteEdgeId> suffixOriginal;
                    std::vector<DfsFrame> suffixFrames;
                    suffixOriginal.reserve(reserveHint);
                    suffixFrames.reserve(reserveHint + 1);
                    constexpr std::uint64_t PIVOT_BATCH = 8;
                    while (!localDeadline.stopped) {
                        const std::uint64_t batchBegin = nextPivot.fetch_add(PIVOT_BATCH, std::memory_order_relaxed);
                        if (batchBegin >= activePivots.size()) break;
                        const std::uint64_t batchEnd = std::min<std::uint64_t>(
                            activePivots.size(), batchBegin + PIVOT_BATCH);
                        for (std::uint64_t slot = batchBegin; slot < batchEnd; ++slot) {
                            processPivot(static_cast<std::size_t>(slot), part, localDeadline,
                                         suffixOriginal, suffixFrames);
                            if (localDeadline.stopped) break;
                        }
                    }
                    part.timedOut = localDeadline.stopped;
                    part.result.finishEmpty();
                    part.suffixDfsScratchBytes =
                        suffixOriginal.capacity() * sizeof(ConcreteEdgeId) +
                        suffixFrames.capacity() * sizeof(DfsFrame);
                    part.resultSinkBytes = part.result.allocatedBytes();
                });
            }
            for (auto& thread : threads) thread.join();
            std::uint64_t concurrentSuffixScratchBytes = 0;
            std::uint64_t concurrentSuffixSinkBytes = 0;
            for (const auto& part : local) {
                metrics.suffixStates += part.suffixStates;
                metrics.processedPivots += part.processedPivots;
                metrics.maxPathLength = std::max(metrics.maxPathLength, part.maxPathLength);
                metrics.timedOut = metrics.timedOut || part.timedOut;
                metrics.suffixDfsScratchBytes += part.suffixDfsScratchBytes;
                concurrentSuffixScratchBytes += part.suffixDfsScratchBytes;
                concurrentSuffixSinkBytes += part.resultSinkBytes;
                metrics.result.merge(part.result);
            }
            metrics.searchWorkspacePeakBytes = std::max(metrics.searchWorkspacePeakBytes,
                prefixPersistentBytes + metrics.prefixDfsScratchBytes + concurrentSuffixScratchBytes +
                concurrentSuffixSinkBytes + local.capacity() * sizeof(SearchMetrics) +
                threads.capacity() * sizeof(std::thread));
        }
    } else {
        metrics.workersUsed = 1;
    }

    metrics.suffixMs = elapsedMs(suffixStart);
    const std::uint64_t combinedStates = metrics.prefixStates + metrics.suffixStates;
    metrics.states = combinedStates >= metrics.processedPivots
        ? combinedStates - metrics.processedPivots : 0;
    metrics.result.finishEmpty();
    metrics.resultSinkBytes = metrics.result.allocatedBytes();
    metrics.searchWorkspacePeakBytes = std::max(metrics.searchWorkspacePeakBytes,
        prefixPersistentBytes + metrics.prefixDfsScratchBytes + metrics.suffixDfsScratchBytes +
        metrics.resultSinkBytes);
    metrics.totalMs = elapsedMs(totalStart);
    return metrics;
}

inline void addWindowRecord(Record& record, const WindowChoice& w, const SuccessorStats& stats) {
    record.set("window_left", w.left); record.set("window_right", w.right);
    record.set("window_offset", w.offset); record.set("window_width", w.W);
    record.set("window_movable_domain", w.D); record.set("window_branch", w.branch);
    record.set("source_tight_edges", w.sourceEdges);
    record.setDouble("raw_expected_pivot_edges", w.rawExpectedPivotEdges);
    record.setDouble("effective_parallelism", w.effectiveParallelism);
    record.setDouble("expected_pivot_edges", w.expectedPivotEdges);
    record.setDouble("branching_geomean", w.branching); record.setDouble("mean_successor_gap", w.meanGap);
    record.setDouble("log_expansion_per_time", w.logR);
    record.setDouble("expansion_per_time_r", std::exp(w.logR));
    record.setDouble("selection_ms", w.selectionMs);
    record.setDouble("successor_stats_ms", stats.buildMs);
    record.set("expandable_tight_edges", stats.expandableEdges);
    record.set("tight_transition_count", stats.transitionCount);
    record.set("successor_stats_bytes", stats.bytes);
    record.set("window_workspace_bytes", sizeof(WindowChoice));
}

inline void addSearchRecord(Record& record, const SearchMetrics& m) {
    record.set("output_mode", "zero_copy_path_view_range_sink");
    record.setDouble("search_ms", m.totalMs);
    record.setDouble("enumeration_total_ms", m.totalMs + m.selectionMs);
    record.setDouble("preflight_ms", m.preflightMs); record.setDouble("prefix_ms", m.prefixMs);
    record.setDouble("suffix_ms", m.suffixMs); record.setDouble("selection_ms", m.selectionMs);
    record.set("search_states", m.states); record.set("preflight_states", m.preflightStates);
    const std::uint64_t pivotStates = m.fallbackToDirect
        ? m.states
        : (m.prefixStates + m.suffixStates >= m.processedPivots
            ? m.prefixStates + m.suffixStates - m.processedPivots : 0);
    record.set("pivot_search_states", pivotStates);
    record.set("required_prefix_nodes", m.requiredPrefixNodes);
    record.set("prefix_budget_nodes", m.prefixBudgetNodes);
    record.set("fallback_to_direct", m.fallbackToDirect ? 1 : 0);
    record.set("prefix_storage_saturated", m.prefixStorageSaturated ? 1 : 0);
    record.set("fallback_reason", m.fallbackReason);
    record.set("prefix_states", m.prefixStates);
    record.set("suffix_states", m.suffixStates); record.set("active_pivots", m.activePivots);
    record.set("processed_pivots", m.processedPivots);
    record.set("stored_half_paths", m.storedPrefixes); record.set("stored_half_path_edge_ids", m.storedPrefixEdgeIds);
    record.set("half_path_payload_bytes", m.prefixPayloadBytes);
    record.set("half_path_capacity_bytes", m.prefixCapacityBytes);
    record.set("half_path_bucket_bytes", m.prefixBucketBytes);
    record.set("half_path_total_bytes", m.prefixCapacityBytes + m.prefixBucketBytes + m.activePivotBytes);
    record.set("stored_prefix_count", m.storedPrefixes);
    record.set("stored_prefix_edge_volume", m.storedPrefixEdgeIds);
    record.set("prefix_payload_logical_bytes", m.prefixPayloadBytes);
    record.set("prefix_arena_allocated_bytes", m.prefixArenaAllocatedBytes);
    record.set("prefix_arena_nodes", m.prefixArenaNodes);
    record.set("prefix_arena_storage_bytes", m.prefixArenaAllocatedBytes);
    record.set("pivot_handle_count", m.pivotHandleCount);
    record.set("pivot_handle_allocated_bytes", m.pivotHandleAllocatedBytes);
    record.set("prefix_bucket_allocated_bytes", m.prefixBucketBytes);
    record.set("active_pivot_bytes", m.activePivotBytes);
    record.set("direct_dfs_scratch_bytes", m.directDfsScratchBytes);
    record.set("prefix_dfs_scratch_bytes", m.prefixDfsScratchBytes);
    record.set("suffix_dfs_scratch_bytes", m.suffixDfsScratchBytes);
    record.set("search_workspace_peak_bytes", m.searchWorkspacePeakBytes);
    record.set("result_sink_bytes", m.resultSinkBytes);
    record.set("pivot_total_extra_bytes", m.prefixCapacityBytes + m.prefixBucketBytes + m.activePivotBytes);
    record.set("max_path_length", m.maxPathLength);
    record.set("actual_workers", m.workersUsed);
    record.set("parallel_tasks", m.parallelTasks);
    record.set("path_count", m.result.count.str()); record.set("output_edge_ids", m.result.outputVolume.str());
    record.set("query_nonempty", m.result.count.isZero() ? 0 : 1);
    record.set("min_path_length", m.result.minLength); record.set("max_emitted_path_length", m.result.maxLength);
}

}
