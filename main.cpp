#include "memory_allocator.h"  // your final memory_allocator.h with optional reclamation
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>


// We'll define an interface for system vs fancy
class AllocInterface {
public:
    virtual ~AllocInterface() {}
    virtual void* allocate(size_t sz)=0;
    virtual void  deallocate(void* ptr)=0;
    virtual bool hasStats() const { return false; }
    virtual AllocStatsSnapshot getStats() const { return {0,0,0,0}; }
};

// A wrapper for FancyPerThreadAllocator
class FancyInterface : public AllocInterface {
public:
    FancyInterface(size_t arenaSize, bool reclamation)
        : fancy_(arenaSize, reclamation)
    {}
    void* allocate(size_t sz) override {
        return fancy_.allocate(sz);
    }
    void deallocate(void* ptr) override {
        fancy_.deallocate(ptr);
    }
    bool hasStats() const override { return true; }
    AllocStatsSnapshot getStats() const override {
        return fancy_.getStatsSnapshot();
    }
private:
    FancyPerThreadAllocator fancy_;
};

// A wrapper for system malloc
class SystemInterface : public AllocInterface {
public:
    void* allocate(size_t sz) override { return std::malloc(sz); }
    void  deallocate(void* ptr) override { std::free(ptr); }
};

// We'll do an ephemeral HPC scenario with ring buffer 
void ephemeralWorker(AllocInterface* alloc, int ops, int ringSize)
{
    struct Slot { void* ptr; int ttl; };
    std::vector<Slot> ring(ringSize, {nullptr, 0});
    int pos=0;

    std::default_random_engine rng(std::random_device{}());
    std::uniform_int_distribution<int> catDist(1,100);
    std::uniform_int_distribution<int> smallDist(16,256);
    std::uniform_int_distribution<int> medDist(512,2048);
    std::uniform_int_distribution<int> largeDist(4096,32768);
    std::uniform_int_distribution<int> ttlDist(50,2000);

    for(int i=0; i<ops; i++){
        auto& slot = ring[pos];
        // free if expired
        if(slot.ptr && slot.ttl <= 0){
            alloc->deallocate(slot.ptr);
            slot.ptr = nullptr;
        }
        // decrement TTL
        if(slot.ptr && slot.ttl>0){
            slot.ttl--;
        }
        // if empty => allocate new
        if(!slot.ptr){
            int c = catDist(rng);
            size_t sz=0;
            if(c<=60) sz=smallDist(rng);
            else if(c<=90) sz=medDist(rng);
            else sz=largeDist(rng);

            void* p = alloc->allocate(sz);
            if(p){
                slot.ptr = p;
                slot.ttl = ttlDist(rng);
            }
        }
        pos = (pos+1) % ringSize;
    }
    // final free
    for(auto& s : ring){
        if(s.ptr){
            alloc->deallocate(s.ptr);
            s.ptr=nullptr;
        }
    }
}



// We'll do a timed test for ephemeral HPC scenario
struct TestResult {
    long long elapsedUs;
    AllocStatsSnapshot snap;
};

TestResult runEphemeralTest(AllocInterface* alloc, int threads, int opsPerThread, int ringSize)
{
    auto start=std::chrono::high_resolution_clock::now();

    std::vector<std::thread> ths;
    ths.reserve(threads);
    for(int i=0;i<threads;i++){
        ths.emplace_back(ephemeralWorker, alloc, opsPerThread, ringSize);
    }
    for(auto& t : ths){
        t.join();
    }
    auto end=std::chrono::high_resolution_clock::now();
    long long us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    TestResult r;
    r.elapsedUs= us;
    if(alloc->hasStats()){
        r.snap= alloc->getStats();
    } else {
        r.snap={0,0,0,0};
    }
    return r;
}

int main(){
    // HPC ephemeral big test
    int threads = 512;
    int opsPerThread = 1000000;
    int ringSize     = 500000;

    std::cout << "\n=== Compare System Malloc vs. Fancy(Off) vs. Fancy(On) under HPC ephemeral scenario ===\n";
    std::cout << "Threads= " << threads << ", Ops/Thread= " << opsPerThread << ", ringSize= " << ringSize << "\n";

    // 1) System
    {
        SystemInterface sys;
        auto r = runEphemeralTest(&sys, threads, opsPerThread, ringSize);
        std::cout << "\n-- System malloc/free --\n";
        std::cout << "Elapsed (us): " << r.elapsedUs << "\n";
    }

    // 2) Fancy Reclamation OFF
    {
        FancyInterface fancyNoReclaim(64ULL*1024ULL*1024ULL, false);
        auto r = runEphemeralTest(&fancyNoReclaim, threads, opsPerThread, ringSize);
        std::cout << "\n-- Fancy Per-Thread (Reclamation OFF) --\n";
        std::cout << "Elapsed (us): " << r.elapsedUs << "\n";
        std::cout << "Alloc calls : " << r.snap.totalAllocCalls 
                  << ", Free calls: " << r.snap.totalFreeCalls 
                  << ", Peak usage: " << r.snap.peakUsedBytes << "\n";
    }

    // 3) Fancy Reclamation ON
    {
        FancyInterface fancyReclaim(64ULL*1024ULL*1024ULL, true);
        auto r = runEphemeralTest(&fancyReclaim, threads, opsPerThread, ringSize);
        std::cout << "\n-- Fancy Per-Thread (Reclamation ON) --\n";
        std::cout << "Elapsed (us): " << r.elapsedUs << "\n";
        std::cout << "Alloc calls : " << r.snap.totalAllocCalls
                  << ", Free calls: " << r.snap.totalFreeCalls
                  << ", Peak usage: " << r.snap.peakUsedBytes << "\n";
    }

    std::cout << "\nAll tests completed.\n";
    return 0;
}
