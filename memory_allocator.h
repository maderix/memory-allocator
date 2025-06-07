#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include <cstddef>     // size_t
#include <cstdint>     // uint32_t
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <condition_variable>
#include <chrono>

//-------------------------------------------------------
// 1) Stats
//-------------------------------------------------------
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
        AllocStatsSnapshot snap;
        snap.totalAllocCalls  = totalAllocCalls.load();
        snap.totalFreeCalls   = totalFreeCalls.load();
        snap.currentUsedBytes = currentUsedBytes.load();
        snap.peakUsedBytes    = peakUsedBytes.load();
        return snap;
    }
};

//-------------------------------------------------------
// 2) Thread-Local small-block cache
//    With multiple bins for up to 256B
//-------------------------------------------------------
static constexpr int SMALL_BIN_COUNT = 4;
static constexpr size_t SMALL_BIN_SIZE[SMALL_BIN_COUNT] = {32, 64, 128, 256};

struct SmallBlockHeader {
    size_t binIndex;
    size_t userSize;
};
struct SmallFreeBlock {
    SmallBlockHeader hdr;
    SmallFreeBlock* next;
};

class ThreadLocalSmallCache {
public:
    ThreadLocalSmallCache() {
        for (int i=0; i<SMALL_BIN_COUNT; i++){
            freeList_[i] = nullptr;
        }
    }
    ~ThreadLocalSmallCache() {}

    // find bin index for a requested size
    int findBin(size_t size) {
        for(int i=0; i<SMALL_BIN_COUNT; i++){
            if(size <= SMALL_BIN_SIZE[i]) return i;
        }
        return -1; 
    }

    void* allocateSmall(size_t reqSize, AllocStats& stats) {
        int bin=findBin(reqSize);
        if(bin<0) return nullptr; // not small
        auto*& head = freeList_[bin];
        if(head){
            // pop
            auto* blk = head;
            head = blk->next;
            // fill userSize
            blk->hdr.userSize = reqSize;
            // stats not updated here, but if you want every small
            // chunk to be a 'system call' you could. We'll skip for performance
            return reinterpret_cast<char*>(blk) + sizeof(SmallBlockHeader);
        }
        // no free => new chunk from system
        size_t totalSz = sizeof(SmallBlockHeader)+SMALL_BIN_SIZE[bin];
        char* block = (char*)::operator new(totalSz);
        std::memset(block, 0, totalSz);

        auto* freeB = reinterpret_cast<SmallFreeBlock*>(block);
        freeB->hdr.binIndex= bin;
        freeB->hdr.userSize= reqSize;

        // increment stats for the new chunk from system
        stats.totalAllocCalls.fetch_add(1);
        stats.currentUsedBytes.fetch_add(totalSz);
        auto c = stats.currentUsedBytes.load();
        auto p = stats.peakUsedBytes.load();
        while(c>p){
            if(stats.peakUsedBytes.compare_exchange_weak(p,c)) break;
        }

        return block + sizeof(SmallBlockHeader);
    }

    void freeSmall(void* userPtr, AllocStats& stats) {
        if(!userPtr) return;
        char* blockStart=(char*)userPtr - sizeof(SmallBlockHeader);
        auto* fb = reinterpret_cast<SmallFreeBlock*>(blockStart);
        int bin = (int)fb->hdr.binIndex;
        if(bin<0 || bin>=SMALL_BIN_COUNT) {
            // invalid
            return;
        }
        size_t totalSz = sizeof(SmallBlockHeader)+SMALL_BIN_SIZE[bin];
        stats.totalFreeCalls.fetch_add(1);
        stats.currentUsedBytes.fetch_sub(totalSz);

        // push front
        fb->next = freeList_[bin];
        freeList_[bin] = fb;
    }

private:
    SmallFreeBlock* freeList_[SMALL_BIN_COUNT];
};

//-------------------------------------------------------
// 3) Arena for large blocks
//    immediate merges
//-------------------------------------------------------
class Arena {
public:
    static constexpr uint32_t MAGIC = 0xCAFEBABE;

    struct BlockHeader {
        uint32_t magic;
        size_t   totalSize;
        size_t   userSize; 
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

    Arena(size_t arenaSize)
        : arenaSize_(arenaSize), usedBytes_(0)
    {
        memory_ = (char*)::operator new(arenaSize_);
        std::memset(memory_,0,arenaSize_);

        auto* fb = reinterpret_cast<FreeBlock*>(memory_);
        fb->hdr.magic=MAGIC;
        fb->hdr.totalSize=arenaSize_;
        fb->hdr.userSize=0;
        fb->hdr.isFree=true;
        fb->next=nullptr;

        auto* foot = getFooter(&fb->hdr);
        foot->magic=MAGIC;
        foot->totalSize=arenaSize_;
        foot->isFree=true;

        firstFree_=fb;
    }
    ~Arena(){
        if(memory_){
            ::operator delete(memory_);
        }
    }

    size_t usedBytes() const { return usedBytes_.load(); }
    bool fullyFree() const { return usedBytes_.load()==0; }

