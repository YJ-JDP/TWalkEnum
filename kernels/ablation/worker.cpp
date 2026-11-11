#include "enumeration.hpp"
#include <cstdlib>

using namespace tpx;

struct Expired {};
inline void check(Deadline& d) { if (d.pollNow()) throw Expired{}; }

struct DagRanges {
    EdgeId base = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    auto rangeOf(EdgeId id) const { return ranges.at(id - base); }
};

struct Run {
    SearchMetrics search;
    double dagMs = 0, pruneMs = 0, filterMs = 0, rangesMs = 0, planMs = 0;
    std::uint64_t eligible = 0, retained = 0, arcs = 0, retainedArcs = 0, dagBytes = 0;
};

void stage(const char* name, double ms, bool quiet) {
    if (!quiet) std::cout << "STAGE\t" << name << '\t' << std::fixed
                          << std::setprecision(6) << ms << std::endl;
}

Run execute(const Graph& graph, const Query& q, int method, Deadline& deadline, bool quiet = false, std::uint64_t windowSeed = 0) {
    Run run;
    if (!q.syntacticallyValid() || q.source >= graph.vertexCount() || q.target >= graph.vertexCount())
        throw std::runtime_error("invalid query or vertex mapping");
    TightGraph tight;
    if (method >= 4) {
        auto start = Clock::now();
        if (method == 6) {
            const auto span = graph.globalRange(q.x1, q.x2);
            std::vector<EdgeId> eligible;
            for (auto i = span.first; i < span.second; ++i) {
                if ((i & 4095) == 0) check(deadline);
                if (endpointAdmissible(graph.edges[i], q)) eligible.push_back(static_cast<EdgeId>(i));
            }
            tight = makeTightGraph(graph, q, std::move(eligible));
        } else {
            auto filtered = slidingTightGraph(graph, q, deadline);
            check(deadline);
            run.filterMs = elapsedMs(start);
            tight = makeTightGraph(graph, q, std::move(filtered.edgeIds));
        }
        run.retained = tight.edgeIds.size();
        stage("filter_ms", run.filterMs, quiet);
        check(deadline);
        start = Clock::now();
        auto stats = buildSuccessorStats(tight, deadline);
        check(deadline);
        run.rangesMs = elapsedMs(start);
        run.retainedArcs = stats.transitionCount;
        stage("ranges_ms", run.rangesMs, quiet);
        if (method == 4) run.search = directDfs(tight, stats, deadline);
        else {
            auto window = method == 7 ? randomWindow(tight, windowSeed) : equationWindow(tight, stats, 1);
            run.planMs = window.selectionMs;
            run.search = pivotDfs(tight, stats, window, deadline, 0, 1);
        }
        return run;
    }

    auto start = Clock::now();
    auto span = graph.globalRange(q.x1, q.x2);
    std::vector<EdgeId> eligible;
    for (std::uint64_t i = span.first; i < span.second; ++i) {
        if ((i & 4095) == 0) check(deadline);
        if (endpointAdmissible(graph.edges[i], q)) eligible.push_back(static_cast<EdgeId>(i));
    }
    tight = makeTightGraph(graph, q, std::move(eligible));
    run.eligible = tight.edgeIds.size();
    DagRanges dag;
    dag.base = span.first;
    dag.ranges.resize(span.second - span.first);
    const std::uint64_t originalSize = tight.outIds.size();
    std::uint64_t arcCount = 0;
    for (EdgeId id : tight.edgeIds) {
        check(deadline);
        const auto& e = graph.edges[id];
        const auto range = tight.outRange(e.dst, safeAdd(e.time, q.effectiveMin()), safeAdd(e.time, q.deltaMax));
        dag.ranges[id - dag.base] = range;
        const auto degree = range.second - range.first;
        if (degree > tight.outIds.max_size() - originalSize - arcCount)
            throw std::length_error("DAG exceeds addressable storage");
        arcCount += degree;
    }
    // Exact CSR capacity avoids repeated allocation while materializing transitions.
    tight.outIds.reserve(originalSize + arcCount);
    check(deadline);
    for (EdgeId id : tight.edgeIds) {
        check(deadline);
        const auto range = dag.rangeOf(id);
        auto begin = tight.outIds.size();
        for (auto i = range.first; i < range.second; ++i) {
            if ((i & 4095) == 0) check(deadline);
            // The first segment stays fixed while explicit DAG arcs are appended.
            const EdgeId next = tight.outIds[i];
            tight.outIds.push_back(next);
        }
        dag.ranges[id - dag.base] = {begin, tight.outIds.size()};
    }
    run.arcs = tight.outIds.size() - originalSize;
    run.dagMs = elapsedMs(start);
    stage("dag_build_ms", run.dagMs, quiet);
    run.retained = run.eligible;
    run.retainedArcs = run.arcs;

    if (method != 1) {
        start = Clock::now();
        std::vector<unsigned char> forward(dag.ranges.size()), backward(dag.ranges.size());
        std::uint64_t visited = 0;
        for (EdgeId id : tight.edgeIds) {
            check(deadline);
            if (graph.edges[id].src == q.source) forward[id - dag.base] = 1;
            if (!forward[id - dag.base]) continue;
            const auto r = dag.rangeOf(id);
            for (auto i = r.first; i < r.second; ++i) {
                if ((++visited & 4095) == 0) check(deadline);
                forward[tight.outIds[i] - dag.base] = 1;
            }
        }
        for (auto it = tight.edgeIds.rbegin(); it != tight.edgeIds.rend(); ++it) {
            check(deadline);
            const EdgeId id = *it;
            if (graph.edges[id].dst == q.target) backward[id - dag.base] = 1;
            const auto r = dag.rangeOf(id);
            for (auto i = r.first; !backward[id - dag.base] && i < r.second; ++i) {
                if ((++visited & 4095) == 0) check(deadline);
                backward[id - dag.base] |= backward[tight.outIds[i] - dag.base];
            }
        }
        const auto keep = [&](EdgeId id) { return forward[id - dag.base] && backward[id - dag.base]; };
        // Compact vertex lists without moving the arc segment before it has been read.
        std::uint64_t write = 0;
        for (std::size_t v = 0; v < tight.vertexCount(); ++v) {
            const auto a = tight.outOffsets[v], b = tight.outOffsets[v + 1];
            tight.outOffsets[v] = write;
            for (auto i = a; i < b; ++i) {
                if ((++visited & 4095) == 0) check(deadline);
                if (keep(tight.outIds[i])) tight.outIds[write++] = tight.outIds[i];
            }
        }
        tight.outOffsets.back() = write;
        const auto prefixSize = write;
        for (EdgeId id : tight.edgeIds) {
            check(deadline);
            const auto r = dag.rangeOf(id);
            auto begin = write;
            if (keep(id)) for (auto i = r.first; i < r.second; ++i) {
                if ((++visited & 4095) == 0) check(deadline);
                if (keep(tight.outIds[i])) tight.outIds[write++] = tight.outIds[i];
            }
            dag.ranges[id - dag.base] = {begin, write};
        }
        tight.outIds.resize(write);
        tight.edgeIds.erase(std::remove_if(tight.edgeIds.begin(), tight.edgeIds.end(),
            [&](EdgeId id) { return !keep(id); }), tight.edgeIds.end());
        run.retained = tight.edgeIds.size();
        run.retainedArcs = write - prefixSize;
        run.pruneMs = elapsedMs(start);
        stage("dag_prune_ms", run.pruneMs, quiet);
    }
    run.dagBytes = tight.outIds.capacity() * sizeof(EdgeId) +
        dag.ranges.capacity() * sizeof(decltype(dag.ranges)::value_type);
    check(deadline);
    if (method != 3) run.search = directDfs(tight, dag, deadline);
    else {
        start = Clock::now();
        // Use exactly the same GapBalance statistics and selector as full TWalkEnum.
        WindowChoice window;
        {
            auto support = makeTightGraph(graph, q, tight.edgeIds);
            auto stats = buildSuccessorStats(support, deadline);
            check(deadline);
            run.rangesMs = elapsedMs(start);
            window = equationWindow(support, stats, 1);
            run.planMs = window.selectionMs;
        }
        stage("ranges_ms", run.rangesMs, quiet);
        run.search = pivotDfs(tight, dag, window, deadline, 0, 1);
    }
    return run;
}

