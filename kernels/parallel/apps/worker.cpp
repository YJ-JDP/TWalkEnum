
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "types.hpp"
#include "search_experiments.hpp"

using namespace tpx;

namespace {

const std::vector<std::string>& columns() {
    static const std::vector<std::string> cols = {
        "graph", "dataset", "gap", "query_id", "variant", "window_mode", "window_seed",
        "workers", "status", "online_total_ms", "filter_ms", "tight_ms", "stats_ms",
        "window_ms", "prefix_ms", "suffix_ms", "enum_ms", "search_states", "prefix_states",
        "suffix_states", "active_pivots", "stored_prefix_count", "path_count",
        "output_edge_ids", "min_path_length", "max_path_length", "eligible_events",
        "filter_events", "window_left", "window_right", "timed_out", "peak_bytes"};
    return cols;
}

struct Row {
    std::map<std::string, std::string> v;
    void set(const std::string& k, const std::string& val) { v[k] = val; }
    void set(const std::string& k, std::uint64_t val) { v[k] = std::to_string(val); }
    void set(const std::string& k, double val) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.3f", val);
        v[k] = buf;
    }
    void print() const {
        const auto& cols = columns();
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) std::cout << '\t';
            auto it = v.find(cols[i]);
            std::cout << (it == v.end() ? std::string() : it->second);
        }
        std::cout << '\n';
    }
    static void printHeader() {
        const auto& cols = columns();
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) std::cout << '\t';
            std::cout << cols[i];
        }
        std::cout << '\n';
    }
};

}

int main(int argc, char** argv) {
    try {
        Args args(argc, argv);

        if (args.flag("print-header")) {
            Row::printHeader();
            return 0;
        }

        if (args.get("action", "run") == "cache-graph") {
            LoadOptions options;
            options.delimiter = args.get("delimiter", "space") == "comma" ? ',' : ' ';
            options.srcColumn = static_cast<int>(args.i64("src-column", 0));
            options.dstColumn = static_cast<int>(args.i64("dst-column", 1));
            options.timeColumn = static_cast<int>(args.i64("time-column", 2));
            options.idColumn = static_cast<int>(args.i64("id-column", -1));
            options.skipHeader = args.flag("skip-header");
            options.maxBadRows = args.u64("max-bad-rows", 0);
            Graph g = loadTextGraph(args.get("graph-file"), options);
            saveGraphCache(g, args.get("graph-cache"));
            std::cout << "OK\t" << g.vertexCount() << '\t' << g.edges.size() << '\n';
            return 0;
        }

        Graph graph = loadGraphCache(args.get("graph-cache"));
        Query query = queryFromArgs(args);
        if (!query.syntacticallyValid() || query.source >= graph.vertexCount() || query.target >= graph.vertexCount())
            throw std::runtime_error("invalid query or endpoint ID outside graph");

        SearchOptions opts;
        const std::string variant = args.get("variant", "full");
        if (variant == "full") opts.variant = SearchVariant::Full;
        else if (variant == "no_chrono") opts.variant = SearchVariant::NoChrono;
        else if (variant == "no_gapbalance") opts.variant = SearchVariant::NoGapBalance;
        else if (variant == "no_pivot") opts.variant = SearchVariant::NoPivot;
        else throw std::runtime_error("unknown --variant: " + variant);

        const std::string window = args.get("window-mode", "gb");
        if (window == "gb") opts.windowMode = WindowMode::GapBalance;
        else if (window == "random") opts.windowMode = WindowMode::Random;
        else if (window == "centred") opts.windowMode = WindowMode::Centred;
        else throw std::runtime_error("unknown --window-mode: " + window);

        opts.windowSeed = args.u64("window-seed", 0);
        opts.workers = std::max<std::uint64_t>(1, args.u64("workers", 1));
        opts.prefixBudget = std::uint64_t{0};
        opts.softTimeoutS = args.real("soft-timeout", 0.0);

        const SearchRun m = runSearch(graph, query, opts);
        const auto& tw = m.tw;
        const auto& s = tw.search;

        Row r;
        r.set("graph", args.get("graph-name", ""));
        r.set("dataset", args.get("dataset", ""));
        r.set("gap", args.get("gap", ""));
        r.set("query_id", std::to_string(query.queryId));
        r.set("variant", variantName(opts.variant));
        r.set("window_mode", windowName(opts.windowMode));
        r.set("window_seed", std::to_string(opts.windowSeed));
        r.set("workers", std::to_string(opts.workers));
        r.set("status", m.timedOut ? std::string("TIMEOUT")
                                   : (s.result.count.isZero() ? std::string("EMPTY") : std::string("OK")));
        r.set("online_total_ms", m.totalMs);
        r.set("filter_ms", tw.filter.totalMs);
        r.set("tight_ms", tw.tightMaterializeMs);
        r.set("stats_ms", tw.successor.buildMs);
        r.set("window_ms", tw.window.selectionMs);
        r.set("prefix_ms", s.prefixMs);
        r.set("suffix_ms", s.suffixMs);
        r.set("enum_ms", s.totalMs);
        r.set("search_states", s.states);
        r.set("prefix_states", s.prefixStates);
        r.set("suffix_states", s.suffixStates);
        r.set("active_pivots", s.activePivots);
        r.set("stored_prefix_count", s.storedPrefixes);
        r.set("path_count", s.result.count.str());
        r.set("output_edge_ids", s.result.outputVolume.str());
        r.set("min_path_length", s.result.minLength);
        r.set("max_path_length", s.result.maxLength);
        r.set("eligible_events", m.eligibleEvents);
        r.set("filter_events", m.filterEvents);
        r.set("window_left", static_cast<std::uint64_t>(tw.window.left));
        r.set("window_right", static_cast<std::uint64_t>(tw.window.right));
        r.set("timed_out", static_cast<std::uint64_t>(m.timedOut ? 1 : 0));
        r.set("peak_bytes", static_cast<std::uint64_t>(s.searchWorkspacePeakBytes));
        r.print();
        return 0;
    } catch (const std::bad_alloc&) {
        std::cout << "OOM\n";
        return 3;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