    void destroy() {
        // unmap
        if(memory_) {
            ::operator delete(memory_);
            memory_=nullptr;
        }
    }

    void* allocate(size_t reqSize, size_t alignment, AllocStats& stats) {
        std::lock_guard<std::mutex> lock(mtx_);
        stats.totalAllocCalls.fetch_add(1);

        const size_t overhead=sizeof(BlockHeader)+sizeof(BlockFooter);
        FreeBlock* prev=nullptr;
        FreeBlock* cur=firstFree_;

        while(cur){
            if(cur->hdr.isFree && cur->hdr.totalSize>=(reqSize+overhead)){
                // alignment
                char* start=(char*)cur;
                char* userArea=start+sizeof(BlockHeader);
                size_t space=cur->hdr.totalSize - overhead;
                void* alignedPtr=userArea;
                if(std::align(alignment,reqSize,alignedPtr,space)){
                    size_t padding=(char*)alignedPtr - userArea;
                    size_t needed= overhead+padding+reqSize;
                    if(cur->hdr.totalSize>=needed){
                        if(!prev) firstFree_=cur->next;
                        else prev->next=cur->next;

                        size_t leftover=cur->hdr.totalSize-needed;
                        bool canSplit= leftover>= (sizeof(FreeBlock)+overhead);
                        if(canSplit){
                            char* leftoverAddr= start+needed;
                            auto* leftoverFB= (FreeBlock*) leftoverAddr;
                            leftoverFB->hdr.magic= MAGIC;
                            leftoverFB->hdr.totalSize= leftover;
                            leftoverFB->hdr.userSize=0;
                            leftoverFB->hdr.isFree=true;
                            leftoverFB->next= firstFree_;
                            firstFree_= leftoverFB;

                            auto* leftoverFoot=getFooter(&leftoverFB->hdr);
                            leftoverFoot->magic= MAGIC;
                            leftoverFoot->totalSize= leftover;
                            leftoverFoot->isFree=true;
                        } else {
                            needed=cur->hdr.totalSize;
                        }
                        // mark allocated
                        cur->hdr.isFree=false;
                        cur->hdr.userSize=reqSize;
                        cur->hdr.totalSize=needed;

                        auto* foot=getFooter(&cur->hdr);
                        foot->magic= MAGIC;
                        foot->totalSize= needed;
                        foot->isFree=false;

                        usedBytes_.fetch_add(needed);
                        stats.currentUsedBytes.fetch_add(needed);
                        auto c=stats.currentUsedBytes.load();
                        auto p=stats.peakUsedBytes.load();
                        while(c>p){
                            if(stats.peakUsedBytes.compare_exchange_weak(p,c)) break;
                        }

                        return start+sizeof(BlockHeader)+padding;
                    }
                }
            }
            prev=cur;
            cur=cur->next;
        }
        return nullptr; // fail
    }

    void deallocate(void* userPtr, AllocStats& stats){
        if(!userPtr) return;
        std::lock_guard<std::mutex> lock(mtx_);

        stats.totalFreeCalls.fetch_add(1);

        char* start=(char*)userPtr - sizeof(BlockHeader);
        auto* hdr=(BlockHeader*)start;
        if(hdr->magic!=MAGIC || hdr->isFree){
            return;
        }
        hdr->isFree=true;

        size_t sz=hdr->totalSize;
        usedBytes_.fetch_sub(sz);
        stats.currentUsedBytes.fetch_sub(sz);

        // insert in free list
        auto* fb=(FreeBlock*)hdr;
        fb->next= firstFree_;
        firstFree_= fb;

        // merges
        coalesceForward(fb);
        coalesceBackward(fb);
    }

    void coalesceAll(){
        std::lock_guard<std::mutex> lock(mtx_);
        // we do merges on free anyway
    }

private:
    BlockFooter* getFooter(BlockHeader* hdr){
        char* footAddr=(char*)hdr + hdr->totalSize - sizeof(BlockFooter);
        return (BlockFooter*)footAddr;
    }
    void removeFreeBlock(BlockHeader* h){
        FreeBlock* prev=nullptr;
        FreeBlock* cur=firstFree_;
        while(cur){
            if(&cur->hdr==h){
                if(!prev) firstFree_=cur->next;
                else prev->next=cur->next;
                cur->next=nullptr;
                return;
            }
            prev=cur;
            cur=cur->next;
        }
    }
    void coalesceForward(FreeBlock* blk){
        char* nxtAddr=(char*)blk + blk->hdr.totalSize;
        if(nxtAddr>=memory_+arenaSize_) return;
        auto* nxtHdr=(BlockHeader*)nxtAddr;
        if(nxtHdr->magic==MAGIC && nxtHdr->isFree){
            removeFreeBlock(nxtHdr);
            blk->hdr.totalSize+= nxtHdr->totalSize;
            auto* foot=getFooter(&blk->hdr);
            foot->magic= MAGIC;
            foot->totalSize= blk->hdr.totalSize;
            foot->isFree= true;
        }
    }
    void coalesceBackward(FreeBlock* blk){
        if((char*)blk== memory_) return;
        char* footAddr=(char*)blk - sizeof(BlockFooter);
        if(footAddr<memory_) return;
        auto* foot=(BlockFooter*)footAddr;
        if(foot->magic==MAGIC && foot->isFree){
            size_t prevSz= foot->totalSize;
            char* prevAddr=(char*)blk - prevSz;
            auto* prevHdr=(BlockHeader*)prevAddr;
            if(prevHdr->magic==MAGIC && prevHdr->isFree){
                removeFreeBlock(&blk->hdr);
                removeFreeBlock(prevHdr);
                prevHdr->totalSize+= blk->hdr.totalSize;
                auto* newFoot=getFooter(prevHdr);
                newFoot->magic= MAGIC;
                newFoot->totalSize= prevHdr->totalSize;
                newFoot->isFree= true;

                auto* fb=(FreeBlock*)prevHdr;
                fb->next= firstFree_;
                firstFree_= fb;
            }
        }
    }

