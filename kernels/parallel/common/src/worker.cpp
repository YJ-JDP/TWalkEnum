#include "worker.hpp"
#include "strict_adaptations.hpp"

namespace tpx {

struct QueryRow {
    Query query;
    std::uint64_t baseQueryId = 0;
    std::uint64_t acceptanceTightEdges = 0;
    std::uint64_t acceptanceRangeEdges = 0;
    int acceptanceNonempty = -1;
    double acceptanceCheckMs = 0.0;
};

static void printQueries(const Graph& graph, const std::vector<QueryRow>& rows) {
    std::cout << "TPATH_QUERIES_V1\n";
    std::cout << "query_id\tbase_query_id\tquery_seed\tgeneration_mode\tsource_internal_id\ttarget_internal_id"
                 "\tsource_external_id\ttarget_external_id\tx1\tx2\tdelta_min\tdelta_max\tgap_ratio"
                 "\tgap_placement_id\tacceptance_tight_edges\tacceptance_range_edges"
                 "\tacceptance_nonempty\tacceptance_check_ms\n";
    for (const auto& row : rows) {
        const auto& q = row.query;
        std::cout << q.queryId << '\t' << row.baseQueryId << '\t' << q.seed << '\t' << q.generationMode << '\t'
                  << q.source << '\t' << q.target << '\t' << graph.externalVertices[q.source] << '\t'
                  << graph.externalVertices[q.target] << '\t' << q.x1 << '\t' << q.x2 << '\t'
                  << q.deltaMin << '\t' << q.deltaMax << '\t' << std::fixed << std::setprecision(6)
                  << q.gapRatio << '\t' << q.placementId << '\t' << row.acceptanceTightEdges << '\t'
                  << row.acceptanceRangeEdges << '\t' << row.acceptanceNonempty << '\t'
                  << std::fixed << std::setprecision(6) << row.acceptanceCheckMs << '\n';
    }
    std::cout.flush();
}

static std::pair<Time, Time> randomTimeRange(const Graph& graph, std::mt19937_64& rng) {
    if (graph.maxTime <= graph.minTime)
        throw std::runtime_error("query sampling requires a positive graph time span");
    std::uniform_int_distribution<Time> startDistribution(graph.minTime, graph.maxTime - 1);
    const Time x1 = startDistribution(rng);
    std::uniform_int_distribution<Time> lengthDistribution(1, graph.maxTime - x1);
    return {x1, x1 + lengthDistribution(rng)};
}

static std::pair<Vertex, Vertex> randomEndpoints(const Graph& graph, Time x1, Time x2,
                                                 std::mt19937_64& rng) {
    const auto [begin, end] = graph.globalRange(x1, x2);
    if (begin == end) return {0, 0};
    std::uniform_int_distribution<std::uint64_t> edgeDistribution(begin, end - 1);
    Vertex source = graph.edges[edgeDistribution(rng)].src;
    Vertex target = graph.edges[edgeDistribution(rng)].dst;
    for (int tries = 0; tries < 16 && source == target; ++tries)
        target = graph.edges[edgeDistribution(rng)].dst;
    return {source, target};
}

static std::vector<QueryRow> generatePairedQueries(const Graph& graph, const Args& args) {
    const std::uint64_t wanted = args.u64("query-count", 100);
    const std::uint64_t maxAttempts = args.u64("max-attempts", 1000000);
    const std::uint64_t minQueryEdges = args.u64("min-query-edges", 10);
    const std::uint64_t seed = args.u64("seed", 2026100801ULL);
    const bool requireNonempty = args.u64("require-nonempty", 1) != 0;
    const double generationTimeout = args.real("generation-total-timeout", 7200.0);
    Deadline generationDeadline(generationTimeout);
    std::mt19937_64 rng(seed);
    std::set<std::tuple<Vertex, Vertex, Time, Time, Time, Time>> unique;
    const std::array<std::pair<double, double>, 2> gaps{{{0.10, 0.20}, {0.20, 0.40}}};
    std::vector<QueryRow> rows;
    std::uint64_t accepted = 0;

    for (std::uint64_t attempt = 0; attempt < maxAttempts && accepted < wanted; ++attempt) {
        if (generationDeadline.pollNow()) break;
        Query base;
        const auto range = randomTimeRange(graph, rng);
        base.x1 = range.first;
        base.x2 = range.second;
        const auto endpoints = randomEndpoints(graph, base.x1, base.x2, rng);
        base.source = endpoints.first;
        base.target = endpoints.second;
        if (!base.syntacticallyValid()) continue;
        const auto interval = graph.globalRange(base.x1, base.x2);
        if (interval.second - interval.first < minQueryEdges) continue;

        std::array<Query, 2> variants;
        std::array<std::uint64_t, 2> tightEdges{{0, 0}};
        std::array<double, 2> checkMs{{0.0, 0.0}};
        bool valid = true;
        for (std::size_t profile = 0; profile < gaps.size(); ++profile) {
            Query q = base;
            const Time span = std::max<Time>(1, q.x2 - q.x1);
            q.deltaMin = std::max<Time>(1, static_cast<Time>(std::ceil(static_cast<double>(span) * gaps[profile].first)));
            q.deltaMax = std::max(q.deltaMin, static_cast<Time>(std::ceil(static_cast<double>(span) * gaps[profile].second)));
            q.seed = splitmix64(seed ^ (static_cast<std::uint64_t>(profile) << 48) ^ attempt);
            q.gapRatio = gaps[profile].second - gaps[profile].first;
            q.placementId = static_cast<int>(profile);
            q.generationMode = profile == 0 ? "paired-gap-10-20" : "paired-gap-20-40";
            if (!q.syntacticallyValid() || !q.multiEdgeFeasible()) { valid = false; break; }
            const auto key = std::make_tuple(q.source, q.target, q.x1, q.x2, q.deltaMin, q.deltaMax);
            if (unique.count(key)) { valid = false; break; }
            if (requireNonempty) {
                const auto start = Clock::now();
                FilterOutput filter = slidingTightGraph(graph, q, generationDeadline);
                checkMs[profile] = elapsedMs(start);
                if (filter.metrics.timedOut || generationDeadline.stopped || filter.edgeIds.empty()) {
                    valid = false;
                    break;
                }
                tightEdges[profile] = filter.edgeIds.size();
            }
            variants[profile] = q;
        }
        if (!valid) continue;
        const std::uint64_t baseId = accepted++;
        for (std::size_t profile = 0; profile < variants.size(); ++profile) {
            Query q = variants[profile];
            q.queryId = rows.size();
            unique.emplace(q.source, q.target, q.x1, q.x2, q.deltaMin, q.deltaMax);
            rows.push_back({q, baseId, tightEdges[profile], interval.second - interval.first,
                            requireNonempty ? 1 : -1, checkMs[profile]});
        }
    }
    return rows;
}

static std::string methodStatus(bool timedOut, bool unsupported = false, bool resourceCap = false) {
    if (timedOut) return "TIMEOUT_SOFT";
    if (resourceCap) return "RESOURCE_CAP";
    if (unsupported) return "UNSUPPORTED_HOP_BOUND";
    return "OK";
}

static void addCommon(Record& record, const std::string& method, const Graph& graph,
                      const Query& query, double wallMs, std::uint64_t workers = 1) {
    record.set("experiment", "main");
    record.set("method", method);
    record.set("workers", workers);
    record.setDouble("wall_ms", wallMs);
    record.set("peak_rss_bytes", peakRssBytes());
    addGraphRecord(record, graph);
    addQueryRecord(record, graph, query);
    record.set("temporal_hop_bound", temporalHopUpperBound(query));
}

static void addCountSummary(Record& record, const ResultSummary& result) {
    record.set("path_count", result.count.str());
    record.set("output_edge_ids", result.outputVolume.str());
    record.set("query_nonempty", result.count.isZero() ? 0 : 1);
    record.set("min_path_length", result.minLength);
    record.set("max_path_length", result.maxLength);
    record.set("validation_mode", "COUNT_ONLY");
}

static void appendResponse(Record& record, const ResponseProbe& probe) {
    if (probe.target == 0) return;
    record.set("response_target", probe.target);
    record.set("response_observed_at_most_target", probe.seen);
    record.set("response_reached", probe.reached ? 1 : 0);
    if (probe.reached) record.setDouble("response_1000_ms", probe.firstTargetMs);
    else record.set("response_1000_ms", "");
    const std::string status = record.values.count("status") ? record.values.at("status") : "";
    record.set("response_status", probe.reached ? "REACHED" :
        (status == "OK" ? "FEWER_THAN_1000" : "TIMEOUT_OR_INCOMPLETE"));
    record.set("response_timing", "ONLINE_QUERY_EXCLUDING_PERSISTENT_GRAPH_AND_TOPOLOGY_LOAD");
}

static Record runPathEnum(const Graph& graph, const TopologyIndex& topology,
                          const Query& query, const Args& args, double topologyLoadMs) {
    const auto wallStart = Clock::now();
    Deadline deadline(args.real("soft-timeout", 120.0));
    const auto maxHop = std::uint32_t{0};
    PathEnumAdaptMetrics m = runPathEnumStrict(graph, topology, query, deadline, maxHop);
    Record r;
    addCommon(r, "PathEnum-DFS-style", graph, query, elapsedMs(wallStart));
    r.set("status", methodStatus(m.timedOut || deadline.stopped, m.unsupportedHop));
    r.setDouble("topology_index_load_ms", topologyLoadMs);
    r.set("topology_index_bytes", topologyIndexBytes(topology));
    r.setDouble("pathenum_index_ms", m.topologyQueryIndexMs);
    r.setDouble("temporal_subgraph_ms", m.temporalSubgraphMs);
    r.setDouble("enumeration_ms", m.enumerationMs);
    r.setDouble("online_total_ms", m.totalMs);
    r.set("static_subgraph_vertices", m.staticVertices);
    r.set("static_subgraph_arcs", m.staticArcs);
    r.set("topology_bfs_edge_scans", m.topologyBfsEdgeScans);
    r.set("temporal_interval_events", m.intervalEvents);
    r.set("temporal_subgraph_events", m.temporalSubgraphEvents);
    r.set("temporal_map_edge_scans", m.temporalMapEdgeScans);
    r.set("static_neighbor_checks", m.staticNeighborChecks);
    r.set("temporal_range_queries", m.temporalRangeQueries);
    r.set("temporal_event_visits", m.temporalEventVisits);
    r.set("search_states", m.dfsStates);
    r.set("gap_accepted_transitions", m.gapAcceptedTransitions);
    r.set("query_index_bytes", m.queryIndexBytes);
    r.set("temporal_subgraph_bytes", m.temporalSubgraphBytes);
    r.set("dfs_scratch_peak_bytes", m.dfsScratchPeakBytes);
    r.set("result_sink_bytes", m.result.allocatedBytes());
    r.set("query_local_peak_bytes_estimate", m.queryPeakBytes);
    addCountSummary(r, m.result);
    return r;
}

static Record runCpe(const Graph& graph, const TopologyIndex& topology,
                     const Query& query, const Args& args, double topologyLoadMs) {
    const auto wallStart = Clock::now();
    Deadline deadline(args.real("soft-timeout", 120.0));
    const auto maxHop = std::uint32_t{0};
    const std::uint64_t partialCap = std::uint64_t{0};
    CpeAdaptMetrics m = runCpeStrict(graph, topology, query, deadline, maxHop, partialCap);
    Record r;
    addCommon(r, "CPE-startup-style", graph, query, elapsedMs(wallStart));
    r.set("status", methodStatus(m.timedOut || deadline.stopped, m.unsupportedHop, m.resourceCap));
    r.setDouble("topology_index_load_ms", topologyLoadMs);
    r.set("topology_index_bytes", topologyIndexBytes(topology));
    r.setDouble("cpe_static_subgraph_ms", m.staticSubgraphMs);
    r.setDouble("temporal_subgraph_ms", m.temporalSubgraphMs);
    r.setDouble("cpe_partial_index_ms", m.partialIndexMs);
    r.setDouble("cpe_join_ms", m.joinMs);
    r.setDouble("online_total_ms", m.totalMs);
    r.set("static_subgraph_vertices", m.staticVertices);
    r.set("static_subgraph_arcs", m.staticArcs);
    r.set("topology_bfs_edge_scans", m.topologyBfsEdgeScans);
    r.set("induced_edge_checks", m.inducedEdgeChecks);
    r.set("temporal_interval_events", m.intervalEvents);
    r.set("temporal_subgraph_events", m.temporalSubgraphEvents);
    r.set("temporal_map_edge_scans", m.temporalMapEdgeScans);
    r.set("static_neighbor_checks", m.staticNeighborChecks);
    r.set("temporal_range_queries", m.temporalRangeQueries);
    r.set("temporal_event_visits", m.temporalEventVisits);
    r.set("partial_paths", m.partialPaths);
    r.set("partial_vertex_slots", m.partialVertexSlots);
    r.set("join_checks", m.joinChecks);
    r.set("join_gap_pass", m.joinGapPass);
    r.set("cpe_plan", cpePlanString(m.plan));
    r.set("static_subgraph_bytes", m.staticSubgraphBytes);
    r.set("temporal_subgraph_bytes", m.temporalSubgraphBytes);
    r.set("partial_storage_peak_bytes", m.partialStoragePeakBytes);
    r.set("result_sink_bytes", m.result.allocatedBytes());
    r.set("query_local_peak_bytes_estimate", m.queryPeakBytes);
    addCountSummary(r, m.result);
    return r;
}

static Record runTspg(const Graph& graph, const Query& query, const Args& args) {
    const auto wallStart = Clock::now();
    Deadline deadline(args.real("soft-timeout", 120.0));
    TspgAdaptMetrics m = runTspgStrict(graph, query, deadline);
    Record r;
    addCommon(r, "TSPG-style", graph, query, elapsedMs(wallStart));
    r.set("status", methodStatus(m.timedOut || deadline.stopped));
    r.setDouble("tspg_forward_ms", m.forwardMs);
    r.setDouble("tspg_backward_ms", m.backwardMs);
    r.setDouble("tspg_verification_ms", m.verificationMs);
    r.setDouble("upper_graph_materialize_ms", m.upperMaterializeMs);
    r.setDouble("enumeration_ms", m.enumerationMs);
    r.setDouble("online_total_ms", m.totalMs);
    r.set("temporal_interval_events", m.intervalEvents);
    r.set("tspg_forward_states", m.forwardStates);
    r.set("tspg_backward_states", m.backwardStates);
    r.set("tspg_forward_edge_scans", m.forwardEdgeScans);
    r.set("tspg_backward_edge_scans", m.backwardEdgeScans);
    r.set("tspg_propagation_gap_checks", m.propagationGapChecks);
    r.set("tspg_verify_edge_scans", m.verifyEdgeScans);
    r.set("tspg_timestamp_probes", m.timestampProbes);
    r.set("temporal_support_events", m.supportEvents);
    r.set("search_states", m.dfsStates);
    r.set("temporal_edge_scans", m.dfsEdgeScans);
    r.set("temporal_gap_checks", m.dfsGapChecks);
    r.set("timestamp_state_bytes", m.timestampStateBytes);
    r.set("upper_graph_bytes", m.upperGraphBytes);
    r.set("dfs_scratch_peak_bytes", m.dfsScratchPeakBytes);
    r.set("result_sink_bytes", m.result.allocatedBytes());
    r.set("query_local_peak_bytes_estimate", m.queryPeakBytes);
    addCountSummary(r, m.result);
    return r;
}

static Record runTwalk(const Graph& graph, const Query& query, const Args& args) {
    const auto wallStart = Clock::now();
    Deadline deadline(args.real("soft-timeout", 120.0));
    const std::uint64_t workers = std::max<std::uint64_t>(1, args.u64("workers", 1));
    const std::uint64_t prefixBudget = std::uint64_t{0};
    TWalkMetrics m = runTWalkEnum(graph, query, deadline, workers, prefixBudget);
    Record r;
    addCommon(r, "TWalkEnum", graph, query, elapsedMs(wallStart), workers);
    r.set("status", methodStatus(m.timedOut || deadline.stopped));
    r.setDouble("chrono_total_ms", m.filter.totalMs);
    r.setDouble("chrono_range_locate_ms", m.filter.rangeLocateMs);
    r.setDouble("chrono_forward_ms", m.filter.forwardMs);
    r.setDouble("chrono_backward_ms", m.filter.backwardMs);
    r.setDouble("chrono_intersection_ms", m.filter.intersectMs);
    r.setDouble("tight_materialize_ms", m.tightMaterializeMs);
    r.setDouble("successor_stats_ms", m.successor.buildMs);
    r.setDouble("gapbalance_ms", m.window.selectionMs);
    r.setDouble("pivot_preflight_ms", m.search.preflightMs);
    r.setDouble("pivot_prefix_ms", m.search.prefixMs);
    r.setDouble("pivot_suffix_ms", m.search.suffixMs);
    r.setDouble("enumeration_ms", m.search.totalMs);
    r.setDouble("online_total_ms", m.totalMs);
    r.set("temporal_interval_events", m.filter.rangeEdges);
    r.set("chrono_scan_forward", m.filter.forwardScanned);
    r.set("chrono_scan_backward", m.filter.backwardScanned);
    r.set("chrono_attempt_forward", m.filter.forwardAttempts);
    r.set("chrono_attempt_backward", m.filter.backwardAttempts);
    r.set("chrono_success_forward", m.filter.forwardSuccess);
    r.set("chrono_success_backward", m.filter.backwardSuccess);
    r.set("temporal_support_events", m.filter.tightEdges);
    r.set("tight_vertices", m.tightVertices);
    r.set("tight_transition_count", m.successor.transitionCount);
    r.set("search_states", m.search.states);
    r.set("preflight_states", m.search.preflightStates);
    r.set("prefix_states", m.search.prefixStates);
    r.set("suffix_states", m.search.suffixStates);
    r.set("active_pivots", m.search.activePivots);
    r.set("processed_pivots", m.search.processedPivots);
    r.set("stored_prefix_count", m.search.storedPrefixes);
    r.set("stored_prefix_edge_volume", m.search.storedPrefixEdgeIds);
    r.set("window_left", m.window.left);
    r.set("window_right", m.window.right);
    r.set("window_width", m.window.W);
    r.set("window_offset", m.window.offset);
    r.setDouble("branching_geomean", m.window.branching);
    r.setDouble("mean_successor_gap", m.window.meanGap);
    r.set("chrono_peak_bytes", m.filter.peakBytes);
    r.set("tight_graph_bytes", m.tightGraphBytes);
    r.set("successor_stats_bytes", m.successor.bytes);
    r.set("search_workspace_peak_bytes", m.search.searchWorkspacePeakBytes);
    r.set("prefix_storage_bytes", m.search.prefixCapacityBytes + m.search.prefixBucketBytes + m.search.activePivotBytes);
    r.set("result_sink_bytes", m.search.result.allocatedBytes());
    r.set("query_local_peak_bytes_estimate", m.queryPeakBytes);
    r.set("fallback_to_direct", m.search.fallbackToDirect ? 1 : 0);
    r.set("fallback_reason", m.search.fallbackReason);
    addCountSummary(r, m.search.result);
    return r;
}

int workerMain(const std::string& experiment, int argc, char** argv) {
    try {
        (void)experiment;
        Args args(argc, argv);
        const std::string action = args.get("action", "run");
        if (action == "cache-graph") {
            LoadOptions options;
            options.delimiter = args.get("delimiter", "space") == "comma" ? ',' : ' ';
            options.srcColumn = static_cast<int>(args.i64("src-column", 0));
            options.dstColumn = static_cast<int>(args.i64("dst-column", 1));
            options.timeColumn = static_cast<int>(args.i64("time-column", 2));
            options.idColumn = static_cast<int>(args.i64("id-column", -1));
            options.skipHeader = args.flag("skip-header");
            options.maxBadRows = args.u64("max-bad-rows", 0);
            Graph graph = loadTextGraph(args.get("graph-file"), options);
            saveGraphCache(graph, args.get("graph-cache"));
            Record r; r.set("status", "OK"); r.set("action", action); addGraphRecord(r, graph); r.print();
            return 0;
        }

        Graph graph = loadGraphCache(args.get("graph-cache"));
        if (action == "build-topology-index") {
            const auto start = Clock::now();
            TopologyIndex topology(graph);
            const double ms = elapsedMs(start);
            saveTopologyIndex(topology, args.get("topology-index-file"));
            Record r; r.set("status", "OK"); r.set("action", action); r.set("method", "TOPOLOGY_INDEX_BUILD");
            r.setDouble("topology_index_build_ms", ms); r.set("topology_index_bytes", topologyIndexBytes(topology));
            r.set("index_serialized_bytes", std::filesystem::file_size(args.get("topology-index-file")));
            addGraphRecord(r, graph); r.print(); return 0;
        }
        if (action == "generate") {
            printQueries(graph, generatePairedQueries(graph, args));
            return 0;
        }
        if (action != "run") throw std::runtime_error("unknown action: " + action);

        Query query = queryFromArgs(args);
        const std::string method = args.get("algorithm");
        ResponseProbe probe;
        probe.target = args.u64("response-target", 0);
        probe.stopAtTarget = args.u64("response-only", 0) != 0;
        if (probe.stopAtTarget && probe.target == 0)
            throw std::runtime_error("--response-only requires --response-target");
        if (probe.target != 0 && args.u64("workers", 1) != 1)
            throw std::runtime_error("response threshold mode requires --workers 1");
        auto show = [&](Record r) -> int {
            if (probe.target) appendResponse(r, probe);
            activeResponseProbe = nullptr;
            r.print();
            return 0;
        };
        try {
            if (method == "PathEnum-DFS-style" || method == "CPE-startup-style") {
                const auto loadStart = Clock::now();
                TopologyIndex topology = loadTopologyIndex(args.get("topology-index-file"));
                requireTopologyIndexMatch(topology, graph);
                const double loadMs = elapsedMs(loadStart);
                if (method == "CPE-startup-style" && probe.target != 0)
                    throw std::runtime_error("CPE not selected for 1000-answer response evaluation");
                probe.begin = Clock::now();
                activeResponseProbe = probe.target ? &probe : nullptr;
                return show(method == "PathEnum-DFS-style"
                    ? runPathEnum(graph, topology, query, args, loadMs)
                    : runCpe(graph, topology, query, args, loadMs));
            }
            probe.begin = Clock::now();
            activeResponseProbe = probe.target ? &probe : nullptr;
            if (method == "TSPG-style") return show(runTspg(graph, query, args));
            if (method == "TWalkEnum") return show(runTwalk(graph, query, args));
            throw std::runtime_error("unknown algorithm: " + method);
        } catch (const ResponseTargetReached&) {

            activeResponseProbe = nullptr;
            Record r;
            addCommon(r, method, graph, query, probe.firstTargetMs);
            r.set("status", "RESPONSE_TARGET_REACHED");
            r.set("response_only", 1);
            r.set("path_count", "");
            appendResponse(r, probe);
            r.print();
            return 0;
        }

    } catch (const std::bad_alloc&) {
        Record r; r.set("status", "OOM"); r.set("error", "std::bad_alloc"); r.print(); return 3;
    } catch (const std::exception& e) {
        Record r; r.set("status", "ERROR"); r.set("error", e.what()); r.print(); return 2;
    }
}

}
