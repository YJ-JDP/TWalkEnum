#include "index.hpp"
#include "strict_adaptations.hpp"

namespace tpx {

static void addResult(Record& r, const ResultSummary& result) {
    r.set("path_count", result.count.str());
    r.set("output_edge_ids", result.outputVolume.str());
    r.set("answer_min_length", result.count.isZero() ? 0 : result.minLength);
    r.set("answer_max_length", result.maxLength);
}
static void addFilter(Record& r, const FilterMetrics& m) {
    r.setDouble("support_filter_ms", m.totalMs);
    r.setDouble("support_forward_ms", m.forwardMs);
    r.setDouble("support_backward_ms", m.backwardMs);
    r.setDouble("support_intersection_ms", m.intersectMs);
    r.set("filter_scan_forward", m.forwardScanned);
    r.set("filter_scan_backward", m.backwardScanned);
    r.set("filter_attempt_forward", m.forwardAttempts);
    r.set("filter_attempt_backward", m.backwardAttempts);
    r.set("filter_success_forward", m.forwardSuccess);
    r.set("filter_success_backward", m.backwardSuccess);
    r.set("support_events", m.tightEdges);
    r.set("filter_query_memory_bytes", m.peakBytes);
    r.set("query_interval_edges", m.rangeEdges);
}
static void addSearch(Record& r, const SearchMetrics& m) {
    r.setDouble("enumeration_ms", m.totalMs);
    r.setDouble("prefix_search_ms", m.prefixMs);
    r.setDouble("suffix_search_ms", m.suffixMs);
    r.setDouble("preflight_ms", m.preflightMs);
    r.set("enumeration_states", m.states);
    r.set("prefix_states", m.prefixStates);
    r.set("suffix_states", m.suffixStates);
    r.set("preflight_states", m.preflightStates);
    r.set("stored_prefixes", m.storedPrefixes);
    r.set("active_pivots", m.activePivots);
    r.set("enumeration_peak_workspace_bytes", m.searchWorkspacePeakBytes);
    addResult(r,m.result);
}
static void addQueryCommon(Record& r, const Graph& graph, const Query& q, const std::string& method) {
    r.set("action", "run"); r.set("experiment", "index_online");
    r.set("method", method); r.set("workers", 1);
    addGraphRecord(r,graph); addQueryRecord(r,graph,q);
    r.set("hop_bound",temporalHopUpperBound(q));
    r.set("gap_horizon_ratio", static_cast<double>(q.x2-q.x1) / std::max<Time>(1,q.deltaMin));
}

static FilterOutput indexedTspgReachability(const Graph& graph, const Query& q,
                                            const Level3Index& pool, Deadline& deadline) {
    FilterOutput output;
    auto& m=output.metrics;
    const auto beginTime=Clock::now();
    const auto& layout=pool.layout;
    const std::size_t edgeCount=graph.edges.size();
    std::vector<std::uint8_t> forward(edgeCount,0), backward(edgeCount,0);
    std::vector<EdgeId> queue;
    queue.reserve(std::min<std::size_t>(edgeCount, 100000));
    const auto [rb,re]=graph.globalRange(q.x1,q.x2);
    m.rangeBegin=rb;m.rangeEnd=re;m.rangeEdges=re-rb;
    auto inRange=[&](EdgeId id) {
        const auto& e=graph.edges[id];
        return e.time>=q.x1 && e.time<=q.x2 && endpointAdmissible(e,q);
    };
    auto seed=[&](bool isForward) {
        const Vertex v=isForward?q.source:q.target;
        const auto [lo,hi]=graph.adjacencyRange(isForward,v,q.x1,q.x2);
        for(auto pos=lo;pos<hi;++pos){
            const EdgeId id=isForward?graph.outIds[pos]:graph.inIds[pos];
            if(isForward) ++m.forwardScanned;else ++m.backwardScanned;
            if(!inRange(id))continue;
            auto& flags=isForward?forward:backward;
            if(!flags[id]){flags[id]=1;queue.push_back(id);}
        }
    };
    auto scan=[&](bool isForward) {
        queue.clear();
        const auto start=Clock::now();
        seed(isForward);
        auto& marked=isForward?forward:backward;
        std::size_t head=0;
        while(head<queue.size() && !deadline.stopped){
            if((head & 4095U)==0 && deadline.pollNow())break;
            EdgeId e=queue[head++];
            const auto& current=graph.edges[e];
            if((isForward && current.dst==q.target) ||
               (!isForward && current.src==q.source))continue;
            auto rg=isForward?layout.succRanges[e]:layout.predRanges[e];
            const auto& ids=isForward?layout.plusIds:layout.minusIds;
            for(auto j=rangeLeft(rg);j<rangeRight(rg);++j){
                if(isForward)++m.forwardAttempts;else ++m.backwardAttempts;
                EdgeId next=ids[j];
                if(marked[next] || !inRange(next))continue;
                marked[next]=1;queue.push_back(next);
                if(isForward)++m.forwardSuccess;else ++m.backwardSuccess;
                if((j & 8191U)==0 && deadline.pollNow())break;
            }
        }
        if(isForward)m.forwardMs=elapsedMs(start);else m.backwardMs=elapsedMs(start);
        m.queueBytes=std::max<std::uint64_t>(m.queueBytes,queue.capacity()*sizeof(EdgeId));
    };
    scan(true);
    if(!deadline.stopped)scan(false);
    const auto intersectStart=Clock::now();
    if(!deadline.stopped){
        output.edgeIds.reserve(std::min<std::uint64_t>(m.rangeEdges,100000));
        for(auto id=rb;id<re;++id){
            if(forward[id] && backward[id])output.edgeIds.push_back(static_cast<EdgeId>(id));
            if((id&8191ULL)==0 && deadline.pollNow())break;
        }
    }
    m.intersectMs=elapsedMs(intersectStart);
    m.tightEdges=output.edgeIds.size();
    m.outputBytes=output.edgeIds.capacity()*sizeof(EdgeId);
    m.workingBytes=forward.capacity()+backward.capacity();
    m.peakBytes=m.workingBytes+m.queueBytes+m.outputBytes;
    m.timedOut=deadline.stopped;
    m.totalMs=elapsedMs(beginTime);
    return output;
}

static Record runIndexedQuery(const Graph& graph, const Query& q, const Args& args) {
    const std::string method=args.get("algorithm");
    const bool indexed=method!="TWalkEnum";
    const bool pathEnum=method=="Indexed-PathEnum-DFS-style";
    const bool cpe=method=="Indexed-CPE-startup-style";
    const bool tspg=method=="Indexed-TSPG-style";
    const bool twalkIdx=method=="PoolIndex+TWalkEnum";
    if (!(method=="TWalkEnum"||pathEnum||cpe||tspg||twalkIdx))
        throw std::runtime_error("unknown indexed algorithm: " + method);
    if (q.deltaMin < 1 || q.deltaMax<q.deltaMin || q.x2-q.x1>10*q.deltaMin || temporalHopUpperBound(q)<3 || temporalHopUpperBound(q)>11)
        throw std::runtime_error("query violates fixed gap or 3<=H_q<=11 requirements");
    Level3Index pool;
    double poolLoad=0, topologyLoad=0;
    if (indexed) {
        auto st=Clock::now(); pool=loadLevel3(args.get("index-file")); poolLoad=elapsedMs(st);
        requireIndexMatch(pool.layout.deltaMin,pool.layout.deltaMax,pool.layout.graphHash,
                          pool.layout.edgeCount,pool.layout.vertexCount,graph,q);
    }
    std::optional<TopologyIndex> topology;
    if (pathEnum||cpe) {
        auto st=Clock::now(); topology=loadTopologyIndex(args.get("topology-index-file"));
        requireTopologyIndexMatch(*topology,graph); topologyLoad=elapsedMs(st);
    }
    Record r; addQueryCommon(r,graph,q,method);
    r.set("fixed_index_bytes",indexed?pool.metrics.bytes:0);
    r.set("topology_index_bytes",topology?topologyIndexBytes(*topology):0);
    r.setDouble("index_load_ms_excluded",poolLoad);
    r.setDouble("topology_load_ms_excluded",topologyLoad);
    const auto start=Clock::now(); responseClockBegin();
    Deadline deadline(args.real("soft-timeout",120.0));
    FilterOutput filter = tspg ? indexedTspgReachability(graph,q,pool,deadline)
                       : indexed ? level3Filter(graph,q,pool,deadline)
                                 : slidingTightGraph(graph,q,deadline);
    addFilter(r,filter.metrics);
    bool timedOut=deadline.stopped||filter.metrics.timedOut;
    std::uint64_t queryPeak=filter.metrics.peakBytes;
    if (!timedOut && (pathEnum||cpe)) {
        if (pathEnum) {
            const auto m=runPathEnumStrict(graph,*topology,q,deadline,11,&filter.edgeIds);
            timedOut=m.timedOut||deadline.stopped||m.unsupportedHop;
            r.setDouble("query_plan_ms",m.topologyQueryIndexMs);
            r.setDouble("temporal_candidate_ms",m.temporalSubgraphMs);
            r.setDouble("enumeration_ms",m.enumerationMs);
            r.set("topology_bfs_scans",m.topologyBfsEdgeScans);
            r.set("static_neighbor_checks",m.staticNeighborChecks);
            r.set("temporal_range_queries",m.temporalRangeQueries);
            r.set("temporal_event_visits",m.temporalEventVisits);
            r.set("temporal_candidate_events",m.temporalSubgraphEvents);
            r.set("enumeration_states",m.dfsStates);
            r.set("gap_accepted_transitions",m.gapAcceptedTransitions);
            r.set("candidate_query_memory_bytes",m.queryPeakBytes);
            queryPeak=std::max(queryPeak,m.queryPeakBytes);
            addResult(r,m.result);
        } else {
            const auto m=runCpeStrict(graph,*topology,q,deadline,11,std::uint64_t{0},&filter.edgeIds);
            timedOut=m.timedOut||deadline.stopped||m.unsupportedHop||m.resourceCap;
            r.set("resource_cap",m.resourceCap?1:0);
            r.setDouble("query_plan_ms",m.staticSubgraphMs);
            r.setDouble("temporal_candidate_ms",m.temporalSubgraphMs);
            r.setDouble("partial_construction_ms",m.partialIndexMs);
            r.setDouble("join_ms",m.joinMs);
            r.set("topology_bfs_scans",m.topologyBfsEdgeScans);
            r.set("static_neighbor_checks",m.staticNeighborChecks);
            r.set("temporal_range_queries",m.temporalRangeQueries);
            r.set("temporal_event_visits",m.temporalEventVisits);
            r.set("partial_paths",m.partialPaths);
            r.set("enumeration_states",m.partialPaths + m.joinChecks);
            r.setDouble("enumeration_ms",m.partialIndexMs + m.joinMs);
            r.set("join_checks",m.joinChecks);
            r.set("join_gap_pass",m.joinGapPass);
            r.set("candidate_query_memory_bytes",m.queryPeakBytes);
            queryPeak=std::max(queryPeak,m.queryPeakBytes);
            addResult(r,m.result);
        }
    } else if (!timedOut) {
        const auto startMat=Clock::now();
        TightGraph tight=makeTightGraph(graph,q,std::move(filter.edgeIds));
        r.setDouble("support_materialize_ms",elapsedMs(startMat));
        r.set("support_graph_bytes",tight.bytes);
        SuccessorStats succ=buildSuccessorStats(tight,deadline);
        r.setDouble("successor_ranges_ms",succ.buildMs);
        r.set("successor_transitions",succ.transitionCount);
        r.set("successor_memory_bytes",succ.bytes);
        if (!deadline.stopped) {
            if (tspg) {

                SearchMetrics m=parallelDirectDfs(tight,succ,deadline,1);
                addSearch(r,m); timedOut=m.timedOut||deadline.stopped;
                queryPeak=std::max(queryPeak,tight.bytes+succ.bytes+m.searchWorkspacePeakBytes);
            } else {
                WindowChoice window=equationWindow(tight,succ,1);
                r.setDouble("gapbalance_ms",window.selectionMs);
                r.set("window_left",window.left); r.set("window_right",window.right);
                SearchMetrics m=pivotDfs(tight,succ,window,deadline,
                                          std::uint64_t{0},1);
                addSearch(r,m); timedOut=m.timedOut||deadline.stopped;
                queryPeak=std::max(queryPeak,tight.bytes+succ.bytes+m.searchWorkspacePeakBytes);
            }
        } else timedOut=true;
    }
    responseClockEnd();
    const double online=elapsedMs(start);
    r.setDouble("online_total_ms",online);
    r.setDouble("query_time_ms",online);
    r.setDouble("response_time_ms",responseFirstAnswerMs>=0?responseFirstAnswerMs:online);
    r.set("first_answer_found",responseFirstAnswerMs>=0?1:0);
    r.set("query_local_peak_bytes_estimate",queryPeak);
    r.set("peak_rss_bytes",peakRssBytes());
    r.set("status",timedOut?(r.values["resource_cap"]=="1"?"RESOURCE_CAP":"TIMEOUT_SOFT"):"OK");
    return r;
}

static void printQueries(const Graph& graph,const Args& args) {
    const auto dm=args.i64("delta-min"),dx=args.i64("delta-max");
    if(dm<=0 || dx<dm || dx>10*dm)throw std::runtime_error("invalid fixed gap; require 0<dmin<=dmax<=10*dmin");
    const std::uint64_t seed=args.u64("seed",2026101108), wanted=args.u64("query-count",200);
    std::mt19937_64 rng(seed);
    std::set<std::tuple<Vertex,Vertex,Time,Time>> seen;
    const Time available=graph.maxTime-graph.minTime;
    if(available<dx) throw std::runtime_error("graph shorter than fixed gap; adjust ratio");
    const Time maxSpan=std::min<Time>(available,dm*10);

    const Time minSpan=std::max<Time>(dx,2*dm);
    if(minSpan>maxSpan)throw std::runtime_error("No feasible observation span for 3<=H_q<=11");
    std::uniform_int_distribution<Time> widthDist(minSpan,maxSpan);
    std::bernoulli_distribution twoEventAnchor(0.5);
    std::uniform_int_distribution<std::uint64_t> edgeDist(0,graph.edges.size()-1);

    std::vector<EdgeId> twoEventSeeds;
    std::optional<Level3Index> generatorIndex;
    if (!args.get("index-file").empty()) {
        generatorIndex=loadLevel3(args.get("index-file"));
        if(generatorIndex->layout.deltaMin!=dm || generatorIndex->layout.deltaMax!=dx ||
           generatorIndex->layout.graphHash!=graphFingerprint(graph))
            throw std::runtime_error("query-generator index does not match graph or gap");
        twoEventSeeds.reserve(generatorIndex->layout.plusIds.size());
        for(EdgeId id=0;id<graph.edges.size();++id){
            const auto range=generatorIndex->layout.succRanges[id];
            if(rangeLeft(range)<rangeRight(range) && graph.edges[id].src!=graph.edges[id].dst)
                twoEventSeeds.push_back(id);
        }
    }
    std::cout << "TWALK_QUERIES_V1\n";
    std::cout << "query_id\tquery_seed\tsource_internal_id\ttarget_internal_id\tsource_external_id\ttarget_external_id\tx1\tx2\tdelta_min\tdelta_max\tH_q\tanchor_type\twitness_edge_1\twitness_edge_2\twitness_length\twitness_time_1\twitness_time_2\n";
    std::uint64_t made=0;
    for(std::uint64_t attempt=0;attempt<10000000 && made<wanted;++attempt){
        const bool wantTwo=twoEventAnchor(rng) && !twoEventSeeds.empty();
        const EdgeId firstId=wantTwo?twoEventSeeds[std::uniform_int_distribution<std::size_t>(0,twoEventSeeds.size()-1)(rng)]
                                   :static_cast<EdgeId>(edgeDist(rng));
        const auto& first=graph.edges[firstId];
        if(first.src==first.dst)continue;
        Vertex s=first.src,t=first.dst;
        Time earliest=first.time,latest=first.time;
        std::string anchor="single";
        EdgeId secondId=std::numeric_limits<EdgeId>::max();
        if(wantTwo){
            auto range=generatorIndex->layout.succRanges[firstId];
            const auto a=rangeLeft(range),b=rangeRight(range);
            std::uniform_int_distribution<std::uint32_t> secondDist(a,b-1);
            for(int j=0;j<24;++j){
                const EdgeId candidateId=generatorIndex->layout.plusIds[secondDist(rng)];
                const auto& f=graph.edges[candidateId];
                if(f.dst!=s && f.dst!=first.dst){
                    t=f.dst;latest=f.time;secondId=candidateId;anchor="two-event";break;
                }
            }
            if(anchor!="two-event")continue;
        }
        if(s==t)continue;

        const Time span=widthDist(rng);
        if(latest-earliest>span)continue;
        const Time lower=std::max(graph.minTime,latest-span);
        const Time upper=std::min(earliest,graph.maxTime-span);
        if(lower>upper)continue;
        std::uniform_int_distribution<Time> startDist(lower,upper);
        const Time x1=startDist(rng),x2=x1+span;
        if(!seen.emplace(s,t,x1,x2).second)continue;
        Query q; q.source=s;q.target=t;q.x1=x1;q.x2=x2;q.deltaMin=dm;q.deltaMax=dx;
        if(!q.syntacticallyValid()||temporalHopUpperBound(q)<3||temporalHopUpperBound(q)>11)
            throw std::runtime_error("generated query violates 3<=H_q<=11");

        if(!(q.x1<=earliest && latest<=q.x2 && first.src==s &&
             (anchor=="single" ? (first.dst==t) :
              (secondId!=std::numeric_limits<EdgeId>::max() &&
               graph.edges[secondId].src==first.dst && graph.edges[secondId].dst==t &&
               graph.edges[secondId].time-first.time>=dm &&
               graph.edges[secondId].time-first.time<=dx && first.dst!=s && first.dst!=t))))
            throw std::runtime_error("generated query witness is not a legal answer");
        std::cout << made << '\t' << splitmix64(seed ^ (made * 0x9e3779b97f4a7c15ULL) ^ (attempt * 0xbf58476d1ce4e5b9ULL)) << '\t' << s << '\t' << t << '\t'
            <<graph.externalVertices[s]<<'\t'<<graph.externalVertices[t]<<'\t'<<x1<<'\t'<<x2<<'\t'
            <<dm<<'\t'<<dx<<'\t'<<temporalHopUpperBound(q)<<'\t'<<anchor<<'\t'
            <<firstId<<'\t'<<(anchor=="single"?std::string(""):std::to_string(secondId))<<'\t'
            <<(anchor=="single"?1:2)<<'\t'<<first.time<<'\t'
            <<(anchor=="single"?std::string(""):std::to_string(latest))<<'\n';
        ++made;
    }
    if(made<wanted)throw std::runtime_error("insufficient unique queries generated; review gap parameters");
}

static Record verifyQueries(const Graph& graph, const Args& args) {
    std::ifstream input(args.get("queries-file"));
    if(!input)throw std::runtime_error("cannot open queries-file");
    std::string header;std::getline(input,header);
    std::vector<std::string> fields;
    {std::istringstream h(header);std::string token;while(std::getline(h,token,'\t'))fields.push_back(token);}
    std::unordered_map<std::string,std::size_t> pos;
    for(std::size_t i=0;i<fields.size();++i)pos[fields[i]]=i;
    for(const char* k:{"source_internal_id","target_internal_id","x1","x2","delta_min","delta_max","H_q","witness_edge_1","witness_edge_2","witness_length"})
        if(!pos.count(k))throw std::runtime_error(std::string("missing query field: ")+k);
    std::size_t checked=0;std::uint32_t minHop=100,maxHop=0;std::string line;
    while(std::getline(input,line)){
        if(line.empty())continue;
        std::vector<std::string> col;{std::istringstream h(line);std::string v;while(std::getline(h,v,'\t'))col.push_back(v);}

        auto at=[&](std::string k)->std::string {return pos[k]<col.size()?col[pos[k]]:std::string("");};
        auto num=[&](std::string k)->std::int64_t{return std::stoll(at(k));};
        Query q;q.source=static_cast<Vertex>(num("source_internal_id"));q.target=static_cast<Vertex>(num("target_internal_id"));
        q.x1=num("x1");q.x2=num("x2");q.deltaMin=num("delta_min");q.deltaMax=num("delta_max");
        auto firstId=static_cast<std::uint64_t>(num("witness_edge_1"));
        int length=static_cast<int>(num("witness_length"));
        if(!q.syntacticallyValid() || q.source>=graph.vertexCount() || q.target>=graph.vertexCount() ||
           firstId>=graph.edges.size() || q.deltaMin<1 || q.deltaMax<q.deltaMin ||
           q.x2-q.x1>10*q.deltaMin || temporalHopUpperBound(q)<3 || temporalHopUpperBound(q)>11 ||
           temporalHopUpperBound(q)!=num("H_q") || (length!=1&&length!=2))
            throw std::runtime_error("query invalid at row "+std::to_string(checked));
        const auto& e=graph.edges[firstId];
        if(e.src!=q.source||e.time<q.x1||e.time>q.x2||!endpointAdmissible(e,q))
            throw std::runtime_error("first witness invalid at row "+std::to_string(checked));
        if(length==1){
            if(e.dst!=q.target)throw std::runtime_error("single witness does not reach target");
        }else{
            const auto secondId=static_cast<std::uint64_t>(num("witness_edge_2"));
            if(secondId>=graph.edges.size())throw std::runtime_error("second witness ID out of range");
            const auto& f=graph.edges[secondId];
            if(e.dst!=f.src||f.dst!=q.target||!endpointAdmissible(f,q)||
               f.time<q.x1||f.time>q.x2||
               f.time-e.time<q.deltaMin||f.time-e.time>q.deltaMax)
                throw std::runtime_error("two-event witness invalid at row "+std::to_string(checked));
        }
        minHop=std::min(minHop,temporalHopUpperBound(q));maxHop=std::max(maxHop,temporalHopUpperBound(q));
        ++checked;
    }
    if(checked!=args.u64("query-count",200))throw std::runtime_error("query count mismatch in certificate verification");
    Record r;r.set("status","OK");r.set("action","verify-queries");r.set("verified_nonempty_queries",checked);
    r.set("min_H_q",minHop);r.set("max_H_q",maxHop);return r;
}

int main08(int argc,char**argv){
    try {
        Args args(argc,argv);const auto action=args.get("action","run");
        if(action=="cache-graph"){
            LoadOptions options;options.delimiter=args.get("delimiter","space")=="comma"?',':' ';
            options.srcColumn=static_cast<int>(args.i64("src-column",0));
            options.dstColumn=static_cast<int>(args.i64("dst-column",1));
            options.timeColumn=static_cast<int>(args.i64("time-column",2));
            options.idColumn=static_cast<int>(args.i64("id-column",-1));
            options.skipHeader=args.flag("skip-header");
            Graph g=loadTextGraph(args.get("graph-file"),options);
            saveGraphCache(g,args.get("graph-cache"));
            Record r;r.set("status","OK");r.set("action",action);addGraphRecord(r,g);r.print();return 0;
        }
        Graph graph=loadGraphCache(args.get("graph-cache"));
        if(action=="build-index"){
            auto st=Clock::now();Deadline d(args.real("index-build-timeout",21600));
            auto index=buildLevel3(graph,args.i64("delta-min"),args.i64("delta-max"),d);
            if(d.stopped||index.metrics.timedOut)throw std::runtime_error("PoolIndex construction timed out");
            saveLevel3(index,args.get("index-file"));
            Record r;r.set("status","OK");r.set("action",action);r.setDouble("index_build_ms",elapsedMs(st));
            r.setDouble("index_algorithm_build_ms",index.metrics.buildMs);
            r.set("index_bytes",index.metrics.bytes);
            r.set("index_serialized_bytes",std::filesystem::file_size(args.get("index-file")));
            r.set("index_successor_refs",index.metrics.successorReferences);
            r.set("index_predecessor_refs",index.metrics.predecessorReferences);
            r.set("index_annotated_events",index.metrics.annotatedEdges);
            r.set("peak_rss_bytes",peakRssBytes());addGraphRecord(r,graph);r.print();return 0;
        }
        if(action=="build-topology-index"){
            auto st=Clock::now();TopologyIndex ti(graph);
            saveTopologyIndex(ti,args.get("topology-index-file"));
            Record r;r.set("status","OK");r.set("action",action);
            r.setDouble("topology_build_ms",elapsedMs(st));r.set("topology_index_bytes",topologyIndexBytes(ti));
            r.set("topology_serialized_bytes",std::filesystem::file_size(args.get("topology-index-file")));
            r.set("peak_rss_bytes",peakRssBytes());addGraphRecord(r,graph);r.print();return 0;
        }
        if(action=="generate-queries") {printQueries(graph,args);return 0;}
        if(action=="verify-queries") {auto r=verifyQueries(graph,args);r.print();return 0;}
        if(action=="run") {Query q=queryFromArgs(args);auto r=runIndexedQuery(graph,q,args);r.print();return 0;}
        throw std::runtime_error("unknown action: "+action);
    }catch(const std::bad_alloc&){Record r;r.set("status","OOM");r.set("error","std::bad_alloc");r.print();return 3;}
     catch(const std::exception& e){Record r;r.set("status","ERROR");r.set("error",e.what());r.print();return 2;}
}
}
int main(int argc,char** argv){return tpx::main08(argc,argv);}
