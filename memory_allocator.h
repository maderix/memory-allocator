#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include <cstddef>    // size_t
#include <cstdint>    // uint32_t
#include <cstring>    // std::memset
#include <mutex>      // std::mutex, std::lock_guard
#include <memory>     // std::align
#include <algorithm>  // std::max

/**
 * A simple statistics struct for usage: calls, bytes, etc.
 */
struct AllocatorStats {
    size_t totalAllocCalls   = 0;  // how many times allocate() was called
    size_t totalDeallocCalls = 0;  // how many times deallocate() was called
    size_t currentUsedBytes  = 0;  // how many bytes are currently "allocated"
    size_t peakUsedBytes     = 0;  // the maximum value currentUsedBytes reached
};

/**
 * ThreadsafeBasicAllocator:
 *  - Single global std::mutex for entire free list (coarse-grained).
 *  - No coalescing, singly-linked free list.
 *  - Uses a signature to detect invalid frees in debug.
 *  - Provides basic usage stats (alloc calls, usage, etc.).
 */
class ThreadsafeBasicAllocator {
public:
    static constexpr uint32_t MAGIC = 0xDEADC0DE;

    explicit ThreadsafeBasicAllocator(size_t poolSize)
        : poolSize_(poolSize)
    {
        pool_ = static_cast<char*>(::operator new(poolSize_));
        std::memset(pool_, 0, poolSize_);

        // Create one large free block
        firstFree_ = reinterpret_cast<FreeBlock*>(pool_);
        firstFree_->signature = MAGIC;
        firstFree_->size      = poolSize_;
        firstFree_->next      = nullptr;
    }

    ~ThreadsafeBasicAllocator() {
        ::operator delete(pool_);
    }

    // no copy
    ThreadsafeBasicAllocator(const ThreadsafeBasicAllocator&) = delete;
    ThreadsafeBasicAllocator& operator=(const ThreadsafeBasicAllocator&) = delete;

    // Let external code retrieve usage stats
    AllocatorStats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    // -------------------------------
    // ALLOCATE
    // -------------------------------
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        if (size == 0) size = 1;

        std::lock_guard<std::mutex> lock(mutex_); // concurrency

        stats_.totalAllocCalls++;

        const size_t hdrSize  = sizeof(BlockHeader);
        const size_t minBlock = sizeof(FreeBlock);

        FreeBlock* prev = nullptr;
        FreeBlock* cur  = firstFree_;
        while (cur) {
            // skip if corrupted
            if (cur->signature != MAGIC || !inPool(cur)) {
                prev = cur;
                cur  = cur->next;
                continue;
            }
            char* blockStart = reinterpret_cast<char*>(cur);
            char* userPtr    = blockStart + hdrSize;
            size_t space     = (cur->size >= hdrSize) ? (cur->size - hdrSize) : 0;
            void* alignedPtr = userPtr;

            if (std::align(alignment, size, alignedPtr, space)) {
                size_t padding   = static_cast<char*>(alignedPtr) - userPtr;
                size_t totalNeed = hdrSize + padding + size;
                if (totalNeed < minBlock) {
                    totalNeed = minBlock;
                }
                // Check if cur->size is enough
                if (cur->size >= totalNeed) {
                    size_t leftover = cur->size - totalNeed;
                    bool canSplit   = false;

                    if (leftover >= minBlock) {
                        // leftover block must be within the pool
                        char* leftoverAddr = blockStart + totalNeed;
                        if (inPool(leftoverAddr) && inPool(leftoverAddr + leftover - 1)) {
                            canSplit = true;
                        }
                    }

                    // do the split or use entire
                    if (canSplit) {
                        auto* newBlk = reinterpret_cast<FreeBlock*>(
                            blockStart + totalNeed
                        );
                        newBlk->signature = MAGIC;
                        newBlk->size      = leftover;
                        newBlk->next      = cur->next;

                        if (!prev) firstFree_ = newBlk;
                        else       prev->next = newBlk;
                    } else {
                        totalNeed = cur->size;
                        if (!prev) firstFree_ = cur->next;
                        else       prev->next = cur->next;
                    }

                    // Mark allocated region
                    auto* hdr = reinterpret_cast<BlockHeader*>(
                        static_cast<char*>(alignedPtr) - hdrSize
                    );
                    hdr->signature = MAGIC;
                    hdr->size      = totalNeed;
                    hdr->padding   = padding;

                    // Stats
                    stats_.currentUsedBytes += totalNeed;
                    if (stats_.currentUsedBytes > stats_.peakUsedBytes) {
                        stats_.peakUsedBytes = stats_.currentUsedBytes;
                    }

                    return alignedPtr;
                }
            }
            prev = cur;
            cur  = cur->next;
        }
        return nullptr; // none found
    }

    // -------------------------------
    // DEALLOCATE
    // -------------------------------
    void deallocate(void* ptr) {
        if (!ptr) return;

        std::lock_guard<std::mutex> lock(mutex_); // concurrency

        stats_.totalDeallocCalls++;

        auto* hdr = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(ptr) - sizeof(BlockHeader)
        );
        if (hdr->signature != MAGIC || !inPool(hdr)) {
            // skip invalid
            return;
        }

        // stats
        stats_.currentUsedBytes -= hdr->size;

        // Turn it into a free block
        auto* freeBlk = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<char*>(hdr) - hdr->padding
        );
        freeBlk->signature = MAGIC;
        freeBlk->size      = hdr->size;

        // Insert at head
        freeBlk->next = firstFree_;
        firstFree_    = freeBlk;
    }

