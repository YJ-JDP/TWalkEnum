#include "search_experiments.hpp"
#include <iostream>
#include <random>
#include <set>
using namespace tpx;
using Paths = std::vector<std::vector<std::uint64_t>>;
void brute(const Graph& g, const Query& q, Vertex v, Time last, bool first,
           std::vector<std::uint64_t>& path, Paths& out) {
    for (const auto& e : g.edges) {
        if(e.src != v || e.time < q.x1 || e.time > q.x2 || !endpointAdmissible(e,q)) continue;
        if(!first && (e.time-last < q.effectiveMin() || e.time-last > q.deltaMax)) continue;
        path.push_back(e.originalId);
        if(e.dst==q.target) out.push_back(path);
        else brute(g,q,e.dst,e.time,false,path,out);
        path.pop_back();
    }
}
int main(int argc,char** argv) {
    std::mt19937 rng(34); int comparisons=0;
    captureMaterializedPathsForTests=true;
    for(int file=1; file<argc; ++file) {
        Graph g=loadTextGraph(argv[file],LoadOptions{});
        for(int j=0;j<12;++j) {
            Query q; q.source=rng()%g.vertexCount();q.target=(q.source+1+rng()%(g.vertexCount()-1))%g.vertexCount();
            q.x1=rng()%4;q.x2=9+rng()%5;q.deltaMin=1+rng()%2;q.deltaMax=q.deltaMin+1+rng()%3;
            Paths expected;std::vector<std::uint64_t> path;brute(g,q,q.source,0,true,path,expected);
            std::sort(expected.begin(),expected.end());
            for(int k=0;k<9;++k) {
                SearchOptions o;o.workers=k==8?4:1;
                if(k==1)o.variant=SearchVariant::NoChrono;
                if(k==2)o.variant=SearchVariant::NoPivot;
                if(k==3)o.variant=SearchVariant::NoGapBalance;
                if(k>=4 && k<=7){o.windowMode=WindowMode::Random;o.windowSeed=k-3;}
                auto m=runSearch(g,q,o);
                auto got=m.tw.search.result.materializedPaths;std::sort(got.begin(),got.end());
                if(got!=expected || m.timedOut || m.tw.search.prefixStorageSaturated) {
                    std::cerr<<"FAIL file="<<file<<" query="<<j<<" variant="<<k<<" expected="<<expected.size()<<" got="<<got.size()<<"\n";return 1;
                }
                if(k==0) {
                    Deadline d;auto old=runTWalkEnum(g,q,d,1,std::numeric_limits<std::uint64_t>::max());
                    auto p=old.search.result.materializedPaths;std::sort(p.begin(),p.end());
                    if(p!=got)return 2;
                }
                ++comparisons;
            }
        }
    }
    std::cout<<"PASS "<<comparisons<<" complete event-ID sequence comparisons\n";
}
