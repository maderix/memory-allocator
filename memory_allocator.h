#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include <cstddef>   // size_t
#include <cstdint>   // uint32_t
#include <cstring>   // memset
#include <iostream>  // debug prints
#include <random>
#include <memory>

#ifdef DEBUG
#  define DEBUG_LOG(msg) std::cerr << msg << std::endl
#else
#  define DEBUG_LOG(msg)
#endif

namespace memory {

/**
 * A simple block-based "BasicAllocator" with no coalescing.
 * Uses a MAGIC signature to skip invalid frees.
 */
class BasicAllocator {
public:
    static constexpr uint32_t MAGIC = 0xDEADC0DE;

    explicit BasicAllocator(size_t poolSize)
        : poolSize_(poolSize)
    {
        pool_ = static_cast<char*>(::operator new(poolSize_));
        // zero-fill the pool so pointers default to 0
        std::memset(pool_, 0, poolSize_);

        // one big free block
        firstFree_ = reinterpret_cast<FreeBlock*>(pool_);
        firstFree_->signature = MAGIC;
        firstFree_->size      = poolSize_;
        firstFree_->next      = nullptr;
    }

    ~BasicAllocator() {
        ::operator delete(pool_);
    }

    BasicAllocator(const BasicAllocator&) = delete;
    BasicAllocator& operator=(const BasicAllocator&) = delete;

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        const size_t hdrSize = sizeof(BlockHeader);
        const size_t minBlock= sizeof(FreeBlock);

        FreeBlock* prev = nullptr;
        FreeBlock* cur  = firstFree_;
        while (cur) {
            if (!isValidFreeBlock(cur)) {
                // skip corrupted
                DEBUG_LOG("BasicAllocator: skipping corrupted free block @" << (void*)cur);
                prev = cur;
                cur  = cur->next;
                continue;
            }
            // Attempt alignment
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
                if (cur->size >= totalNeed) {
                    // Enough space
                    size_t leftover = cur->size - totalNeed;
                    if (leftover >= minBlock) {
                        // split
                        auto* newBlk = reinterpret_cast<FreeBlock*>(
                            blockStart + totalNeed
                        );
                        newBlk->signature = MAGIC;
                        newBlk->size      = leftover;
                        newBlk->next      = cur->next;

                        if (!prev) firstFree_ = newBlk;
                        else       prev->next = newBlk;
                    } else {
                        // use entire block
                        totalNeed = cur->size;
                        if (!prev) firstFree_ = cur->next;
                        else       prev->next = cur->next;
                    }

                    // Build allocated block header
                    auto* hdr = reinterpret_cast<BlockHeader*>(
                        static_cast<char*>(alignedPtr) - hdrSize
                    );
                    hdr->signature = MAGIC;  // mark allocated
                    hdr->size      = totalNeed;
                    hdr->padding   = padding;
                    return alignedPtr;
                }
            }
            prev = cur;
            cur  = cur->next;
        }
        return nullptr;
    }

    void deallocate(void* ptr) {
        if (!ptr) return;

        // Reconstruct header
        auto* hdr = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(ptr) - sizeof(BlockHeader)
        );
        if (!isValidAllocatedHeader(hdr)) {
            DEBUG_LOG("BasicAllocator: Invalid block header in deallocate for ptr=" << ptr);
            return; // skip
        }
        // Convert to free
        auto* freeBlk = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<char*>(hdr) - hdr->padding
        );
        freeBlk->signature = MAGIC;
        freeBlk->size      = hdr->size;
        freeBlk->next      = firstFree_;
        firstFree_         = freeBlk;
    }

    void debugPrintFreeList() const {
#ifdef DEBUG
        std::cout << "BasicAllocator free list:\n";
        size_t total=0;
        int idx=0;
        auto* cur = firstFree_;
        while (cur) {
            if (isValidFreeBlock(cur)) {
                std::cout << "  FreeBlock " << idx++ 
                          << " @" << (void*)cur 
                          << " size=" << cur->size << "\n";
                total += cur->size;
            } else {
                std::cout << "  Corrupted block @" << (void*)cur << "\n";
            }
            cur = cur->next;
        }
        std::cout << "Total free: " << total 
                  << " / " << poolSize_
                  << " (" << (100.0*total/poolSize_) << "%)\n";
#endif
    }

    size_t poolSize() const { return poolSize_; }

