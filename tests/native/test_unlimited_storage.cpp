#include "strict_adaptations.hpp"
#if __has_include("index.hpp")
#include "index.hpp"
#define HAS_LAYOUTS 1
#endif
#include <stdexcept>
using namespace tpx;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    static_assert(sizeof(StoredSpanRef{}.offset) == 8, "64-bit prefix offset required");
    Graph graph = loadTextGraph(argv[1], LoadOptions{});
    Query query;
    query.source = 0; query.target = 2;
    query.x1 = 1; query.x2 = 8; query.deltaMin = 1; query.deltaMax = 3;
    TopologyIndex topology(graph);
    Deadline filterDeadline;
    auto support = slidingTightGraph(graph, query, filterDeadline);
    auto tight = makeTightGraph(graph, query, std::move(support.edgeIds));
    auto stats = buildSuccessorStats(tight, filterDeadline);
    WindowChoice window; window.left = 3; window.right = 5;
    captureMaterializedPathsForTests = true;
    Deadline referenceDeadline;
    auto reference = directDfs(tight, stats, referenceDeadline);
    auto expected = reference.result.materializedPaths;
    std::sort(expected.begin(), expected.end());
    require(expected.size() == 8, "reference count mismatch");
    for (auto budget : {std::uint64_t{0}, std::uint64_t{1}}) {
        Deadline deadline;
        auto result = pivotDfs(tight, stats, window, deadline, budget, 2);
        auto paths = result.result.materializedPaths;
        std::sort(paths.begin(), paths.end());
        require(paths == expected, "pivot full-sequence mismatch");
        require(result.storedPrefixEdgeIds > 1 && !result.prefixStorageSaturated,
                "prefix storage is limited");
        require(result.prefixBudgetNodes == 0, "budget metadata mismatch");
        Deadline cpeDeadline;
        auto cpe = runCpeStrict(graph, topology, query, cpeDeadline, 1, budget);
        cpe.result.finishEmpty();
        require(!cpe.resourceCap && !cpe.unsupportedHop && cpe.partialPaths > 1 &&
                cpe.result.count.str() == "8", "CPE enumeration is limited");
        Deadline pathDeadline;
        auto path = runPathEnumStrict(graph, topology, query, pathDeadline, 1);
        path.result.finishEmpty();
        require(!path.unsupportedHop && path.result.count.str() == "8",
                "hop pruning is capped");
#ifdef HAS_LAYOUTS
        Deadline l1Deadline, l2Deadline;
        auto l1 = buildLevel1(graph, 1, 3, l1Deadline, budget);
        auto l2 = buildLevel2(graph, 1, 3, l2Deadline, budget);
        require(!l1.metrics.memoryLimit && !l2.metrics.memoryLimit &&
                !l1.succIds.empty() && !l2.succIds.empty(), "index storage is limited");
#endif
    }
    std::cout << "PASS unlimited storage and path caps; complete pivot sequences match DFS\n";
}
