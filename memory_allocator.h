#ifndef ARENA_ALLOCATOR_H
#define ARENA_ALLOCATOR_H

#include <cstddef>   // size_t
#include <cstdint>   // uint32_t
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <memory>
#include <cstring>   // for memset
#include <algorithm> // for std::align

//---------------------------------------------------
// 1. Stats
//---------------------------------------------------
struct AllocStatsSnapshot {
    size_t totalAllocCalls;
    size_t totalFreeCalls;
    size_t currentUsedBytes;
    size_t peakUsedBytes;
};

struct AllocStats {
    std::atomic<size_t> totalAllocCalls{0};
    std::atomic<size_t> totalFreeCalls{0};
    std::atomic<size_t> currentUsedBytes{0};
    std::atomic<size_t> peakUsedBytes{0};

    AllocStatsSnapshot snapshot() const {
        AllocStatsSnapshot s;
        s.totalAllocCalls  = totalAllocCalls.load();
        s.totalFreeCalls   = totalFreeCalls.load();
        s.currentUsedBytes = currentUsedBytes.load();
        s.peakUsedBytes    = peakUsedBytes.load();
        return s;
    }
};

//---------------------------------------------------
// 2. Thread-local small-block cache
//    - multiple bins for 32,64,128,256
//---------------------------------------------------
static constexpr int SMALL_BIN_COUNT = 4;
static constexpr size_t SMALL_BIN_SIZE[SMALL_BIN_COUNT] = {32, 64, 128, 256};

// We'll store a small metadata in the chunk itself:
// [ binIndex (uint16_t) | userSize (uint16_t) | free-list pointer if freed ]
// For demonstration, we'll store them as "size_t" for simplicity.
struct SmallBlockHeader {
    size_t binIndex;   // which bin
    size_t userSize;   // actual requested size
    // Then the user pointer is => (this header) + sizeof(SmallBlockHeader)
};

struct SmallFreeBlock {
    SmallBlockHeader hdr;
    SmallFreeBlock* next;
};

// For each bin, we keep a singly linked list of free blocks
class ThreadLocalSmallCache {
public:
    ThreadLocalSmallCache() {
        for (int i=0; i<SMALL_BIN_COUNT; i++) {
            freeList_[i] = nullptr;
        }
    }
    ~ThreadLocalSmallCache() {}

    // get bin index for a request
    int findBin(size_t size) {
        for (int i=0; i<SMALL_BIN_COUNT; i++) {
            if (size <= SMALL_BIN_SIZE[i]) return i;
        }
        return -1; // not small
    }

    // Provide a chunk if available
    void* allocateSmall(size_t reqSize, AllocStats& stats) {
        int bin = findBin(reqSize);
        if (bin < 0) return nullptr; // not small

        // pop from free list if exist
        auto*& head = freeList_[bin];
        if (head) {
            auto* blk = head;
            head = blk->next;
            // fill the header info
            blk->hdr.binIndex = bin;
            blk->hdr.userSize = reqSize;
            // user pointer => after header
            return reinterpret_cast<char*>(blk) + sizeof(SmallBlockHeader);
        }

        // no free chunk in this bin => allocate a fresh chunk from system or a new approach
        // For demonstration, let's just create a new chunk from system
        size_t chunkSize = SMALL_BIN_SIZE[bin];
        // We store the header + user space
        size_t totalSize = sizeof(SmallBlockHeader) + chunkSize;
        char* block = (char*)::operator new(totalSize);
        ::memset(block, 0, totalSize);

        auto* sb = reinterpret_cast<SmallFreeBlock*>(block);
        sb->hdr.binIndex = bin;
        sb->hdr.userSize = reqSize;

        stats.totalAllocCalls.fetch_add(1); // treat it as an "allocation" from system
        stats.currentUsedBytes.fetch_add(totalSize);
        auto c = stats.currentUsedBytes.load();
        auto p = stats.peakUsedBytes.load();
        while (c>p) {
            if (stats.peakUsedBytes.compare_exchange_weak(p,c)) break;
        }

        return block + sizeof(SmallBlockHeader);
    }