std::vector<std::vector<std::uint64_t>> brute(const Graph& g, const Query& q) {
    std::vector<std::vector<std::uint64_t>> answers;
    std::vector<std::uint64_t> path;
    std::function<void(Vertex, Time, bool)> visit = [&](Vertex u, Time previous, bool first) {
        for (const auto& e : g.edges) {
            if (e.src != u || e.time < q.x1 || e.time > q.x2 || !endpointAdmissible(e, q)) continue;
            if (!first && (e.time <= previous || e.time - previous < q.deltaMin || e.time - previous > q.deltaMax)) continue;
            path.push_back(e.originalId);
            if (e.dst == q.target) answers.push_back(path);
            else visit(e.dst, e.time, false);
            path.pop_back();
        }
    };
    visit(q.source, 0, true);
    std::sort(answers.begin(), answers.end());
    return answers;
}

void selfTest() {
    captureMaterializedPathsForTests = true;
    std::mt19937_64 random(20261110);
    std::size_t checks = 0;
    for (int test = 0; test < 160; ++test) {
        Graph g;
        g.externalVertices = {0, 1, 2, 3, 4};
        // Includes revisits, equal timestamps, duplicate events, and exact gap boundaries.
        g.edges = {{0,1,1,0},{1,2,3,1},{2,1,5,2},{1,4,7,3},
                   {0,1,1,4},{1,4,3,5},{0,4,2,6},{4,2,4,7},{2,0,6,8}};
        for (int i = 0; i < 9; ++i)
            g.edges.push_back({static_cast<Vertex>(random()%5), static_cast<Vertex>(random()%5),
                               static_cast<Time>(random()%11), static_cast<std::uint64_t>(9+i)});
        std::stable_sort(g.edges.begin(), g.edges.end(), [](const auto& a, const auto& b) {
            return std::tie(a.time,a.originalId) < std::tie(b.time,b.originalId);
        });
        g.minTime=g.edges.front().time; g.maxTime=g.edges.back().time;
        buildAdjacency(g);
        Query q;
        q.source = test == 0 ? 0 : random()%5;
        do { q.target = random()%5; } while(q.target==q.source);
        q.x1 = random()%5; q.x2 = q.x1 + random()%8;
        q.deltaMin = random()%4; q.deltaMax = q.deltaMin + random()%5;
        if (test == 0) {q.source=0;q.target=4;q.x1=0;q.x2=10;q.deltaMin=2;q.deltaMax=2;}
        const auto expected = brute(g,q);
        for(int method=1;method<=7;++method) {
          for (std::uint64_t seed = 1; seed <= (method == 7 ? 5 : 1); ++seed) {
            Deadline deadline(120);
            auto result=execute(g,q,method,deadline,true,seed);
            auto actual=result.search.result.materializedPaths;
            std::sort(actual.begin(),actual.end());
            if(actual!=expected) throw std::runtime_error("self-test mismatch: case="+std::to_string(test)+" method="+std::to_string(method));
            ++checks;
          }
        }
    }
    std::cout << "PASS " << checks << " complete event-ID multiset comparisons against independent DFS\n";
}

