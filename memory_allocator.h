#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include <cstddef>  // for size_t
#include <cstdint>  // for uintptr_t
#include <cassert>  // for assert
#include <memory>   // for std::align
#include <iostream> // for debugging

namespace memory {

/**
 * V1: A simple block-based memory allocator.
 * This version uses a basic free list approach without considering fragmentation.
 */
class BasicAllocator {
public:
    // Constructor: Allocate a pool of memory of the specified size
    explicit BasicAllocator(size_t poolSize) 
        : poolSize_(poolSize) {
        // Allocate memory pool
        pool_ = static_cast<char*>(::operator new(poolSize));
        
        // Initially, there's one big free block
        firstFreeBlock_ = reinterpret_cast<FreeBlock*>(pool_);
        firstFreeBlock_->size = poolSize;
        firstFreeBlock_->next = nullptr;
    }
    
    // Destructor: Release all memory
    ~BasicAllocator() {
        ::operator delete(pool_);
    }
    
    // Disable copying and moving
    BasicAllocator(const BasicAllocator&) = delete;
    BasicAllocator& operator=(const BasicAllocator&) = delete;
    BasicAllocator(BasicAllocator&&) = delete;
    BasicAllocator& operator=(BasicAllocator&&) = delete;
    
    // Allocate a block of the specified size
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        // Ensure minimum block size and proper alignment
        const size_t headerSize = sizeof(BlockHeader);
        const size_t minBlockSize = sizeof(FreeBlock);
        const size_t alignedSize = std::max(size, minBlockSize - headerSize);
        
        // Search for a suitable free block
        FreeBlock* prev = nullptr;
        FreeBlock* current = firstFreeBlock_;
        
        while (current != nullptr) {
            // Calculate aligned address for user data
            char* blockStart = reinterpret_cast<char*>(current);
            char* userData = blockStart + headerSize;
            
            // Align the user data pointer
            size_t space = current->size - headerSize;
            void* alignedData = userData;
            if (!std::align(alignment, alignedSize, alignedData, space)) {
                // Move to next block if alignment isn't possible
                prev = current;
                current = current->next;
                continue;
            }
            
            // Calculate padding needed for alignment
            size_t padding = static_cast<char*>(alignedData) - userData;
            size_t totalSize = headerSize + padding + alignedSize;
            
            // Check if block is large enough
            if (current->size < totalSize) {
                prev = current;
                current = current->next;
                continue;
            }
            
            // Calculate remaining size after allocation
            size_t remainingSize = current->size - totalSize;
            
            // If remaining size is large enough, split the block
            if (remainingSize >= minBlockSize) {
                // Create new free block from remaining space
                FreeBlock* newBlock = reinterpret_cast<FreeBlock*>(
                    blockStart + totalSize);
                newBlock->size = remainingSize;
                newBlock->next = current->next;
                
                // Update free list
                if (prev == nullptr) {
                    firstFreeBlock_ = newBlock;
                } else {
                    prev->next = newBlock;
                }
            } else {
                // Use the entire block
                totalSize = current->size;
                
                // Remove current block from free list
                if (prev == nullptr) {
                    firstFreeBlock_ = current->next;
                } else {
                    prev->next = current->next;
                }
            }
            
            // Initialize block header
            BlockHeader* header = reinterpret_cast<BlockHeader*>(
                static_cast<char*>(alignedData) - headerSize);
            header->size = totalSize;
            header->padding = padding;
            
            // Return aligned user data pointer
            return alignedData;
        }
        
        // No suitable block found
        return nullptr;
    }
    
    // Free a previously allocated block
    void deallocate(void* ptr) {
        if (ptr == nullptr) return;
        
        // Get block header
        BlockHeader* header = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(ptr) - sizeof(BlockHeader));
        
        // Convert the entire block to a free block
        FreeBlock* block = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<char*>(header) - header->padding);
        block->size = header->size;
        
        // Add block to the beginning of the free list (simple approach)
        block->next = firstFreeBlock_;
        firstFreeBlock_ = block;
        
        // Note: This simple implementation doesn't merge adjacent free blocks
        // which will lead to fragmentation over time
    }
    
    // Get the size of the memory pool
    size_t poolSize() const {
        return poolSize_;
    }
    
    // For debugging: print the state of the free list
    void debugPrintFreeList() const {
        std::cout << "Free blocks:" << std::endl;
        FreeBlock* current = firstFreeBlock_;
        size_t blockCount = 0;
        size_t totalFree = 0;
        
        while (current != nullptr) {
            std::cout << "  Block " << blockCount++ 
                      << " - Address: " << current 
                      << " Size: " << current->size << std::endl;
            totalFree += current->size;
            current = current->next;
        }
        
        std::cout << "Total free: " << totalFree << " / " << poolSize_ 
                  << " (" << (totalFree * 100.0 / poolSize_) << "%)" << std::endl;
    }

private:
    // Header for allocated blocks
    struct BlockHeader {
        size_t size;     // Total size of this block including header and padding
        size_t padding;  // Padding before header for alignment
    };
    
    // Structure for free blocks
    struct FreeBlock {
        size_t size;     // Total size of this free block
        FreeBlock* next; // Next free block in the list
    };
    
    char* pool_;              // Pointer to the memory pool
    size_t poolSize_;         // Size of the memory pool
    FreeBlock* firstFreeBlock_; // Start of the free list
};

} // namespace memory

#endif // MEMORY_ALLOCATOR_H