private:
    struct BlockHeader {
        uint32_t signature; // must be MAGIC if allocated
        size_t   size;
        size_t   padding;
    };
    struct FreeBlock {
        uint32_t signature; // must be MAGIC if free
        size_t   size;
        FreeBlock* next;
    };

    bool isValidFreeBlock(const FreeBlock* fb) const {
        // **range check** first
        if (!inPoolRange(fb)) return false;
        // now read signature
        if (fb->signature != MAGIC) return false;
        if (fb->size > poolSize_)  return false;
        if (fb->size < sizeof(BlockHeader)) return false;
        return true;
    }
    bool isValidAllocatedHeader(const BlockHeader* hdr) const {
        if (!inPoolRange(hdr)) return false;
        if (hdr->signature != MAGIC) return false;
        if (hdr->size > poolSize_)   return false;
        if (hdr->size < sizeof(BlockHeader)) return false;
        return true;
    }
    bool inPoolRange(const void* p) const {
        auto* addr = reinterpret_cast<const char*>(p);
        return (addr >= pool_) && (addr < pool_ + poolSize_);
    }

    char*  pool_;
    size_t poolSize_;
    FreeBlock* firstFree_;
};

/**
 * A coalescing allocator using boundary tags, doubly-linked free list,
 * debug signatures, and range checks before pointer dereferences.
 */
class CoalescingAllocator {
public:
    static constexpr uint32_t MAGIC = 0xDEADC0DE;

    explicit CoalescingAllocator(size_t poolSize)
        : poolSize_(poolSize)
    {
        const size_t minSz = sizeof(FreeBlock) + sizeof(BlockFooter);
        if (poolSize_ < minSz) {
            poolSize_ = minSz;
        }
        pool_ = static_cast<char*>(::operator new(poolSize_));
        std::memset(pool_, 0, poolSize_); // zero fill

        // one big free block
        freeListHead_ = reinterpret_cast<FreeBlock*>(pool_);
        freeListHead_->header.signature = MAGIC;
        freeListHead_->header.size      = poolSize_;
        freeListHead_->header.padding   = 0;
        freeListHead_->header.isFree    = true;
        freeListHead_->prev = nullptr;
        freeListHead_->next = nullptr;

        auto* foot = getFooter(&freeListHead_->header);
        foot->signature = MAGIC;
        foot->size      = poolSize_;
        foot->isFree    = true;
    }

    ~CoalescingAllocator() {
        ::operator delete(pool_);
    }

    CoalescingAllocator(const CoalescingAllocator&) = delete;
    CoalescingAllocator& operator=(const CoalescingAllocator&) = delete;

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        if (size == 0) size = 1;
        const size_t overhead = sizeof(BlockHeader) + sizeof(BlockFooter);
        const size_t minBlock = sizeof(FreeBlock);

        FreeBlock* blk = freeListHead_;
        while (blk) {
            // read next pointer now, so we never do 'blk->next' on a corrupted block
            FreeBlock* nxt = blk->next;

            if (!isValidFreeBlock(blk)) {
                // skip or break
                DEBUG_LOG("CoalescingAllocator: skipping corrupted free block @" << (void*)blk);
                blk = nxt;
                continue;
            }
            size_t blkSize  = blk->header.size;
            char*  userArea = reinterpret_cast<char*>(blk) + sizeof(BlockHeader);
            size_t usable   = blkSize - overhead;

            void* alignedPtr= userArea;
            size_t space    = usable;
            if (std::align(alignment, size, alignedPtr, space)) {
                size_t padding = static_cast<char*>(alignedPtr) - userArea;
                size_t totalNeeded = overhead + padding + size;
                if (totalNeeded < minBlock) {
                    totalNeeded = minBlock;
                }
                if (blkSize >= totalNeeded) {
                    return useBlock(blk, size, padding, totalNeeded);
                }
            }
            blk = nxt;
        }
        return nullptr;
    }

    void deallocate(void* ptr) {
        if (!ptr) return;

        // range check
        if (!inPoolRange(ptr)) {
            DEBUG_LOG("CoalescingAllocator: pointer out of range in deallocate: " << ptr);
            return;
        }
        // reconstruct header
        auto* hdr = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(ptr) - sizeof(BlockHeader)
        );
        if (!isValidAllocatedHeader(hdr)) {
            DEBUG_LOG("Invalid block header in deallocate for ptr=" << ptr);
            return;
        }
        hdr->isFree = true;

        auto* foot = getFooter(hdr);
        if (!foot || foot->signature != MAGIC) {
            DEBUG_LOG("Invalid footer in deallocate");
            return;
        }
        foot->isFree = true;

        // convert to free block
        auto* block = reinterpret_cast<FreeBlock*>(hdr);
        insertBlockOrdered(block);
        coalesce(block);
    }

    size_t poolSize() const { return poolSize_; }

    void debugPrintFreeList() const {
#ifdef DEBUG
        std::cout << "CoalescingAllocator free list:\n";
        size_t total = 0;
        int idx = 0;
        for (auto* b = freeListHead_; b; b = b->next) {
            if (isValidFreeBlock(b)) {
                std::cout << "  FreeBlock " << idx++ 
                          << " @" << (void*)b 
                          << " size=" << b->header.size << "\n";
                total += b->header.size;
            } else {
                std::cout << "  Corrupted block @" << (void*)b << "\n";
            }
        }
        std::cout << "Total free: " << total 
                  << " / " << poolSize_ 
                  << " (" << (100.0*total / poolSize_) << "%)\n";
#endif
    }

