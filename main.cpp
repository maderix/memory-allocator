#include "memory_allocator.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef DEBUG
#  define PRINT_FREE_LIST(a) (a).debugPrintFreeList()
#else
#  define PRINT_FREE_LIST(a) (void)0
#endif

// A simple test object
struct TestObject {
    int id;
    double value;
    char name[50];
    TestObject(int i, double v, const char* n) : id(i), value(v) {
        std::strncpy(name, n, 49);
        name[49] = '\0';
    }
    void print() const {
        std::cout << "TestObject(" << id << ", " << value 
                  << ", \"" << name << "\")\n";
    }
};

// A timing helper
class Timer {
public:
    Timer(const std::string& label)
        : label_(label), start_(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto us  = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        std::cout << label_ << ": " << us << " microseconds" << std::endl;
    }
private:
    std::string label_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

void testBasicAllocation() {
    std::cout << "\n=== Basic Allocation Test ===\n";
    memory::BasicAllocator alloc(4096);

    std::cout << "Initial state:\n";
    PRINT_FREE_LIST(alloc);

    void* p1 = alloc.allocate(sizeof(TestObject));
    void* p2 = alloc.allocate(sizeof(TestObject));
    void* p3 = alloc.allocate(sizeof(TestObject));
    if (p1 && p2 && p3) {
        new (p1) TestObject(1, 1.1, "First");
        new (p2) TestObject(2, 2.2, "Second");
        new (p3) TestObject(3, 3.3, "Third");

        static_cast<TestObject*>(p1)->print();
        static_cast<TestObject*>(p2)->print();
        static_cast<TestObject*>(p3)->print();

        std::cout << "\nAfter allocations:\n";
        PRINT_FREE_LIST(alloc);

        static_cast<TestObject*>(p1)->~TestObject();
        alloc.deallocate(p1);
        static_cast<TestObject*>(p2)->~TestObject();
        alloc.deallocate(p2);
        static_cast<TestObject*>(p3)->~TestObject();
        alloc.deallocate(p3);

        std::cout << "\nAfter deallocations:\n";
        PRINT_FREE_LIST(alloc);
    }
}

void testAlignment() {
    std::cout << "\n=== Alignment Test ===\n";
    memory::BasicAllocator alloc(4096);
    void* p1 = alloc.allocate(10, 1);
    void* p2 = alloc.allocate(10, 4);
    void* p3 = alloc.allocate(10, 8);
    void* p4 = alloc.allocate(10, 16);

    std::cout << "1-byte alignment: " << p1 
              << " (%" << (reinterpret_cast<uintptr_t>(p1) % 1) << ")\n";
    std::cout << "4-byte alignment: " << p2 
              << " (%" << (reinterpret_cast<uintptr_t>(p2) % 4) << ")\n";
    std::cout << "8-byte alignment: " << p3 
              << " (%" << (reinterpret_cast<uintptr_t>(p3) % 8) << ")\n";
    std::cout << "16-byte alignment: " << p4
              << " (%" << (reinterpret_cast<uintptr_t>(p4) % 16) << ")\n";

    alloc.deallocate(p1);
    alloc.deallocate(p2);
    alloc.deallocate(p3);
    alloc.deallocate(p4);
}

void testCoalescing() {
    std::cout << "\n=== Coalescing Test (Improved Allocator) ===\n";
    memory::CoalescingAllocator alloc(8192);

    std::cout << "Initial:\n";
    PRINT_FREE_LIST(alloc);

    constexpr size_t n=10;
    void* blocks[n]{};

    std::cout << "\nAllocating 10 blocks...\n";
    for (size_t i = 0; i < n; ++i) {
        size_t sz = 100 + i*20;
        blocks[i] = alloc.allocate(sz);
        if (!blocks[i]) {
            std::cerr << "Allocation failed at block " << i << "\n";
            break;
        }
    }
    PRINT_FREE_LIST(alloc);

    std::cout << "\nFreeing odd blocks...\n";
    for (size_t i = 1; i < n; i+=2) {
        if (blocks[i]) {
            alloc.deallocate(blocks[i]);
            blocks[i] = nullptr;
        }
    }
    PRINT_FREE_LIST(alloc);

    std::cout << "\nFreeing blocks 2, 6...\n";
    if (blocks[2]) {
        alloc.deallocate(blocks[2]);
        blocks[2] = nullptr;
    }
    if (blocks[6]) {
        alloc.deallocate(blocks[6]);
        blocks[6] = nullptr;
    }
    PRINT_FREE_LIST(alloc);

    std::cout << "\nAlloc large 1000:\n";
    void* large = alloc.allocate(1000);
    if (large) {
        std::cout << "Success!\n";
        alloc.deallocate(large);
    } else {
        std::cout << "Failed!\n";
    }

    std::cout << "\nFreeing the rest...\n";
    for (size_t i=0; i<n; ++i) {
        if (blocks[i]) {
            alloc.deallocate(blocks[i]);
            blocks[i] = nullptr;
        }
    }
    PRINT_FREE_LIST(alloc);
}

void testPerformance() {
    std::cout << "\n=== Performance Test ===\n";
    constexpr size_t poolSz = 1024*1024; // 1MB
    constexpr size_t nAllocs= 10000;
    constexpr size_t maxSize= 100;

    // Basic
    {
        std::cout << "\n- Basic Allocator Performance:\n";
        memory::BasicAllocator alloc(poolSz);
        std::vector<void*> ptrs; 
        ptrs.reserve(nAllocs);

        {
            Timer t("  Basic - 10k allocations");
            for (size_t i=0; i<nAllocs; ++i) {
                size_t sz = 1 + rand()%maxSize;
                void* p   = alloc.allocate(sz);
                if (p) ptrs.push_back(p);
            }
        }
        {
            Timer t("  Basic - 10k deallocations");
            for (auto* p : ptrs) {
                alloc.deallocate(p);
            }
        }
    }
    // Coalescing
    {
        std::cout << "\n- Coalescing Allocator Performance:\n";
        memory::CoalescingAllocator alloc(poolSz + 64);
        std::vector<void*> ptrs;
        ptrs.reserve(nAllocs);

        {
            Timer t("  Coalescing - 10k allocations");
            for (size_t i=0; i<nAllocs; ++i) {
                size_t sz = 1 + rand()%maxSize;
                void* p   = alloc.allocate(sz);
                if (p) ptrs.push_back(p);
            }
        }
        {
            Timer t("  Coalescing - 10k deallocations");
            for (auto* p : ptrs) {
                alloc.deallocate(p);
            }
        }
    }
    // system
    {
        std::cout << "\n- System Allocator Performance:\n";
        std::vector<void*> ptrs;
        ptrs.reserve(nAllocs);

        {
            Timer t("  malloc - 10k allocations");
            for (size_t i=0; i<nAllocs; ++i) {
                size_t sz = 1 + rand()%maxSize;
                void* p   = std::malloc(sz);
                ptrs.push_back(p);
            }
        }
        {
            Timer t("  free - 10k deallocations");
            for (auto* p : ptrs) {
                std::free(p);
            }
        }
    }
}

int main() {
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    testBasicAllocation();
    testAlignment();
    testCoalescing();
    testPerformance();

    return 0;
}