int main(int argc, char** argv) {
    try {
        Args args(argc,argv);
        if(args.flag("self-test")) {selfTest();return 0;}
        if(args.get("prepare")!="") {
            LoadOptions options;
            options.srcColumn=args.i64("src-column",0); options.dstColumn=args.i64("dst-column",1);
            options.timeColumn=args.i64("time-column",2); options.idColumn=args.i64("id-column",-1);
            options.skipHeader=args.flag("skip-header");
            auto delimiter=args.get("delimiter","space");
            options.delimiter=delimiter=="tab"?'\t':delimiter=="comma"?',':' ';
            auto graph=loadTextGraph(args.get("prepare"),options);
            saveGraphCache(graph,args.get("cache"));
            std::cout << "GRAPH\t" << graph.vertexCount() << '\t' << graph.edges.size() << '\n';
            return 0;
        }
        auto graph=loadGraphCache(args.get("cache"));
        auto q=queryFromArgs(args);
        const int method=static_cast<int>(args.i64("method"));
        if(method<1||method>7) throw std::runtime_error("method must be 1..7");
        std::cout << "READY\t" << graph.allocatedBytes() << std::endl;
        std::string go; std::getline(std::cin,go);
        if(go!="GO") return 2;
        const auto start=Clock::now();
        Deadline deadline(120);
        ResponseProbe response; response.target=1000; response.begin=start;
        activeResponseProbe=&response;
        Record record;
        try {
            auto run=execute(graph,q,method,deadline,false,args.u64("window-seed",0));
            record.set("status",(run.search.timedOut||deadline.pollNow())?"TIMEOUT":"OK");
            record.setDouble("online_total_ms",elapsedMs(start));
            record.setDouble("dag_build_ms",run.dagMs);record.setDouble("dag_prune_ms",run.pruneMs);
            record.setDouble("filter_ms",run.filterMs);
            record.setDouble("ranges_ms",run.rangesMs);
            record.setDouble("plan_ms",run.planMs);record.setDouble("prefix_ms",run.search.prefixMs);
            record.setDouble("suffix_ms",run.search.suffixMs);record.setDouble("search_ms",run.search.totalMs);
            record.set("answers",run.search.result.count.str());record.set("event_id_volume",run.search.result.outputVolume.str());
            record.set("search_states",run.search.states);record.set("prefix_states",run.search.prefixStates);
            record.set("suffix_states",run.search.suffixStates);record.set("active_pivots",run.search.activePivots);
            record.set("stored_prefixes",run.search.storedPrefixes);
            if(method<=3) {
                record.set("eligible_events",run.eligible);record.set("dag_arcs",run.arcs);
            }
            record.set("retained_events",run.retained);record.set("retained_arcs",run.retainedArcs);
        } catch(const Expired&) {
            record.set("status","TIMEOUT");record.setDouble("online_total_ms",elapsedMs(start));
        }
        activeResponseProbe=nullptr;
        record.setDouble("response_1000_ms",response.firstTargetMs);
        record.print();
        std::cout << "DONE" << std::endl;
        return 0;
    } catch(const std::bad_alloc&) {std::cerr << "OUT_OF_MEMORY\n";return 3;}
      catch(const std::exception& e) {std::cerr << e.what() << '\n';return 2;}
}