    // Freed small block => push to bin
    void freeSmall(void* userPtr, AllocStats& stats) {
        if (!userPtr) return;
        // recover the header
        char* blockStart = (char*)userPtr - sizeof(SmallBlockHeader);
        auto* sb = reinterpret_cast<SmallFreeBlock*>(blockStart);
        int bin = sb->hdr.binIndex;
        if (bin<0 || bin>=SMALL_BIN_COUNT) {
            // invalid
            return;
        }
        size_t totalSize = sizeof(SmallBlockHeader) + SMALL_BIN_SIZE[bin];
        stats.totalFreeCalls.fetch_add(1);
        stats.currentUsedBytes.fetch_sub(totalSize);

        sb->next = freeList_[bin];
        freeList_[bin] = sb;
    }

private:
    SmallFreeBlock* freeList_[SMALL_BIN_COUNT];
};

//---------------------------------------------------
// 3. Arena for large blocks
//    We'll store a "BlockHeader" with totalSize, isFree
//    plus a separate "userSize" so we know how big user wanted
//---------------------------------------------------
class Arena {
public:
    static constexpr uint32_t MAGIC = 0xCAFEBABE;

    struct BlockHeader {
        uint32_t magic;
        size_t   totalSize; // including header+footer
        size_t   userSize;  // how many bytes user requested
        bool     isFree;
    };
    struct BlockFooter {
        uint32_t magic;
        size_t   totalSize;
        bool     isFree;
    };

    struct FreeBlock {
        BlockHeader hdr;
        FreeBlock* next;
    };

    Arena(size_t arenaSize) : arenaSize_(arenaSize) {
        memory_ = (char*)::operator new(arenaSize_);
        ::memset(memory_, 0, arenaSize_);

        // one big free block
        auto* fb = reinterpret_cast<FreeBlock*>(memory_);
        fb->hdr.magic     = MAGIC;
        fb->hdr.totalSize = arenaSize_;
        fb->hdr.userSize  = 0;      // 0 => no user?
        fb->hdr.isFree    = true;
        fb->next = nullptr;

        auto* foot = getFooter(&fb->hdr);
        foot->magic     = MAGIC;
        foot->totalSize = arenaSize_;
        foot->isFree    = true;

        firstFree_ = fb;
    }

    ~Arena() {
        ::operator delete(memory_);
    }

