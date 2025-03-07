#include "memory_allocator.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstring>

// Structure for basic allocation testing
struct TestObject {
    int id;
    double value;
    char name[50];
    
    TestObject(int i, double v, const char* n) : id(i), value(v) {
        std::strncpy(name, n, 49);
        name[49] = '\0';
    }
    
    void print() const {
        std::cout << "TestObject(" << id << ", " << value << ", \"" << name << "\")" << std::endl;
    }
};

// Helper for timing tests
class Timer {
public:
    Timer(const std::string& name) : name_(name), start_(std::chrono::high_resolution_clock::now()) {}
    
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        std::cout << name_ << ": " << duration << " microseconds" << std::endl;
    }
    
private:
    std::string name_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// Test basic allocation and deallocation
void testBasicAllocation() {
    std::cout << "\n=== Basic Allocation Test ===\n";
    
    // Create a memory pool of 4KB
    memory::BasicAllocator allocator(4096);
    
    // Print initial state
    std::cout << "Initial state:" << std::endl;
    allocator.debugPrintFreeList();
    
    // Allocate a few objects
    void* ptr1 = allocator.allocate(sizeof(TestObject));
    void* ptr2 = allocator.allocate(sizeof(TestObject));
    void* ptr3 = allocator.allocate(sizeof(TestObject));
    
    if (ptr1 && ptr2 && ptr3) {
        // Construct objects
        new (ptr1) TestObject(1, 1.1, "First");
        new (ptr2) TestObject(2, 2.2, "Second");
        new (ptr3) TestObject(3, 3.3, "Third");
        
        // Print objects
        static_cast<TestObject*>(ptr1)->print();
        static_cast<TestObject*>(ptr2)->print();
        static_cast<TestObject*>(ptr3)->print();
        
        // Print free list after allocations
        std::cout << "\nAfter allocations:" << std::endl;
        allocator.debugPrintFreeList();
        
        // Free objects
        static_cast<TestObject*>(ptr1)->~TestObject();
        static_cast<TestObject*>(ptr2)->~TestObject();
        static_cast<TestObject*>(ptr3)->~TestObject();
        
        allocator.deallocate(ptr1);
        allocator.deallocate(ptr2);
        allocator.deallocate(ptr3);
        
        // Print free list after deallocations
        std::cout << "\nAfter deallocations:" << std::endl;
        allocator.debugPrintFreeList();
    } else {
        std::cerr << "Allocation failed!" << std::endl;
    }
}

// Test alignment
void testAlignment() {
    std::cout << "\n=== Alignment Test ===\n";
    
    memory::BasicAllocator allocator(4096);
    
    // Allocate with different alignments
    void* ptr1 = allocator.allocate(10, 1);  // Align to 1 byte
    void* ptr2 = allocator.allocate(10, 4);  // Align to 4 bytes
    void* ptr3 = allocator.allocate(10, 8);  // Align to 8 bytes
    void* ptr4 = allocator.allocate(10, 16); // Align to 16 bytes
    
    std::cout << "1-byte alignment: " << ptr1 << " (%" << (reinterpret_cast<uintptr_t>(ptr1) % 1) << ")" << std::endl;
    std::cout << "4-byte alignment: " << ptr2 << " (%" << (reinterpret_cast<uintptr_t>(ptr2) % 4) << ")" << std::endl;
    std::cout << "8-byte alignment: " << ptr3 << " (%" << (reinterpret_cast<uintptr_t>(ptr3) % 8) << ")" << std::endl;
    std::cout << "16-byte alignment: " << ptr4 << " (%" << (reinterpret_cast<uintptr_t>(ptr4) % 16) << ")" << std::endl;
    
    allocator.deallocate(ptr1);
    allocator.deallocate(ptr2);
    allocator.deallocate(ptr3);
    allocator.deallocate(ptr4);
}

// Test fragmentation behavior
void testFragmentation() {
    std::cout << "\n=== Fragmentation Test ===\n";
    
    memory::BasicAllocator allocator(4096);
    
    std::cout << "Initial state:" << std::endl;
    allocator.debugPrintFreeList();
    
    // Allocate several blocks
    constexpr size_t numBlocks = 10;
    void* blocks[numBlocks];
    
    std::cout << "\nAllocating " << numBlocks << " blocks of varied sizes..." << std::endl;
    for (size_t i = 0; i < numBlocks; ++i) {
        size_t size = 100 + (i * 20); // Different sizes
        blocks[i] = allocator.allocate(size);
        if (!blocks[i]) {
            std::cerr << "Allocation failed at block " << i << std::endl;
            break;
        }
    }
    
    allocator.debugPrintFreeList();
    
    std::cout << "\nFreeing odd-indexed blocks to create fragmentation..." << std::endl;
    for (size_t i = 1; i < numBlocks; i += 2) {
        allocator.deallocate(blocks[i]);
        blocks[i] = nullptr;
    }
    
    allocator.debugPrintFreeList();
    
    std::cout << "\nTrying to allocate a large block..." << std::endl;
    void* largeBlock = allocator.allocate(1000);
    if (largeBlock) {
        std::cout << "Succeeded in allocating large block." << std::endl;
        allocator.deallocate(largeBlock);
    } else {
        std::cout << "Failed to allocate large block due to fragmentation!" << std::endl;
    }
    
    std::cout << "\nFreeing remaining blocks..." << std::endl;
    for (size_t i = 0; i < numBlocks; ++i) {
        if (blocks[i]) {
            allocator.deallocate(blocks[i]);
        }
    }
    
    allocator.debugPrintFreeList();
}

// Simple performance comparison
void testPerformance() {
    std::cout << "\n=== Performance Test ===\n";
    
    constexpr size_t poolSize = 1024 * 1024; // 1MB
    constexpr size_t numAllocations = 10000;
    constexpr size_t maxObjectSize = 100;
    
    // Random allocations with our allocator
    {
        memory::BasicAllocator allocator(poolSize);
        std::vector<void*> allocations;
        allocations.reserve(numAllocations);
        
        {
            Timer timer("Our allocator - 10k random allocations");
            for (size_t i = 0; i < numAllocations; ++i) {
                size_t size = 1 + rand() % maxObjectSize;
                void* ptr = allocator.allocate(size);
                allocations.push_back(ptr);
            }
        }
        
        {
            Timer timer("Our allocator - 10k deallocations");
            for (void* ptr : allocations) {
                allocator.deallocate(ptr);
            }
        }
    }
    
    // Random allocations with malloc/free
    {
        std::vector<void*> allocations;
        allocations.reserve(numAllocations);
        
        {
            Timer timer("malloc - 10k random allocations");
            for (size_t i = 0; i < numAllocations; ++i) {
                size_t size = 1 + rand() % maxObjectSize;
                void* ptr = malloc(size);
                allocations.push_back(ptr);
            }
        }
        
        {
            Timer timer("free - 10k deallocations");
            for (void* ptr : allocations) {
                free(ptr);
            }
        }
    }
}

int main() {
    // Seed the random number generator
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    
    // Run tests
    testBasicAllocation();
    testAlignment();
    testFragmentation();
    testPerformance();
    
    return 0;
}