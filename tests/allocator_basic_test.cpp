#include "../memory_allocator.h"
#include <cassert>
#include <iostream>

int main() {
    FancyPerThreadAllocator alloc(1024 * 1024); // 1MB arena
    void* ptr = alloc.allocate(128);
    assert(ptr != nullptr);
    alloc.deallocate(ptr);

    AllocStatsSnapshot snap = alloc.getStatsSnapshot();
    assert(snap.currentUsedBytes == 0);

    std::cout << "Basic allocation test passed\n";
    return 0;
}