    // We'll store the user pointer => blockStart + sizeof(BlockHeader)
    // Then block->hdr.userSize = reqSize
    // totalSize is overhead + user
    void* allocate(size_t reqSize, size_t alignment, AllocStats& stats) {
        std::lock_guard<std::mutex> lock(mtx_);

        stats.totalAllocCalls.fetch_add(1);

        const size_t overhead = sizeof(BlockHeader) + sizeof(BlockFooter);

        FreeBlock* prev = nullptr;
        FreeBlock* cur  = firstFree_;

        while (cur) {
            if (cur->hdr.isFree && cur->hdr.totalSize >= (reqSize + overhead)) {
                // attempt alignment
                char* blockStart = reinterpret_cast<char*>(cur);
                char* userArea   = blockStart + sizeof(BlockHeader);
                size_t space     = cur->hdr.totalSize - overhead;
                void* alignedPtr = userArea;

                if (std::align(alignment, reqSize, alignedPtr, space)) {
                    size_t padding = (char*)alignedPtr - userArea;
                    size_t needed  = overhead + padding + reqSize;

                    if (cur->hdr.totalSize >= needed) {
                        // split or use entire
                        size_t leftover = cur->hdr.totalSize - needed;
                        bool canSplit   = leftover >= (sizeof(FreeBlock)+ overhead);
                        // remove from free list
                        if (!prev) firstFree_ = cur->next;
                        else       prev->next = cur->next;

                        if (canSplit) {
                            // create leftover
                            char* leftoverAddr = blockStart + needed;
                            auto* leftoverFB = reinterpret_cast<FreeBlock*>(leftoverAddr);
                            leftoverFB->hdr.magic     = MAGIC;
                            leftoverFB->hdr.totalSize = leftover;
                            leftoverFB->hdr.userSize  = 0;
                            leftoverFB->hdr.isFree    = true;
                            leftoverFB->next = firstFree_;
                            firstFree_ = leftoverFB;

                            auto* leftoverFoot = getFooter(&leftoverFB->hdr);
                            leftoverFoot->magic     = MAGIC;
                            leftoverFoot->totalSize = leftover;
                            leftoverFoot->isFree    = true;

                            needed = needed; 
                        } else {
                            needed = cur->hdr.totalSize;
                        }

                        // now mark cur allocated
                        cur->hdr.isFree    = false;
                        cur->hdr.userSize  = reqSize;
                        cur->hdr.totalSize = needed;

                        auto* foot = getFooter(&cur->hdr);
                        foot->magic     = MAGIC;
                        foot->totalSize = needed;
                        foot->isFree    = false;

                        stats.currentUsedBytes.fetch_add(needed);
                        auto c = stats.currentUsedBytes.load();
                        auto p = stats.peakUsedBytes.load();
                        while (c>p) {
                            if (stats.peakUsedBytes.compare_exchange_weak(p,c)) break;
                        }

                        // return user pointer
                        return blockStart + sizeof(BlockHeader) + padding;
                    }
                }
            }
            prev = cur;
            cur  = cur->next;
        }
        return nullptr; // fail
    }

    void deallocate(void* userPtr, AllocStats& stats) {
        if (!userPtr) return;
        std::lock_guard<std::mutex> lock(mtx_);

        // recover block
        char* blockStart = (char*)userPtr - sizeof(BlockHeader);
        auto* hdr = reinterpret_cast<BlockHeader*>(blockStart);
        if (hdr->magic != MAGIC || !hdr->isFree == false) {
            return; // invalid
        }
        hdr->isFree = true;

        stats.totalFreeCalls.fetch_add(1);
        stats.currentUsedBytes.fetch_sub(hdr->totalSize);

        // re-insert into free list
        auto* fb = reinterpret_cast<FreeBlock*>(hdr);
        fb->next = firstFree_;
        firstFree_ = fb;

        // immediate coalescing
        coalesceForward(fb);
        coalesceBackward(fb);
    }

private:
    // get footer
    BlockFooter* getFooter(BlockHeader* hdr) {
        char* footAddr = reinterpret_cast<char*>(hdr) + hdr->totalSize - sizeof(BlockFooter);
        return reinterpret_cast<BlockFooter*>(footAddr);
    }

    // coalesce forward
    void coalesceForward(FreeBlock* blk) {
        // next block is at (char*)blk + blk->hdr.totalSize
        char* nextAddr = (char*)blk + blk->hdr.totalSize;
        if (nextAddr >= (memory_ + arenaSize_)) return;
        auto* nxtHdr = reinterpret_cast<BlockHeader*>(nextAddr);
        if (nxtHdr->magic == MAGIC && nxtHdr->isFree) {
            // remove from free list
            removeFreeBlock(nxtHdr);
            // unify
            blk->hdr.totalSize += nxtHdr->totalSize;
            auto* foot = getFooter(&blk->hdr);
            foot->magic     = MAGIC;
            foot->totalSize = blk->hdr.totalSize;
            foot->isFree    = true;
        }
    }

