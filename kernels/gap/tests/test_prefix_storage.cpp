#include "types.hpp"
#include <cassert>
#include <type_traits>
using namespace tpx;
int main() {
    static_assert(std::is_same<decltype(StoredSpanRef{}.offset), std::uint64_t>::value, "64-bit offset required");
    StoredSpanRef large;
    large.offset = (std::uint64_t{1} << 32) + 123;
    assert(large.offset == 4294967419ULL);
    std::vector<StoredSpanRef> descriptors{large};
    assert(descriptors[0].offset == large.offset);
    ConcreteEdgeId arena[] = {11,12,13};
    ConcreteEdgeId suffix[] = {14};
    StoredSpanRef prefixes[2];
    prefixes[0].offset=0;prefixes[0].length=1;
    prefixes[1].offset=1;prefixes[1].length=2;
    ResultSummary result;
    result.emitJoinedRange(arena,prefixes,std::uint64_t{2},suffix,1);
    result.finishEmpty();
    assert(result.count.str()=="2" && result.outputVolume.str()=="5");
}