private:
    // -------------------------------------------------------------------------
    // Data Structures
    // -------------------------------------------------------------------------
    struct BlockHeader {
        uint32_t signature; // must be MAGIC
        size_t   size;      // total size (header + data + footer)
        size_t   padding;   // alignment
        bool     isFree;
    };

    struct BlockFooter {
        uint32_t signature; // must be MAGIC
        size_t   size;      // matches header.size
        bool     isFree;
    };

    struct FreeBlock {
        BlockHeader header;
        FreeBlock*  prev;
        FreeBlock*  next;
    };

    char*  pool_;
    size_t poolSize_;
    FreeBlock* freeListHead_;

    // -------------------------------------------------------------------------
    // Checking Routines
    // -------------------------------------------------------------------------
    bool inPoolRange(const void* p) const {
        auto* addr = reinterpret_cast<const char*>(p);
        return (addr >= pool_) && (addr < pool_ + poolSize_);
    }

    bool isValidFreeBlock(const FreeBlock* fb) const {
        // 1) range check
        if (!inPoolRange(fb)) return false;
        // 2) signature, etc.
        if (fb->header.signature != MAGIC) return false;
        if (!fb->header.isFree) return false;
        if (fb->header.size > poolSize_) return false;

        // check footer
        auto* foot = getFooter(&fb->header);
        if (!foot) return false;
        if (foot->signature != MAGIC) return false;
        if (!foot->isFree) return false;
        if (foot->size != fb->header.size) return false;
        return true;
    }

    bool isValidAllocatedHeader(const BlockHeader* hdr) const {
        if (!inPoolRange(hdr)) return false;
        if (hdr->signature != MAGIC) return false;
        if (hdr->isFree) return false; // allocated should be false
        if (hdr->size > poolSize_) return false;
        return true;
    }

    // -------------------------------------------------------------------------
    // Boundary Access
    // -------------------------------------------------------------------------
    BlockFooter* getFooter(const BlockHeader* h) const {
        if (!h || h->signature != MAGIC) return nullptr;
        char* fAddr = reinterpret_cast<char*>(const_cast<BlockHeader*>(h))
                      + h->size - sizeof(BlockFooter);
        if (fAddr < pool_ || fAddr >= pool_ + poolSize_) {
            return nullptr;
        }
        return reinterpret_cast<BlockFooter*>(fAddr);
    }

    BlockHeader* getNextHeader(const BlockHeader* h) const {
        if (!h || h->signature != MAGIC) return nullptr;
        char* nextAddr = reinterpret_cast<char*>(const_cast<BlockHeader*>(h)) + h->size;
        if (nextAddr >= pool_ + poolSize_) return nullptr;
        auto* nxt = reinterpret_cast<BlockHeader*>(nextAddr);
        if (nxt->signature != MAGIC) return nullptr;
        return nxt;
    }

    BlockHeader* getPrevHeader(const BlockHeader* h) const {
        if (!h || h->signature != MAGIC) return nullptr;
        const char* hdrAddr = reinterpret_cast<const char*>(h);
        if (hdrAddr <= pool_) return nullptr;

        char* footAddr = const_cast<char*>(hdrAddr) - sizeof(BlockFooter);
        if (footAddr < pool_) return nullptr;
        auto* foot = reinterpret_cast<BlockFooter*>(footAddr);
        if (foot->signature != MAGIC) return nullptr;

        if (foot->size > poolSize_) return nullptr;
        char* prevAddr = reinterpret_cast<char*>(foot) 
                         - (foot->size - sizeof(BlockFooter));
        if (prevAddr < pool_ || prevAddr >= pool_ + poolSize_) {
            return nullptr;
        }
        auto* prevHdr = reinterpret_cast<BlockHeader*>(prevAddr);
        if (prevHdr->signature != MAGIC) return nullptr;
        return prevHdr;
    }

    // -------------------------------------------------------------------------
    // Allocation Helper: remove block, optionally split leftover
    // -------------------------------------------------------------------------
    void* useBlock(FreeBlock* block, size_t userSize, size_t padding, size_t totalNeeded) {
        removeFromList(block);

        size_t oldSize = block->header.size;
        block->header.isFree  = false;
        block->header.padding = padding;

        auto* usedFoot = getFooter(&block->header);
        if (!usedFoot) {
            DEBUG_LOG("useBlock: invalid usedFooter");
            return nullptr;
        }

        size_t leftover = oldSize - totalNeeded;
        if (leftover >= (sizeof(FreeBlock)+sizeof(BlockFooter))) {
            // create leftover
            char* leftoverAddr = reinterpret_cast<char*>(block) + totalNeeded;
            auto* leftoverBlk  = reinterpret_cast<FreeBlock*>(leftoverAddr);

            leftoverBlk->header.signature = MAGIC;
            leftoverBlk->header.size      = leftover;
            leftoverBlk->header.padding   = 0;
            leftoverBlk->header.isFree    = true;
            leftoverBlk->prev = leftoverBlk->next = nullptr;

            auto* leftoverFoot = getFooter(&leftoverBlk->header);
            if (leftoverFoot) {
                leftoverFoot->signature = MAGIC;
                leftoverFoot->size      = leftover;
                leftoverFoot->isFree    = true;
            } else {
                DEBUG_LOG("useBlock: leftover foot invalid");
                leftover = 0;
            }
            if (leftover > 0) {
                insertBlockOrdered(leftoverBlk);
                block->header.size = totalNeeded;
                usedFoot = getFooter(&block->header);
                if (usedFoot) {
                    usedFoot->signature = MAGIC;
                    usedFoot->size      = totalNeeded;
                    usedFoot->isFree    = false;
                }
            } else {
                usedFoot->signature = MAGIC;
                usedFoot->size      = block->header.size;
                usedFoot->isFree    = false;
            }
        } else {
            // no leftover
            usedFoot->signature = MAGIC;
            usedFoot->size      = block->header.size;
            usedFoot->isFree    = false;
        }

        block->prev = block->next = nullptr;

        // Return user data pointer
        char* start = reinterpret_cast<char*>(block);
        return start + sizeof(BlockHeader) + padding;
    }

    // -------------------------------------------------------------------------
    // Coalescing
    // -------------------------------------------------------------------------
    void coalesce(FreeBlock* block) {
        if (!isValidFreeBlock(block)) return;

        // forward
        auto* nxtHdr = getNextHeader(&block->header);
        if (nxtHdr && nxtHdr->isFree) {
            auto* nxtBlk = reinterpret_cast<FreeBlock*>(nxtHdr);
            if (isValidFreeBlock(nxtBlk)) {
                removeFromList(nxtBlk);
                block->header.size += nxtHdr->size;
                auto* foot = getFooter(&block->header);
                if (foot) {
                    foot->signature = MAGIC;
                    foot->size      = block->header.size;
                    foot->isFree    = true;
                }
            }
        }
        // backward
        auto* prevHdr = getPrevHeader(&block->header);
        if (prevHdr && prevHdr->isFree) {
            auto* prevBlk = reinterpret_cast<FreeBlock*>(prevHdr);
            if (isValidFreeBlock(prevBlk)) {
                removeFromList(prevBlk);
                prevBlk->header.size += block->header.size;
                auto* foot = getFooter(&prevBlk->header);
                if (foot) {
                    foot->signature = MAGIC;
                    foot->size      = prevBlk->header.size;
                    foot->isFree    = true;
                }
                block = prevBlk;
            }
        }
        removeFromList(block);
        insertBlockOrdered(block);
    }

    // -------------------------------------------------------------------------
    // Free List
    // -------------------------------------------------------------------------
    void removeFromList(FreeBlock* b) {
        // if pointer or block is not valid, do nothing
        if (!b || !b->header.isFree) return;
        // if b is head
        if (b == freeListHead_) {
            freeListHead_ = b->next;
            if (freeListHead_) {
                freeListHead_->prev = nullptr;
            }
            b->prev = b->next = nullptr;
            return;
        }
        // else
        auto* p = b->prev;
        auto* n = b->next;
        if (p) p->next = n;
        if (n) n->prev = p;
        b->prev = b->next = nullptr;
    }

    void insertBlockOrdered(FreeBlock* b) {
        if (!b) return;
        // must be free, signature = MAGIC
        if (!b->header.isFree || b->header.signature != MAGIC) {
            DEBUG_LOG("insertBlockOrdered: block invalid or not free");
            return;
        }
        // if empty list
        if (!freeListHead_) {
            freeListHead_ = b;
            b->prev = b->next = nullptr;
            return;
        }
        // if b < freeListHead_
        if (b < freeListHead_) {
            b->next = freeListHead_;
            b->prev = nullptr;
            freeListHead_->prev = b;
            freeListHead_ = b;
            return;
        }
        // else find insertion
        auto* cur = freeListHead_;
        while (cur->next && cur->next < b) {
            cur = cur->next;
        }
        b->next = cur->next;
        b->prev = cur;
        if (cur->next) cur->next->prev = b;
        cur->next = b;
    }
};

} // namespace memory

#endif // MEMORY_ALLOCATOR_H
