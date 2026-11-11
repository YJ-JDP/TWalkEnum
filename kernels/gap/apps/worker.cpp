#include "strict_adaptations.hpp"
#include "correctness_adaptations.hpp"
#include "index.hpp"
#include "search_experiments.hpp"
#include <memory>

using namespace tpx;
using Paths = std::vector<std::vector<std::uint64_t>>;

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> fields; std::stringstream in(s); std::string v;
    while (std::getline(in, v, '\t')) fields.push_back(v);
    return fields;
}
static void writePaths(const Paths& paths, const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file); if (!out) throw std::runtime_error("cannot write sequence file");
    out << "ordinal\tevent_ids\n";
    for (std::size_t i=0;i<paths.size();++i) {
        out << i << '\t';
        for (std::size_t j=0;j<paths[i].size();++j) { if(j)out<<',';out<<paths[i][j]; }
        out << '\n';
    }
    if(!out) throw std::runtime_error("sequence file write failed");
}

static void brute(const Graph& g,const Query& q,Deadline& deadline,
                  Vertex v,Time last,std::vector<std::uint64_t>& path,Paths& paths,
                  std::uint64_t& steps,std::uint64_t limit) {
    const auto interval=g.globalRange(q.x1,q.x2);
    for (std::uint64_t id=interval.first;id<interval.second;++id) {
        if((++steps & 1023)==0 && deadline.pollNow())return;
        const auto& e=g.edges[id];
        if(e.src!=v || e.src==q.target || e.dst==q.source)continue;
        if(!path.empty() && (e.time<=last || e.time-last<q.deltaMin || e.time-last>q.deltaMax))continue;
        path.push_back(e.originalId);
        if(e.dst==q.target) {

            paths.push_back(path);
        } else brute(g,q,deadline,e.dst,e.time,path,paths,steps,limit);
        path.pop_back(); if(deadline.stopped)return;
    }
}

struct Session {
    Graph graph;
    std::unique_ptr<TopologyIndex> topology;
    std::unique_ptr<Level1Index> l1;
    std::unique_ptr<Level2Index> l2;
    std::unique_ptr<Level3Index> pool;
    std::string loadedType;

