

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "types.hpp"
#include "preprocess.hpp"
#include "enumeration.hpp"
#include "strict_adaptations.hpp"

namespace tpx {

enum class SearchVariant { Full, NoChrono, NoGapBalance, NoPivot };
enum class WindowMode { GapBalance, Random, Centred };

inline const char* variantName(SearchVariant v) {
    switch (v) {
        case SearchVariant::Full: return "full";
        case SearchVariant::NoChrono: return "no_chrono";
        case SearchVariant::NoGapBalance: return "no_gapbalance";
        case SearchVariant::NoPivot: return "no_pivot";
    }
    return "full";
}

inline const char* windowName(WindowMode w) {
    switch (w) {
        case WindowMode::GapBalance: return "gb";
        case WindowMode::Random: return "random";
        case WindowMode::Centred: return "centred";
    }
    return "gb";
}

struct SearchOptions {
    SearchVariant variant = SearchVariant::Full;
    WindowMode windowMode = WindowMode::GapBalance;
    std::uint64_t windowSeed = 0;
    std::uint64_t workers = 1;
    std::uint64_t prefixBudget = 0;
    double softTimeoutS = 0.0;
};

struct SearchRun {
    TWalkMetrics tw;
    std::uint64_t eligibleEvents = 0;
    std::uint64_t filterEvents = 0;
    bool directSearch = false;
    bool timedOut = false;
    double totalMs = 0.0;
};

inline TightGraph makeUnfilteredTightGraph(const Graph& graph, const Query& query,
                                           std::uint64_t* eligibleOut) {
    std::vector<EdgeId> ids;
    const auto range = graph.globalRange(query.x1, query.x2);
    ids.reserve(static_cast<std::size_t>(range.second - range.first));
    for (std::uint64_t i = range.first; i < range.second; ++i) {
        const auto& edge = graph.edges[i];
        if (edge.src == query.target || edge.dst == query.source) continue;
        ids.push_back(static_cast<EdgeId>(i));
    }
    if (eligibleOut) *eligibleOut = ids.size();
    return makeTightGraph(graph, query, std::move(ids));
}

inline WindowChoice centredWindow(const TightGraph& tight) {
    const auto start = Clock::now();
    WindowChoice choice;
    choice.T = tight.query.points();
    choice.W = std::min<Time>(std::max<Time>(1, tight.query.deltaMax), choice.T);
    choice.D = std::max<Time>(0, choice.T - choice.W);
    choice.offset = choice.D / 2;
    choice.left = safeAdd(tight.query.x1, choice.offset);
    choice.right = safeAdd(choice.left, choice.W - 1);
    choice.branch = "centred";
    choice.selectionMs = elapsedMs(start);
    return choice;
}

inline SearchRun runSearch(const Graph& graph, const Query& query, const SearchOptions& opts) {
    SearchRun out;
    Deadline deadline(opts.softTimeoutS);
    const auto totalStart = Clock::now();

    TightGraph tight;
    if (opts.variant == SearchVariant::NoChrono) {
        const auto t0 = Clock::now();
        tight = makeUnfilteredTightGraph(graph, query, &out.eligibleEvents);
        out.tw.tightMaterializeMs = elapsedMs(t0);
        out.tw.tightGraphBytes = tight.bytes;
        out.tw.tightVertices = tight.vertexCount();
        out.filterEvents = tight.edgeIds.size();
    } else {
        FilterOutput filter = slidingTightGraph(graph, query, deadline);
        out.tw.filter = filter.metrics;
        out.eligibleEvents = filter.metrics.rangeEdges;
        out.filterEvents = filter.edgeIds.size();
        if (filter.metrics.timedOut || deadline.stopped) {
            out.timedOut = true;
            out.tw.timedOut = true;
            out.totalMs = elapsedMs(totalStart);
            out.tw.totalMs = out.totalMs;
            return out;
        }
        const auto t0 = Clock::now();
        tight = makeTightGraph(graph, query, std::move(filter.edgeIds));
        out.tw.tightMaterializeMs = elapsedMs(t0);
        out.tw.tightGraphBytes = tight.bytes;
        out.tw.tightVertices = tight.vertexCount();
    }

    out.tw.successor = buildSuccessorStats(tight, deadline);
    if (deadline.stopped) {
        out.timedOut = true;
        out.tw.timedOut = true;
        out.totalMs = elapsedMs(totalStart);
        out.tw.totalMs = out.totalMs;
        return out;
    }

    WindowChoice window;
    switch (opts.variant == SearchVariant::NoGapBalance ? WindowMode::Centred : opts.windowMode) {
        case WindowMode::GapBalance:
            window = equationWindow(tight, out.tw.successor, opts.workers);
            break;
        case WindowMode::Random:
            window = randomWindow(tight, opts.windowSeed);
            break;
        case WindowMode::Centred:
            window = centredWindow(tight);
            break;
    }
    out.tw.window = window;

    const std::uint64_t budget = 0;

    if (opts.variant == SearchVariant::NoPivot) {
        out.directSearch = true;
        out.tw.search = opts.workers > 1
            ? parallelDirectDfs(tight, out.tw.successor, deadline, opts.workers)
            : directDfs(tight, out.tw.successor, deadline);
    } else {
        out.tw.search = pivotDfs(tight, out.tw.successor, window, deadline, budget, opts.workers);
    }

    out.timedOut = deadline.stopped || out.tw.search.timedOut;
    out.tw.timedOut = out.timedOut;
    out.totalMs = elapsedMs(totalStart);
    out.tw.totalMs = out.totalMs;
    return out;
}

}