private:
    // check pointer within [pool_, pool_+poolSize_)
    bool inPool(const void* p) const {
        auto addr = reinterpret_cast<const char*>(p);
        return (addr >= pool_) && (addr < (pool_ + poolSize_));
    }

    struct BlockHeader {
        uint32_t signature;
        size_t   size;
        size_t   padding;
    };
    struct FreeBlock {
        uint32_t signature;
        size_t   size;
        FreeBlock* next;
    };

    char*  pool_;
    size_t poolSize_;
    FreeBlock* firstFree_;
    mutable std::mutex mutex_;

    AllocatorStats stats_;
};

// -----------------------------------------------------------------------------
// ThreadsafeCoalescingAllocator
// -----------------------------------------------------------------------------
class ThreadsafeCoalescingAllocator {
public:
    static constexpr uint32_t MAGIC = 0xDEADC0DE;

    explicit ThreadsafeCoalescingAllocator(size_t poolSize)
        : poolSize_(poolSize)
    {
        const size_t minSize = sizeof(FreeBlock) + sizeof(BlockFooter);
        if (poolSize_ < minSize) {
            poolSize_ = minSize;
        }
        pool_ = static_cast<char*>(::operator new(poolSize_));
        std::memset(pool_, 0, poolSize_);

        // one big free block
        freeListHead_ = reinterpret_cast<FreeBlock*>(pool_);
        freeListHead_->header.signature = MAGIC;
        freeListHead_->header.size      = poolSize_;
        freeListHead_->header.padding   = 0;
        freeListHead_->header.isFree    = true;

        // set up the footer
        BlockFooter* foot = getFooter(&(freeListHead_->header));
        if (foot) {
            foot->signature = MAGIC;
            foot->size      = poolSize_;
            foot->isFree    = true;
        }
        freeListHead_->prev = nullptr;
        freeListHead_->next = nullptr;
    }

    ~ThreadsafeCoalescingAllocator() {
        ::operator delete(pool_);
    }

    ThreadsafeCoalescingAllocator(const ThreadsafeCoalescingAllocator&) = delete;
    ThreadsafeCoalescingAllocator& operator=(const ThreadsafeCoalescingAllocator&) = delete;

    AllocatorStats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    // ALLOCATE
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        if (size == 0) size = 1;
        std::lock_guard<std::mutex> lock(mutex_);

        stats_.totalAllocCalls++;

        const size_t overhead = sizeof(BlockHeader) + sizeof(BlockFooter);
        const size_t minBlock = sizeof(FreeBlock);

        FreeBlock* blk = freeListHead_;
        while (blk) {
            FreeBlock* nxt = blk->next;
            if (!isValidFreeBlock(blk)) {
                blk = nxt;
                continue;
            }
            size_t blkSize  = blk->header.size;
            char* userArea  = reinterpret_cast<char*>(blk) + sizeof(BlockHeader);
            size_t usable   = (blkSize > overhead) ? (blkSize - overhead) : 0;

            void* alignedPtr = userArea;
            size_t space     = usable;
            if (std::align(alignment, size, alignedPtr, space)) {
                size_t padding    = static_cast<char*>(alignedPtr) - userArea;
                size_t totalNeeded= overhead + padding + size;
                if (totalNeeded < minBlock) totalNeeded = minBlock;
                if (blkSize >= totalNeeded) {
                    return useBlock(blk, totalNeeded);
                }
            }
            blk = nxt;
        }
        return nullptr;
    }

    // DEALLOCATE
    void deallocate(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(mutex_);

        stats_.totalDeallocCalls++;

        if (!inPool(ptr)) {
            return;
        }
        auto* hdr = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(ptr) - sizeof(BlockHeader)
        );
        if (!isValidAllocatedHeader(hdr)) {
            return;
        }
        hdr->isFree = true;

        // stats
        stats_.currentUsedBytes -= hdr->size;

        auto* foot = getFooter(hdr);
        if (!foot || foot->signature != MAGIC) {
            return;
        }
        foot->isFree = true;

        auto* freeBlk = reinterpret_cast<FreeBlock*>(hdr);
        insertBlockOrdered(freeBlk);
        coalesce(freeBlk);
    }

