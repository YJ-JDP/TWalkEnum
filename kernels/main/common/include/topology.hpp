#pragma once

#include "types.hpp"
#include <functional>

namespace tpx {

struct TopologyIndex {
    TopologyIndex() = default;
    explicit TopologyIndex(const Graph& graph) {
        formatVersion = 3;
        graphHash = graphFingerprint(graph);
        edgeCount = graph.edges.size();
        vertexCount = graph.vertexCount();
        outgoing.assign(graph.vertexCount(), {});
        incoming.assign(graph.vertexCount(), {});
        std::vector<std::pair<Vertex, Vertex>> pairs;
        pairs.reserve(graph.edges.size());
        for (const auto& edge : graph.edges) pairs.emplace_back(edge.src, edge.dst);
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
        for (const auto& [source, target] : pairs) {
            outgoing[source].push_back(target);
            incoming[target].push_back(source);
        }
    }
    std::vector<int> reverseDistance(Vertex target, std::uint32_t limit,
                                     std::uint64_t* workspaceBytes = nullptr) const {
        std::vector<int> distance(outgoing.size(), -1);
        std::vector<Vertex> queue;
        std::size_t head = 0;
        distance[target] = 0; queue.push_back(target);
        while (head < queue.size()) {
            const Vertex vertex = queue[head++];
            if (static_cast<std::uint32_t>(distance[vertex]) >= limit) continue;
            for (Vertex previous : incoming[vertex]) if (distance[previous] < 0) {
                distance[previous] = distance[vertex] + 1;
                queue.push_back(previous);
            }
        }
        if (workspaceBytes) {
            *workspaceBytes = distance.capacity() * sizeof(int) + queue.capacity() * sizeof(Vertex);
        }
        return distance;
    }
    std::uint64_t formatVersion = 3;
    std::uint64_t graphHash = 0;
    std::uint64_t edgeCount = 0;
    std::uint64_t vertexCount = 0;
    std::vector<std::vector<Vertex>> outgoing;
    std::vector<std::vector<Vertex>> incoming;
};

inline std::uint64_t topologyIndexBytes(const TopologyIndex& index) {
    std::uint64_t bytes = (index.outgoing.capacity() + index.incoming.capacity()) *
        sizeof(std::vector<Vertex>);
    for (const auto& values : index.outgoing) bytes += values.capacity() * sizeof(Vertex);
    for (const auto& values : index.incoming) bytes += values.capacity() * sizeof(Vertex);
    return bytes;
}

inline void writeNestedVertices(std::ofstream& out,
                                const std::vector<std::vector<Vertex>>& values) {
    const std::uint64_t size = values.size();
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    for (const auto& row : values) writeVector(out, row);
}

inline void readNestedVertices(std::ifstream& in, std::vector<std::vector<Vertex>>& values) {
    std::uint64_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("corrupt topology index length");
    }
    values.resize(static_cast<std::size_t>(size));
    for (auto& row : values) readVector(in, row);
}

inline void saveTopologyIndex(const TopologyIndex& index, const std::string& path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create topology index: " + path);
    const std::uint64_t magic = 0x545058544f504f33ULL;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&index.formatVersion), sizeof(index.formatVersion));
    out.write(reinterpret_cast<const char*>(&index.graphHash), sizeof(index.graphHash));
    out.write(reinterpret_cast<const char*>(&index.edgeCount), sizeof(index.edgeCount));
    out.write(reinterpret_cast<const char*>(&index.vertexCount), sizeof(index.vertexCount));
    writeNestedVertices(out, index.outgoing); writeNestedVertices(out, index.incoming);
}

inline TopologyIndex loadTopologyIndex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open topology index: " + path);
    std::uint64_t magic = 0; in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x545058544f504f33ULL) throw std::runtime_error("invalid topology index");
    TopologyIndex index;
    in.read(reinterpret_cast<char*>(&index.formatVersion), sizeof(index.formatVersion));
    in.read(reinterpret_cast<char*>(&index.graphHash), sizeof(index.graphHash));
    in.read(reinterpret_cast<char*>(&index.edgeCount), sizeof(index.edgeCount));
    in.read(reinterpret_cast<char*>(&index.vertexCount), sizeof(index.vertexCount));
    if (index.formatVersion != 3) throw std::runtime_error("unsupported topology index version");
    readNestedVertices(in, index.outgoing); readNestedVertices(in, index.incoming);
    if (!in) throw std::runtime_error("truncated topology index");
    return index;
}

inline void requireTopologyIndexMatch(const TopologyIndex& index, const Graph& graph) {
    if (index.formatVersion != 3 || index.graphHash != graphFingerprint(graph) ||
        index.edgeCount != graph.edges.size() || index.vertexCount != graph.vertexCount() ||
        index.outgoing.size() != graph.vertexCount() || index.incoming.size() != graph.vertexCount()) {
        throw std::runtime_error("topology index metadata does not match graph");
    }
}

}