    // coalesce backward
    void coalesceBackward(FreeBlock* blk) {
        // check if there's a block behind us
        if ((char*)blk == memory_) return; // no prior
        // the footer of the prev block is right before (char*)blk
        char* footAddr = (char*)blk - sizeof(BlockFooter);
        if (footAddr < memory_) return;
        auto* foot = reinterpret_cast<BlockFooter*>(footAddr);
        if (foot->magic == MAGIC && foot->isFree) {
            // the prev block is free
            size_t prevSize = foot->totalSize;
            char* prevHdrAddr = (char*)blk - prevSize;
            auto* prevHdr = reinterpret_cast<BlockHeader*>(prevHdrAddr);
            if (prevHdr->magic == MAGIC && prevHdr->isFree) {
                // remove 'blk' from free list
                removeFreeBlock(&blk->hdr);

                // unify
                prevHdr->totalSize += blk->hdr.totalSize;
                auto* newFoot = getFooter(prevHdr);
                newFoot->magic     = MAGIC;
                newFoot->totalSize = prevHdr->totalSize;
                newFoot->isFree    = true;
            }
        }
    }

    // remove a block from free list
    void removeFreeBlock(BlockHeader* h) {
        FreeBlock* prev=nullptr;
        FreeBlock* cur=firstFree_;
        while (cur) {
            if (&cur->hdr == h) {
                if (!prev) firstFree_=cur->next;
                else prev->next=cur->next;
                cur->next=nullptr;
                return;
            }
            prev=cur;
            cur=cur->next;
        }
    }

private:
    char* memory_;
    size_t arenaSize_;
    FreeBlock* firstFree_;
    std::mutex mtx_;
};

//---------------------------------------------------
// 4. The thread data
//---------------------------------------------------
struct ThreadLocalData {
    Arena* arena;
    ThreadLocalSmallCache smallCache;
};

//---------------------------------------------------
// 5. The fancy per-thread facade
//---------------------------------------------------
class FancyPerThreadAllocator {
public:
    explicit FancyPerThreadAllocator(size_t defaultArenaSize)
        : defaultArenaSize_(defaultArenaSize)
    {
        // create one global arena for demonstration
        globalArena_ = new Arena(defaultArenaSize_);
    }

    ~FancyPerThreadAllocator() {
        delete globalArena_;
    }

    AllocStatsSnapshot getStatsSnapshot() const {
        return stats_.snapshot();
    }

    // unified allocate
    void* allocate(size_t size) {
        if (size==0) size=1;

        // check thread local data
        auto* tld = getThreadData();

        // attempt small block
        if (size <= 256) {
            void* p = tld->smallCache.allocateSmall(size, stats_);
            if (p) return p;
        }
        // else large block
        return tld->arena->allocate(size, alignof(std::max_align_t), stats_);
    }

    // unified free
    void deallocate(void* userPtr) {
        if (!userPtr) return;
        // first read the size we stored
        // if it's a small block, we have a small header
        // if it's a large block, we have the arena's boundary

        // We do a tiny metadata approach: check the magic
        // We'll read a 32-bit magic from userPtr - 4 bytes. 
        // This is a quick hack to see if it's small or large
        char* p = (char*)userPtr - 4; 
        uint32_t mg = *(uint32_t*)p;

        auto* tld = getThreadData();
        if (mg == Arena::MAGIC) {
            // large block
            tld->arena->deallocate(userPtr, stats_);
        } else {
            // small block approach
            tld->smallCache.freeSmall(userPtr, stats_);
        }
    }

private:
    static thread_local ThreadLocalData* tld_;

    ThreadLocalData* getThreadData() {
        if (!tld_) {
            // create an arena for each thread
            auto* a = new Arena(defaultArenaSize_);
            tld_ = new ThreadLocalData{a, ThreadLocalSmallCache()};
        }
        return tld_;
    }

    size_t defaultArenaSize_;
    Arena* globalArena_; // example (not used heavily)

    mutable AllocStats stats_;
};

thread_local ThreadLocalData* FancyPerThreadAllocator::tld_ = nullptr;

#endif // ARENA_ALLOCATOR_H