private:
    // data structures
    struct BlockHeader {
        uint32_t signature;
        size_t   size;     
        size_t   padding;
        bool     isFree;
    };
    struct BlockFooter {
        uint32_t signature;
        size_t   size;
        bool     isFree;
    };
    struct FreeBlock {
        BlockHeader header;
        FreeBlock*  prev;
        FreeBlock*  next;
    };

    // data members
    char*  pool_;
    size_t poolSize_;
    FreeBlock* freeListHead_;
    mutable std::mutex mutex_;
    AllocatorStats stats_;

    // utility
    bool inPool(const void* p) const {
        auto addr = reinterpret_cast<const char*>(p);
        return (addr >= pool_) && (addr < (pool_ + poolSize_));
    }

    // checks
    bool isValidFreeBlock(FreeBlock* fb) const {
        if (!inPool(fb)) return false;
        if (fb->header.signature != MAGIC) return false;
        if (!fb->header.isFree) return false;
        // check footer
        auto* foot = getFooter(&(fb->header));
        if (!foot || foot->signature != MAGIC) return false;
        if (!foot->isFree) return false;
        if (foot->size != fb->header.size) return false;
        return true;
    }
    bool isValidAllocatedHeader(BlockHeader* hdr) const {
        if (!inPool(hdr)) return false;
        if (hdr->signature != MAGIC) return false;
        if (hdr->isFree) return false;
        if (hdr->size > poolSize_) return false;
        return true;
    }

    // boundary
    BlockFooter* getFooter(BlockHeader* h) const {
        if (!h || h->signature != MAGIC) return nullptr;
        char* footAddr = reinterpret_cast<char*>(h) + h->size - sizeof(BlockFooter);
        if (!inPool(footAddr)) return nullptr;
        return reinterpret_cast<BlockFooter*>(footAddr);
    }

    BlockHeader* getNextHeader(BlockHeader* h) const {
        if (!h || (h->signature != MAGIC)) return nullptr;
        char* nextAddr = reinterpret_cast<char*>(h) + h->size;
        if (!inPool(nextAddr)) return nullptr;
        auto* nxt = reinterpret_cast<BlockHeader*>(nextAddr);
        if (nxt->signature != MAGIC) return nullptr;
        return nxt;
    }
    BlockHeader* getPrevHeader(BlockHeader* h) const {
        if (!h || (h->signature != MAGIC)) return nullptr;
        const char* hdrAddr = reinterpret_cast<const char*>(h);
        if (hdrAddr <= pool_) return nullptr;
        char* footAddr = const_cast<char*>(hdrAddr) - sizeof(BlockFooter);
        if (!inPool(footAddr)) return nullptr;

        auto* foot = reinterpret_cast<BlockFooter*>(footAddr);
        if (foot->signature != MAGIC) return nullptr;
        if (foot->size > poolSize_) return nullptr;

        char* prevAddr = reinterpret_cast<char*>(foot) - (foot->size - sizeof(BlockFooter));
        if (!inPool(prevAddr)) return nullptr;
        auto* prevHdr = reinterpret_cast<BlockHeader*>(prevAddr);
        if (prevHdr->signature != MAGIC) return nullptr;
        return prevHdr;
    }

    // useBlock
    void* useBlock(FreeBlock* blk, size_t totalNeeded) {
        removeFromList(blk);

        size_t oldSize = blk->header.size;
        blk->header.isFree = false;

        auto* usedFooter = getFooter(&(blk->header));
        if (!usedFooter) return nullptr;

        size_t leftover = (oldSize > totalNeeded) ? (oldSize - totalNeeded) : 0;
        if (leftover >= (sizeof(FreeBlock) + sizeof(BlockFooter))) {
            // carve leftover
            char* leftoverAddr = reinterpret_cast<char*>(blk) + totalNeeded;
            if (!inPool(leftoverAddr) || !inPool(leftoverAddr + leftover - 1)) {
                leftover = 0;
            }

            if (leftover > 0) {
                auto* leftoverBlk = reinterpret_cast<FreeBlock*>(leftoverAddr);
                leftoverBlk->header.signature = MAGIC;
                leftoverBlk->header.size      = leftover;
                leftoverBlk->header.padding   = 0;
                leftoverBlk->header.isFree    = true;

                auto* leftoverFoot = getFooter(&(leftoverBlk->header));
                if (!leftoverFoot) leftover = 0;
                else {
                    leftoverFoot->signature = MAGIC;
                    leftoverFoot->size      = leftover;
                    leftoverFoot->isFree    = true;
                }
                if (leftover > 0) {
                    leftoverBlk->prev = leftoverBlk->next = nullptr;
                    insertBlockOrdered(leftoverBlk);

                    blk->header.size = totalNeeded;
                    usedFooter = getFooter(&(blk->header));
                    if (usedFooter) {
                        usedFooter->signature = MAGIC;
                        usedFooter->size      = totalNeeded;
                        usedFooter->isFree    = false;
                    }
                } else {
                    usedFooter->signature = MAGIC;
                    usedFooter->size      = blk->header.size;
                    usedFooter->isFree    = false;
                }
            } else {
                usedFooter->signature = MAGIC;
                usedFooter->size      = blk->header.size;
                usedFooter->isFree    = false;
            }
        } else {
            usedFooter->signature = MAGIC;
            usedFooter->size      = blk->header.size;
            usedFooter->isFree    = false;
        }

        blk->prev = blk->next = nullptr;

        // stats
        stats_.currentUsedBytes += blk->header.size;
        if (stats_.currentUsedBytes > stats_.peakUsedBytes) {
            stats_.peakUsedBytes = stats_.currentUsedBytes;
        }

        // user pointer
        return reinterpret_cast<char*>(blk) + sizeof(BlockHeader) + blk->header.padding;
    }

    // coalesce
    void coalesce(FreeBlock* block) {
        if (!isValidFreeBlock(block)) return;

        // forward neighbor
        if (auto* nxtHdr = getNextHeader(&(block->header))) {
            if (nxtHdr->isFree) {
                auto* nxtBlk = reinterpret_cast<FreeBlock*>(nxtHdr);
                if (isValidFreeBlock(nxtBlk)) {
                    removeFromList(nxtBlk);
                    stats_.currentUsedBytes -= nxtHdr->size;

                    block->header.size += nxtHdr->size;
                    if (auto* foot = getFooter(&(block->header))) {
                        foot->signature = MAGIC;
                        foot->size      = block->header.size;
                        foot->isFree    = true;
                    }
                }
            }
        }

        // backward neighbor
        if (auto* prevHdr = getPrevHeader(&(block->header))) {
            if (prevHdr->isFree) {
                auto* prevBlk = reinterpret_cast<FreeBlock*>(prevHdr);
                if (isValidFreeBlock(prevBlk)) {
                    removeFromList(prevBlk);
                    stats_.currentUsedBytes -= block->header.size;

                    prevBlk->header.size += block->header.size;
                    if (auto* foot = getFooter(&(prevBlk->header))) {
                        foot->signature = MAGIC;
                        foot->size      = prevBlk->header.size;
                        foot->isFree    = true;
                    }
                    block = prevBlk;
                }
            }
        }
        insertBlockOrdered(block);
    }

    // free-list
    void removeFromList(FreeBlock* b) {
        if (!b || !b->header.isFree) return;
        if (b == freeListHead_) {
            freeListHead_ = b->next;
            if (freeListHead_) {
                freeListHead_->prev = nullptr;
            }
            b->prev = b->next = nullptr;
            return;
        }
        auto* p = b->prev;
        auto* n = b->next;
        if (p) p->next = n;
        if (n) n->prev = p;
        b->prev = b->next = nullptr;
    }
    void insertBlockOrdered(FreeBlock* b) {
        if (!b || !b->header.isFree || (b->header.signature != MAGIC)) return;
        if (!inPool(b)) return;

        if (!freeListHead_) {
            freeListHead_ = b;
            b->prev = b->next = nullptr;
            return;
        }
        if (b < freeListHead_) {
            b->next = freeListHead_;
            b->prev = nullptr;
            freeListHead_->prev = b;
            freeListHead_ = b;
            return;
        }
        FreeBlock* cur = freeListHead_;
        while (cur->next && (cur->next < b)) {
            if (!inPool(cur->next)) {
                break;
            }
            cur = cur->next;
        }
        b->next = cur->next;
        b->prev = cur;
        if (cur->next && inPool(cur->next)) {
            cur->next->prev = b;
        }
        cur->next = b;
    }
};

#endif // MEMORY_ALLOCATOR_H