    void clearIndex(){ l1.reset();l2.reset();pool.reset();loadedType.clear(); }
    Query query(const Args& a) const {
        auto q=queryFromArgs(a);
        if(!q.syntacticallyValid() || q.deltaMin<1 || q.source>=graph.vertexCount() || q.target>=graph.vertexCount())
            throw std::runtime_error("invalid query or endpoint outside graph");
        return q;
    }
    Record build(const Args& a) {
        clearIndex();
        const auto type=a.get("index-type"); const auto file=a.get("index-file");
        Deadline deadline(a.real("soft-timeout",21600));
        const auto dmin=a.i64("delta-min"),dmax=a.i64("delta-max");
        const auto maxBytes=std::uint64_t{0};
        IndexMetrics metrics;const auto buildStart=Clock::now();
        if(type=="PoolIndex") {pool=std::make_unique<Level3Index>(buildLevel3(graph,dmin,dmax,deadline));metrics=pool->metrics;}
        else if(type=="L1") {l1=std::make_unique<Level1Index>(buildLevel1(graph,dmin,dmax,deadline,maxBytes));metrics=l1->metrics;}
        else if(type=="L2") {l2=std::make_unique<Level2Index>(buildLevel2(graph,dmin,dmax,deadline,maxBytes));metrics=l2->metrics;}
        else throw std::runtime_error("unknown index type");
        const bool memory=metrics.memoryLimit;
        const bool timeout=deadline.stopped || metrics.timedOut;
        Record r;r.set("status",timeout?"TIMEOUT_BUILD":memory?"MEMORY_LIMIT":"OK");
        r.setDouble("build_ms",elapsedMs(buildStart));r.setDouble("library_build_ms",metrics.buildMs);
        if(!timeout && !memory) {
            r.set("space_bytes",metrics.bytes);
            const auto start=Clock::now();
            if(pool)saveLevel3(*pool,file);else if(l1)saveLevel1(*l1,file);else saveLevel2(*l2,file);
            r.setDouble("serialization_ms_excluded",elapsedMs(start));
            r.set("serialized_bytes",std::filesystem::file_size(file));
        }
        r.set("peak_rss_including_graph",peakRssBytes());clearIndex();return r;
    }
    Record load(const Args& a) {
        clearIndex();const auto type=a.get("index-type");const auto file=a.get("index-file");
        if(type=="PoolIndex")pool=std::make_unique<Level3Index>(loadLevel3(file));
        else if(type=="L1")l1=std::make_unique<Level1Index>(loadLevel1(file));
        else if(type=="L2")l2=std::make_unique<Level2Index>(loadLevel2(file));
        else throw std::runtime_error("unknown index type");
        loadedType=type;Record r;r.set("status","OK");return r;
    }
    Record onlineFilter(const Args& a) {
        const auto q=query(a);Deadline deadline(a.real("soft-timeout",120));
        const auto start=Clock::now();
        auto f=slidingTightGraph(graph,q,deadline);
        const double ms=elapsedMs(start);
        const bool complete=!f.metrics.timedOut && !deadline.stopped;
        Record r;r.setDouble("filter_ms",ms);
        r.set("status",complete?"OK":"TIMEOUT");r.set("measurement_scope","online_filter_only");
        if(complete) {
            r.set("path_graph_events",f.edgeIds.size());
            r.set("support_checksum",tightEdgeFingerprint(graph,f.edgeIds));
        }
        return r;
    }
    Record temporalReach(const Args& a) {
        const Vertex source=static_cast<Vertex>(a.u64("source"));
        const Vertex target=static_cast<Vertex>(a.u64("target"));
        const Time x1=a.i64("x1"),x2=a.i64("x2");
        if(source>=graph.vertexCount() || target>=graph.vertexCount() || source==target || x1>=x2)
            throw std::runtime_error("invalid reachability candidate");
        std::vector<Time> dist(graph.vertexCount(),std::numeric_limits<Time>::max());
        std::vector<unsigned char> reached(graph.vertexCount(),0);
        const auto range=graph.globalRange(x1,x2);

        for(EdgeId id=range.first;id<range.second;++id) {
            const auto& e=graph.edges[id];
            if((e.src==source || (reached[e.src] && dist[e.src]<e.time)) &&
               (!reached[e.dst] || e.time<dist[e.dst])) {
                reached[e.dst]=1;dist[e.dst]=e.time;
            }
        }
        Record r;r.set("status","OK");r.set("reachable",reached[target]?1:0);
        r.set("e_q",range.second-range.first);return r;
    }
    Record sweep(const Args& a) {
        const auto q=query(a);const auto start=Clock::now();Deadline deadline(a.real("soft-timeout",120));
        ResponseProbe probe;probe.target=1000;probe.begin=start;
        activeResponseProbe=&probe;
        TWalkMetrics m;
        try{m=runTWalkEnum(graph,q,deadline,1,std::numeric_limits<std::uint64_t>::max());}
        catch(...){activeResponseProbe=nullptr;throw;}
        activeResponseProbe=nullptr;
        const double callWall=elapsedMs(start);const double online=m.totalMs;m.search.result.finishEmpty();
        const bool timeout=m.timedOut || deadline.pollNow();
        Record r;r.set("status",timeout?"TIMEOUT":"OK");r.setDouble("online_total_ms",online);
        r.set("answers",m.search.result.count.str());r.set("nonempty",m.search.result.count.isZero()?0:1);
        r.set("e_q",m.filter.rangeEdges);
        if(!m.filter.timedOut)r.set("e_r",m.filter.tightEdges);
        r.set("h_q",temporalHopUpperBound(q));r.setDouble("prefix_ms",m.search.prefixMs);
        r.setDouble("suffix_ms",m.search.suffixMs);r.setDouble("enumeration_ms",m.search.totalMs);
        r.set("prefix_states",m.search.prefixStates);r.set("suffix_states",m.search.suffixStates);
        r.set("active_pivots",m.search.activePivots);r.set("peak_bytes",m.queryPeakBytes);
        if(probe.reached)r.setDouble("response_1000_ms",probe.firstTargetMs);
        r.set("filter_complete",!m.filter.timedOut?1:0);r.set("prefix_storage_saturated",m.search.prefixStorageSaturated?1:0);
        r.setDouble("call_wall_ms_including_final_cleanup",callWall);
        return r;
    }
    Record indexQuery(const Args& a) {
        if(loadedType!=a.get("index-type"))throw std::runtime_error("requested index not loaded");
        const auto q=query(a);Deadline deadline(a.real("soft-timeout",120));
        const auto start=Clock::now();FilterOutput f;
        if(pool)f=level3Filter(graph,q,*pool,deadline);else if(l1)f=level1Filter(graph,q,*l1,deadline);else f=level2Filter(graph,q,*l2,deadline);
        const double filterTime=elapsedMs(start);const bool filterComplete=!f.metrics.timedOut && !deadline.stopped;
        Record r;r.setDouble("filter_ms",filterTime);

        r.set("measurement_scope","filter_only");
        r.set("status",filterComplete?"OK":"TIMEOUT");
        r.set("filter_complete",filterComplete?1:0);
        r.set("peak_bytes",f.metrics.peakBytes);
        if(filterComplete) {
            r.set("path_graph_events",f.edgeIds.size());

            r.set("support_checksum",tightEdgeFingerprint(graph,f.edgeIds));
        }
        return r;
    }
    Record ablation(const Args& a) {
        const auto q=query(a);SearchOptions opts;
        const auto variant=a.get("variant","full"),window=a.get("window-mode","gb");
        if(variant=="full")opts.variant=SearchVariant::Full;
        else if(variant=="no_chrono")opts.variant=SearchVariant::NoChrono;
        else if(variant=="no_pivot")opts.variant=SearchVariant::NoPivot;
        else throw std::runtime_error("unknown ablation variant");
        if(window=="gb")opts.windowMode=WindowMode::GapBalance;
        else if(window=="random")opts.windowMode=WindowMode::Random;
        else throw std::runtime_error("unknown ablation window");
        opts.windowSeed=a.u64("window-seed",0);opts.workers=1;opts.prefixBudget=0;
        opts.softTimeoutS=a.real("soft-timeout",120);
        auto m=runSearch(graph,q,opts);auto& tw=m.tw;auto& s=tw.search;s.result.finishEmpty();
        Record r;r.set("status",m.timedOut?"TIMEOUT":s.result.count.isZero()?"EMPTY":"OK");
        r.setDouble("online_total_ms",m.totalMs);r.setDouble("filter_ms",tw.filter.totalMs);
        r.setDouble("tight_ms",tw.tightMaterializeMs);r.setDouble("stats_ms",tw.successor.buildMs);
        r.setDouble("window_ms",tw.window.selectionMs);r.setDouble("prefix_ms",s.prefixMs);
        r.setDouble("suffix_ms",s.suffixMs);r.setDouble("enum_ms",s.totalMs);
        r.set("search_states",s.states);r.set("prefix_states",s.prefixStates);r.set("suffix_states",s.suffixStates);
        r.set("active_pivots",s.activePivots);r.set("stored_prefix_count",s.storedPrefixes);
        r.set("path_count",s.result.count.str());r.set("output_edge_ids",s.result.outputVolume.str());
        r.set("min_path_length",s.result.minLength);r.set("max_path_length",s.result.maxLength);
        r.set("eligible_events",m.eligibleEvents);r.set("filter_events",m.filterEvents);
        r.set("window_left",tw.window.left);r.set("window_right",tw.window.right);
        r.set("timed_out",m.timedOut?1:0);r.set("fallback_direct",(m.directSearch||s.fallbackToDirect)?1:0);
        r.set("prefix_storage_saturated",s.prefixStorageSaturated?1:0);r.set("peak_bytes",s.searchWorkspacePeakBytes);
        return r;
    }
    Record correct(const Args& a) {
        const auto q=query(a);const auto dir=std::filesystem::path(a.get("sequence-dir"));
        if(!topology)topology=std::make_unique<TopologyIndex>(graph);
        const double seconds=a.real("soft-timeout",30);
        const auto limit=std::uint64_t{0};
        std::map<std::string,Paths> all;std::map<std::string,double> times;std::map<std::string,std::string> reported;
        bool timeout=false;captureMaterializedPathsForTests=true;
        try {
            for(const auto& name:std::vector<std::string>{"dfs","twalk","pathenum","cpe","tspg"}) {
                const auto start=Clock::now();Deadline d(seconds);Paths paths;
                if(name=="dfs") {std::vector<std::uint64_t> p;std::uint64_t steps=0;brute(graph,q,d,q.source,0,p,paths,steps,limit);reported[name]=std::to_string(paths.size());}
                else if(name=="twalk") {auto m=runTWalkEnum(graph,q,d,1,std::numeric_limits<std::uint64_t>::max());timeout|=m.timedOut;m.search.result.finishEmpty();reported[name]=m.search.result.count.str();paths=std::move(m.search.result.materializedPaths);}
                else if(name=="pathenum") {auto m=runPathEnumCaptured(graph,*topology,q,d);timeout|=m.timedOut||m.unsupportedHop;m.result.finishEmpty();reported[name]=m.result.count.str();paths=std::move(m.result.materializedPaths);}
                else if(name=="cpe") {auto m=runCpeCaptured(graph,*topology,q,d,256,0);timeout|=m.timedOut||m.unsupportedHop||m.resourceCap;m.result.finishEmpty();reported[name]=m.result.count.str();paths=std::move(m.result.materializedPaths);}
                else {auto m=runTspgCaptured(graph,q,d);timeout|=m.timedOut;m.result.finishEmpty();reported[name]=m.result.count.str();paths=std::move(m.result.materializedPaths);}
                times[name]=elapsedMs(start);timeout|=d.pollNow();
                std::sort(paths.begin(),paths.end());
                if(!dir.empty())writePaths(paths,dir/(name+".tsv"));
                all[name]=std::move(paths);if(timeout)break;
            }
        } catch(...) {captureMaterializedPathsForTests=false;throw;}
        captureMaterializedPathsForTests=false;
        bool match=!timeout && all.size()==5;
        for(const auto& kv:all)if(kv.second!=all["dfs"] || reported[kv.first]!=std::to_string(kv.second.size()))match=false;
        Record r;
        for(const auto& kv:all){r.set("answers_"+kv.first,reported[kv.first]);r.setDouble("ms_"+kv.first,times[kv.first]);}
        r.set("multiset_match",timeout?"":match?"PASS":"MISMATCH");r.set("sequences_compared",all["dfs"].size());
        r.set("status",timeout?"TIMEOUT":match?"PASS":"MISMATCH");
        if(!match && !timeout && !dir.empty()) {
            std::ofstream out(dir/"mismatch.txt");std::size_t shown=0;
            std::map<std::vector<std::uint64_t>,std::size_t> reference;
            for(const auto& p:all["dfs"])++reference[p];
            for(const auto& kv:all)if(kv.first!="dfs") {
                auto counts=reference;for(const auto& p:kv.second)counts.emplace(p,0);
                std::map<std::vector<std::uint64_t>,std::size_t> actual;for(const auto& p:kv.second)++actual[p];
                for(const auto& entry:counts) {
                    const auto n=actual[entry.first];if(n==entry.second)continue;
                    if(shown++>=50)break;
                    out<<kv.first<<"\texpected="<<entry.second<<"\tactual="<<n<<"\t";
                    for(auto id:entry.first) {out<<id<<',';}
                    out<<'\n';
                }
            }
        }
        return r;
    }
    Record sample(const Args& a) {
        std::mt19937_64 rng(a.u64("query-seed"));
        auto pick=[&](std::uint64_t n){return std::uniform_int_distribution<std::uint64_t>(0,n-1)(rng);};
        const Time span=std::max<Time>(3,static_cast<Time>(std::ceil(graph.timeSpan()*a.real("short-fraction",0.005))));
        if(graph.edges.empty())throw std::runtime_error("empty graph");
        for(int attempt=0;attempt<20000;++attempt) {
            const auto& e=graph.edges[pick(graph.edges.size())];
            if(e.src==e.dst)continue;
            Query q;q.x1=std::max(graph.minTime,std::min(e.time-span/5,graph.maxTime-span));q.x2=q.x1+span;
            q.deltaMin=(span+4)/5;q.deltaMax=(2*span+4)/5;
            const auto range=graph.adjacencyRange(graph.outIds,graph.outOffsets,e.dst,safeAdd(e.time,q.deltaMin),safeAdd(e.time,q.deltaMax));
            if(range.first==range.second)continue;
            const auto& f=graph.edges[graph.outIds[range.first+pick(range.second-range.first)]];
            if(f.time>q.x2 || f.dst==e.src || f.dst==e.dst)continue;
            if(graph.globalRange(q.x1,q.x2).second-graph.globalRange(q.x1,q.x2).first>a.u64("max-events",500))continue;
            q.source=e.src;q.target=f.dst;
            Record r;r.set("status","OK");r.set("src",q.source);r.set("dst",q.target);r.set("x1",q.x1);r.set("x2",q.x2);
            r.set("delta_min",q.deltaMin);r.set("delta_max",q.deltaMax);r.set("witness_hops",2);return r;
        }
        throw std::runtime_error("cannot sample a short two-event witness; adjust short-fraction/max-events explicitly");
    }
    Record selectCorrectness(const Args& a) {
        std::mt19937_64 rng(a.u64("query-seed"));
        auto pick=[&](std::uint64_t n){return std::uniform_int_distribution<std::uint64_t>(0,n-1)(rng);};
        const Time full=graph.maxTime-graph.minTime;
        const Time span=std::min(full,std::max<Time>(1,static_cast<Time>(std::ceil(full*a.real("short-fraction",0.005)))));
        if(full<1 || graph.vertexCount()<2)throw std::runtime_error("graph too small for correctness sampling");
        const auto start=Clock::now();Deadline overall(a.real("selection-timeout",60));
        std::filesystem::path logFile=a.get("attempts-file");std::filesystem::create_directories(logFile.parent_path());
        if(std::filesystem::exists(logFile))throw std::runtime_error("selection log already exists; inspect interrupted attempt");
        std::ofstream log(logFile);if(!log)throw std::runtime_error("cannot write selection attempts");
        log<<"attempt\tsrc\tdst\tx1\tx2\tdelta_min\tdelta_max\tstatus\tanswers\tonline_total_ms\n";
        for(std::uint64_t attempt=1;attempt<=a.u64("max-attempts",200000);++attempt) {
            if(overall.pollNow())break;
            Query q;q.source=pick(graph.vertexCount());q.target=pick(graph.vertexCount()-1);if(q.target>=q.source)++q.target;
            q.x1=graph.minTime+pick(static_cast<std::uint64_t>(full-span)+1);q.x2=q.x1+span;
            q.deltaMin=(span+4)/5;q.deltaMax=(2*span+4)/5;
            Deadline d(a.real("screen-timeout",2));
            auto m=runTWalkEnum(graph,q,d,1,std::numeric_limits<std::uint64_t>::max());m.search.result.finishEmpty();
            const bool timeout=m.timedOut || d.pollNow();
            const bool positive=!m.search.result.count.isZero();
            log<<attempt<<'\t'<<q.source<<'\t'<<q.target<<'\t'<<q.x1<<'\t'<<q.x2<<'\t'<<q.deltaMin<<'\t'<<q.deltaMax<<'\t'<<(timeout?"TIMEOUT":"OK")<<'\t'<<m.search.result.count.str()<<'\t'<<m.totalMs<<'\n';
            if(!timeout && positive) {
                Record r;r.set("status","OK");r.set("src",q.source);r.set("dst",q.target);r.set("x1",q.x1);r.set("x2",q.x2);
                r.set("delta_min",q.deltaMin);r.set("delta_max",q.deltaMax);r.set("selection_answers",m.search.result.count.str());
                r.set("selection_attempts",attempt);r.setDouble("selection_ms",elapsedMs(start));return r;
            }
        }
        Record r;r.set("status","SELECTION_INCOMPLETE");r.setDouble("selection_ms",elapsedMs(start));return r;
    }
};