    char* memory_;
    size_t arenaSize_;
    std::atomic<size_t> usedBytes_;
    FreeBlock* firstFree_;
    std::mutex mtx_;
};

//-------------------------------------------------------
// 4) ThreadLocalData
//-------------------------------------------------------
struct ThreadLocalData {
    Arena* arena;
    ThreadLocalSmallCache smallCache;
};

//-------------------------------------------------------
// 5) GlobalArenaManager with optional reclamation
//-------------------------------------------------------
class GlobalArenaManager {
public:
    GlobalArenaManager(bool enableReclamation)
        : stopThread_(false)
        , enableReclamation_(enableReclamation)
    {
        if(enableReclamation_){
            bgThread_ = std::thread([this]{ this->bgLoop(); });
        }
    }
    ~GlobalArenaManager(){
        {
            std::lock_guard<std::mutex> lk(mgrMutex_);
            stopThread_ = true;
        }
        cv_.notify_all();
        if(bgThread_.joinable()){
            bgThread_.join();
        }
        // clean up all arenas
        for(auto* a: arenas_){
            a->destroy();
            delete a;
        }
    }

    Arena* createArena(size_t arenaSize){
        std::lock_guard<std::mutex> lk(mgrMutex_);
        auto* a=new Arena(arenaSize);
        arenas_.push_back(a);
        return a;
    }

private:
    void bgLoop(){
        // runs every 1 second
        while(true){
            std::unique_lock<std::mutex> lk(mgrMutex_);
            cv_.wait_for(lk, std::chrono::seconds(1), [this]{return stopThread_;});
            if(stopThread_) break;
            if(!enableReclamation_) continue; 

            // pass
            for(size_t i=0; i<arenas_.size(); ){
                auto* ar= arenas_[i];
                ar->coalesceAll();
                if(ar->fullyFree()){
                    // reclaim
                    ar->destroy();
                    delete ar;
                    arenas_.erase(arenas_.begin()+i);
                } else {
                    i++;
                }
            }
        }
    }

    std::vector<Arena*> arenas_;
    bool stopThread_;
    bool enableReclamation_;
    std::thread bgThread_;
    std::mutex mgrMutex_;
    std::condition_variable cv_;
};

//-------------------------------------------------------
// 6) The main facade
//-------------------------------------------------------
class FancyPerThreadAllocator {
public:
    explicit FancyPerThreadAllocator(size_t defaultArenaSize, bool enableReclamation=false)
        : defaultArenaSize_(defaultArenaSize)
    {
        manager_ = std::make_shared<GlobalArenaManager>(enableReclamation);
    }

    AllocStatsSnapshot getStatsSnapshot() const {
        return stats_.snapshot();
    }

    void* allocate(size_t size) {
        if(size==0) size=1;
        auto* tld = getThreadData();
        // small path
        if(size <= 256){
            return tld->smallCache.allocateSmall(size, stats_);
        }
        // else large
        return tld->arena->allocate(size, alignof(std::max_align_t), stats_);
    }

    void deallocate(void* ptr) {
        if(!ptr) return;
        // read the last 4 bytes to see if magic
        char* p = (char*)ptr - 4;
        uint32_t mg = *(uint32_t*)p;
        auto* tld=getThreadData();
        if(mg==Arena::MAGIC){
            tld->arena->deallocate(ptr, stats_);
        } else {
            tld->smallCache.freeSmall(ptr, stats_);
        }
    }

private:
    static thread_local ThreadLocalData* tld_;

    ThreadLocalData* getThreadData() {
        if(!tld_){
            Arena* a = manager_->createArena(defaultArenaSize_);
            tld_ = new ThreadLocalData{a, ThreadLocalSmallCache()};
        }
        return tld_;
    }

    size_t defaultArenaSize_;
    std::shared_ptr<GlobalArenaManager> manager_;
    mutable AllocStats stats_;
};

thread_local ThreadLocalData* FancyPerThreadAllocator::tld_ = nullptr;

#endif // MEMORY_ALLOCATOR_H
