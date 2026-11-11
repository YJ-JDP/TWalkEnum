#include "types.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const auto graph = tpx::loadTextGraph(argv[1], tpx::LoadOptions{});
        tpx::Record result;
        result.set("status", "OK");
        result.set("graph_vertex_count", graph.vertexCount());
        result.set("graph_edge_count", graph.edges.size());
        result.set("graph_min_time", graph.minTime);
        result.set("graph_max_time", graph.maxTime);
        result.set("graph_time_span", graph.timeSpan());
        result.set("graph_fingerprint", tpx::graphFingerprint(graph));
        result.print();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
