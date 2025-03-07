#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>
#include "memory_allocator.h" // Contains ThreadsafeBasicAllocator + ThreadsafeCoalescingAllocator

// -----------------------------------------------
// 1) Multi-thread test for Basic Alloc
// -----------------------------------------------
void workerBasic(ThreadsafeBasicAllocator* alloc, int numOps) {
    std::vector<void*> localPtrs;
    localPtrs.reserve(numOps / 2);

    std::default_random_engine rng(std::random_device{}());
    std::uniform_int_distribution<int> sizeDist(1, 256);  // random block sizes
    std::uniform_int_distribution<int> opDist(0, 99);

    for(int i = 0; i < numOps; i++){
        int op = opDist(rng);
        if(op < 50) {
            // ~50% allocate
            size_t sz = sizeDist(rng);
            void* p   = alloc->allocate(sz);
            if(p) {
                localPtrs.push_back(p);
            }
        } else {
            // ~50% free
            if(!localPtrs.empty()) {
                int idx = rng() % localPtrs.size();
                void* p = localPtrs[idx];
                localPtrs[idx] = localPtrs.back();
                localPtrs.pop_back();
                alloc->deallocate(p);
            }
        }
    }
    // final free
    for(void* p : localPtrs) {
        alloc->deallocate(p);
    }
}

void threadedTestBasicAllocator() {
    std::cout << "\n=== Threaded Test: Basic Allocator ===\n";
    const int NUM_THREADS    = 4;
    const int OPS_PER_THREAD = 50000;

    ThreadsafeBasicAllocator alloc(2 * 1024 * 1024); // 2MB

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for(int i=0; i<NUM_THREADS; i++){
        threads.emplace_back(workerBasic, &alloc, OPS_PER_THREAD);
    }
    for(auto& t : threads){
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "threadedTestBasicAllocator done.\n";
    std::cout << "Elapsed time: " << dur_us << " microseconds\n";

    // Show final stats
    auto stats = alloc.getStats();
    std::cout << "Alloc calls: "   << stats.totalAllocCalls
              << ", Dealloc calls: " << stats.totalDeallocCalls
              << ", Peak usage: "    << stats.peakUsedBytes << " bytes\n";
}

// -----------------------------------------------
// 2) Multi-thread test for Coalescing Alloc
// -----------------------------------------------
void workerCoalescing(ThreadsafeCoalescingAllocator* alloc, int numOps) {
    std::vector<void*> localPtrs;
    localPtrs.reserve(numOps / 2);

    std::default_random_engine rng(std::random_device{}());
    std::uniform_int_distribution<int> sizeDist(1, 512);
    std::uniform_int_distribution<int> opDist(0, 99);

    for(int i=0; i<numOps; i++){
        int op = opDist(rng);
        if(op < 60) {
            // ~60% allocate
            size_t sz = sizeDist(rng);
            void* p   = alloc->allocate(sz);
            if(p) {
                localPtrs.push_back(p);
            }
        } else {
            // ~40% free
            if(!localPtrs.empty()) {
                int idx = rng() % localPtrs.size();
                void* p = localPtrs[idx];
                localPtrs[idx] = localPtrs.back();
                localPtrs.pop_back();
                alloc->deallocate(p);
            }
        }
    }
    // final free
    for(void* p : localPtrs) {
        alloc->deallocate(p);
    }
}

void threadedTestCoalescingAllocator() {
    std::cout << "\n=== Threaded Test: Coalescing Allocator ===\n";
    const int NUM_THREADS    = 4;
    const int OPS_PER_THREAD = 80000;

    ThreadsafeCoalescingAllocator alloc(4 * 1024 * 1024); // 4MB

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for(int i=0; i<NUM_THREADS; i++){
        threads.emplace_back(workerCoalescing, &alloc, OPS_PER_THREAD);
    }
    for(auto& t : threads){
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "threadedTestCoalescingAllocator done.\n";
    std::cout << "Elapsed time: " << dur_us << " microseconds\n";

    // final stats
    auto stats = alloc.getStats();
    std::cout << "Alloc calls: "   << stats.totalAllocCalls
              << ", Dealloc calls: " << stats.totalDeallocCalls
              << ", Peak usage: "    << stats.peakUsedBytes << " bytes\n";
}

// -----------------------------------------------
// 3) Multi-thread test for system malloc/free
// -----------------------------------------------
void workerMalloc(int numOps) {
    std::vector<void*> localPtrs;
    localPtrs.reserve(numOps / 2);

    std::default_random_engine rng(std::random_device{}());
    std::uniform_int_distribution<int> sizeDist(1, 512);
    std::uniform_int_distribution<int> opDist(0, 99);

    for(int i=0; i<numOps; i++){
        int op = opDist(rng);
        if(op < 60) {
            // ~60% allocate
            size_t sz = sizeDist(rng);
            void* p   = std::malloc(sz);
            if(p) {
                localPtrs.push_back(p);
            }
        } else {
            // ~40% free
            if(!localPtrs.empty()) {
                int idx = rng() % localPtrs.size();
                void* p = localPtrs[idx];
                localPtrs[idx] = localPtrs.back();
                localPtrs.pop_back();
                std::free(p);
            }
        }
    }
    // final free
    for(void* p : localPtrs) {
        std::free(p);
    }
}

void threadedTestMalloc() {
    std::cout << "\n=== Threaded Test: System malloc/free ===\n";
    const int NUM_THREADS    = 4;
    const int OPS_PER_THREAD = 60000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for(int i=0; i<NUM_THREADS; i++){
        threads.emplace_back(workerMalloc, OPS_PER_THREAD);
    }
    for(auto& t : threads){
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "threadedTestMalloc done.\n";
    std::cout << "Elapsed time: " << dur_us << " microseconds\n";
}

// -----------------------------------------------
// Main
// -----------------------------------------------
int main() {
    std::srand(static_cast<unsigned>(time(nullptr)));

    // 1) Basic Alloc
    threadedTestBasicAllocator();

    // 2) Coalescing Alloc
    threadedTestCoalescingAllocator();

    // 3) System Malloc
    threadedTestMalloc();

    std::cout << "\nAll tests completed.\n";
    return 0;
}
