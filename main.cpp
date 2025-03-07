#include "memory_allocator.h"  // your FancyPerThreadAllocator
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>
#include <mutex>

// A simple interface to unify "FancyPerThreadAllocator" and "SystemAlloc"
class AllocInterface {
public:
    virtual ~AllocInterface() {}
    virtual void* allocate(size_t size) = 0;
    virtual void deallocate(void* ptr) = 0;
    // For stats if available, system alloc won't have stats
    virtual bool hasStats() const { return false; }
    virtual AllocStatsSnapshot getStatsSnapshot() const { return AllocStatsSnapshot{0,0,0,0}; }
};

////////////////////////////////////////////////////////
// 1) Implementation for FancyPerThreadAllocator
////////////////////////////////////////////////////////
class FancyAllocWrapper : public AllocInterface {
public:
    FancyAllocWrapper(size_t arenaSize)
        : fpa_(arenaSize)
    {}

    void* allocate(size_t size) override {
        return fpa_.allocate(size);
    }
    void deallocate(void* ptr) override {
        fpa_.deallocate(ptr);
    }
    bool hasStats() const override { return true; }
    AllocStatsSnapshot getStatsSnapshot() const override {
        return fpa_.getStatsSnapshot();
    }

private:
    FancyPerThreadAllocator fpa_;
};

////////////////////////////////////////////////////////
// 2) Implementation for system malloc/free
////////////////////////////////////////////////////////
class SystemAllocWrapper : public AllocInterface {
public:
    void* allocate(size_t size) override {
        return std::malloc(size);
    }
    void deallocate(void* ptr) override {
        std::free(ptr);
    }
    // has no stats
};

////////////////////////////////////////////////////////
// Global pointer pool for the concurrency test
////////////////////////////////////////////////////////
static constexpr size_t GLOBAL_CAPACITY = 100000;
static std::vector<void*> g_ptrs(GLOBAL_CAPACITY, nullptr);
static std::mutex g_ptrsMutex;

////////////////////////////////////////////////////////
// The concurrency test function
////////////////////////////////////////////////////////
struct TestResult {
    long long elapsedMicroseconds;
    // If the alloc has stats
    size_t totalAllocCalls;
    size_t totalFreeCalls;
    size_t peakUsage;
};

TestResult runHighConcurrencyTest(AllocInterface* alloc, int numThreads, int opsPerThread) {
    // reset global pointer array
    {
        std::lock_guard<std::mutex> lk(g_ptrsMutex);
        std::fill(g_ptrs.begin(), g_ptrs.end(), nullptr);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // spawn threads
    std::vector<std::thread> ths;
    ths.reserve(numThreads);

    for (int i=0; i<numThreads; i++) {
        ths.emplace_back([=](AllocInterface* a) {
            std::default_random_engine rng(std::random_device{}());
            std::uniform_int_distribution<int> sizeDist(1, 4096);
            std::uniform_int_distribution<int> opDist(0,99);
            std::uniform_int_distribution<size_t> idxDist(0, GLOBAL_CAPACITY - 1);

            for(int c=0; c<opsPerThread; c++){
                int op = opDist(rng);
                if (op<60) {
                    // 60% allocate
                    size_t sz = sizeDist(rng);
                    // pick slot
                    std::lock_guard<std::mutex> gl(g_ptrsMutex);
                    size_t idx = idxDist(rng);
                    if(!g_ptrs[idx]) {
                        void* p = a->allocate(sz);
                        g_ptrs[idx]=p;
                    }
                } else {
                    // 40% free
                    std::lock_guard<std::mutex> gl(g_ptrsMutex);
                    size_t idx = idxDist(rng);
                    if(g_ptrs[idx]) {
                        void* p = g_ptrs[idx];
                        g_ptrs[idx] = nullptr;
                        a->deallocate(p);
                    }
                }
            }
        }, alloc);
    }

    for (auto& t: ths) {
        t.join();
    }

    // final cleanup
    {
        std::lock_guard<std::mutex> lk(g_ptrsMutex);
        for (auto& ptr : g_ptrs) {
            if (ptr) {
                alloc->deallocate(ptr);
                ptr=nullptr;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us  = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    TestResult result;
    result.elapsedMicroseconds = us;

    if (alloc->hasStats()) {
        auto snap = alloc->getStatsSnapshot();
        result.totalAllocCalls = snap.totalAllocCalls;
        result.totalFreeCalls  = snap.totalFreeCalls;
        result.peakUsage       = snap.peakUsedBytes;
    } else {
        result.totalAllocCalls = 0;
        result.totalFreeCalls  = 0;
        result.peakUsage       = 0;
    }

    return result;
}

////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////
int main() {
    std::cout << "=== Detailed High Concurrency Comparison ===\n";

    // We define the test parameters
    const int THREADS=64;
    const int OPS=1000000; // 1 million ops per thread (64 million total)

    // 1) Fancy
    {
        FancyAllocWrapper fancy(64 * 1024 * 1024); // 64MB arena
        TestResult r = runHighConcurrencyTest(&fancy, THREADS, OPS);
        std::cout << "\n-- FancyPerThreadAllocator --\n";
        std::cout << "Threads     : " << THREADS << "\n";
        std::cout << "Ops/Thread  : " << OPS << "\n";
        std::cout << "Elapsed (us): " << r.elapsedMicroseconds << "\n";
        std::cout << "Alloc calls : " << r.totalAllocCalls << "\n";
        std::cout << "Free calls  : " << r.totalFreeCalls << "\n";
        std::cout << "Peak usage  : " << r.peakUsage << " bytes\n";
    }

    // 2) System
    {
        SystemAllocWrapper sysAlloc;
        TestResult r = runHighConcurrencyTest(&sysAlloc, THREADS, OPS);
        std::cout << "\n-- System malloc/free --\n";
        std::cout << "Threads     : " << THREADS << "\n";
        std::cout << "Ops/Thread  : " << OPS << "\n";
        std::cout << "Elapsed (us): " << r.elapsedMicroseconds << "\n";
        std::cout << "Alloc calls : " << r.totalAllocCalls << "\n"; // always 0 for system
        std::cout << "Free calls  : " << r.totalFreeCalls << "\n";  // always 0
        std::cout << "Peak usage  : " << r.peakUsage << " bytes\n"; // always 0
    }

    std::cout << "\nAll tests completed.\n";
    return 0;
}