int main(int argc,char** argv) {
    try {
        Args initial(argc,argv);
        if(initial.get("action")=="cache-graph") {
            LoadOptions o;o.delimiter=initial.get("delimiter","space")=="comma"?',':' ';
            o.srcColumn=initial.i64("src-column",0);o.dstColumn=initial.i64("dst-column",1);o.timeColumn=initial.i64("time-column",2);o.idColumn=initial.i64("id-column",-1);o.skipHeader=initial.flag("skip-header");
            auto g=loadTextGraph(initial.get("graph-file"),o);saveGraphCache(g,initial.get("graph-cache"));return 0;
        }
        Session session;session.graph=loadGraphCache(initial.get("graph-cache"));
        std::cout<<"TPATH_READY\t"<<session.graph.vertexCount()<<'\t'<<session.graph.edges.size()<<'\t'<<session.graph.minTime<<'\t'<<session.graph.maxTime<<'\t'<<graphFingerprint(session.graph)<<std::endl;
        std::string line;
        while(std::getline(std::cin,line)) {
            if(line=="QUIT")break;
            Record r;bool fatal=false;
            try {
                auto values=split(line);values.insert(values.begin(),"worker");
                std::vector<char*> pointers;for(auto& v:values)pointers.push_back(v.data());
                Args a(static_cast<int>(pointers.size()),pointers.data());const auto action=a.get("action");
                if(action=="online-filter")r=session.onlineFilter(a);
                else if(action=="temporal-reach")r=session.temporalReach(a);
                else if(action=="sweep")r=session.sweep(a);
                else if(action=="build-index")r=session.build(a);
                else if(action=="load-index")r=session.load(a);
                else if(action=="index-query")r=session.indexQuery(a);
                else if(action=="ablation")r=session.ablation(a);
                else if(action=="correct")r=session.correct(a);
                else if(action=="sample-witness")r=session.sample(a);
                else if(action=="select-correctness")r=session.selectCorrectness(a);
                else if(action=="clear-index"){session.clearIndex();r.set("status","OK");}
                else throw std::runtime_error("unknown action");
            } catch(const std::bad_alloc&) {activeResponseProbe=nullptr;captureMaterializedPathsForTests=false;r.set("status","OOM");fatal=true;}
            catch(const std::exception& e){activeResponseProbe=nullptr;captureMaterializedPathsForTests=false;r.set("status","ERROR");r.set("error",e.what());}
            r.print();std::cout<<"TPATH_END"<<std::endl;if(fatal)return 3;
        }
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 2;}
}
