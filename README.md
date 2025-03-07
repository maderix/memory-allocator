# Custom High-Concurrency Arena Allocator

This project implements a high-performance, per-thread arena allocator with additional features designed for extreme concurrency and server workloads. It includes detailed comparisons against system `malloc/free` under various stress test scenarios.

## Overview

Our custom allocator is designed to deliver:

- **High concurrency** by providing per-thread arenas and local caches.
- **Efficient allocation and deallocation** through immediate coalescing and size-segregated free lists.
- **Optional reclamation**: A background thread can reclaim fully free arenas (i.e., return memory to the OS) to reduce peak memory usage in memory-constrained scenarios.
- **Multiple test scenarios**: Stress tests simulate both random ephemeral workloads and more targeted, real-world usage patterns.

## Key Concepts and Features

### Per-Thread Arenas

- **Thread-Local Data**: Each thread is assigned its own arena, minimizing lock contention.
- **Local Free Lists**: Allocations are managed independently per thread, which improves performance on multi-core systems.
- **Arenas with Boundary Tags**: Each arena manages a contiguous memory region with metadata (block headers and footers) to support immediate coalescing.

### Small-Block Caches

- **Multiple Bins**: We implement a thread-local small-block cache with multiple bins (e.g., for sizes 32, 64, 128, and 256 bytes). Freed blocks are stored in the appropriate bin.
- **Lock-Free Fast Path**: Small allocations (up to 256 bytes) are served directly from the thread-local cache without locking.
- **Metadata Storage**: Each small block stores a header (with the bin index and requested size) immediately preceding the user-accessible memory, ensuring correct deallocation.

### Immediate Coalescing

- **On-Free Merging**: When a block is freed from an arena, the allocator immediately checks adjacent blocks and merges them if free, reducing fragmentation.
- **Boundary-Tag Merging**: Both forward and backward coalescing are performed using the block’s header and footer information.

### Optional Arena Reclamation

- **Background Thread**: An optional background thread periodically scans all arenas.
- **Reclamation Policy**: If an arena becomes fully free (usage equals zero), the allocator can unmap the entire arena and return it to the operating system.
- **Configurable Behavior**: Reclamation can be enabled or disabled depending on your memory constraints and performance requirements. Disabling reclamation improves raw throughput, while enabling it helps reduce peak memory usage.

### Global Arena Manager

- **Central Management**: A global manager tracks all arenas and coordinates background reclamation.
- **Concurrency Control**: The manager uses fine-grained locking to ensure low overhead when allocating new arenas or reclaiming fully free ones.

## Test Scenarios

The project includes several test harnesses to compare our custom allocator against system `malloc/free`:

1. **Basic Concurrency Test**: A global pointer array simulates random allocate/free operations under multi-threaded conditions.
2. **HPC Ephemeral Scenario**: A ring-buffer-based test simulates targeted server workloads where allocations have a random time-to-live (TTL), mimicking ephemeral object lifetimes.
3. **Detailed Comparison**: Multiple tests run in sequence (basic concurrency, ephemeral scenario, large concurrency) to compare performance (elapsed time, alloc/free counts, peak usage) between:
   - System `malloc/free`
   - FancyPerThreadAllocator with reclamation **off**
   - FancyPerThreadAllocator with reclamation **on**

## Building and Running

### Requirements

- C++17 compiler (e.g., g++ 7+)
- POSIX threading support (`-pthread`)

### Compilation

A sample Makefile is provided. To compile:

```bash
make
./memory_allocator_test

example output:
./build/memory_allocator_test 

=== Compare System Malloc vs. Fancy(Off) vs. Fancy(On) under HPC ephemeral scenario ===
Threads= 512, Ops/Thread= 1000000, ringSize= 500000

-- System malloc/free --
Elapsed (us): 70847186

-- Fancy Per-Thread (Reclamation OFF) --
Elapsed (us): 27639829
Alloc calls : 352289103, Free calls: 216816746, Peak usage: 36315500019

-- Fancy Per-Thread (Reclamation ON) --
Elapsed (us): 41289834
Alloc calls : 352295903, Free calls: 216814382, Peak usage: 36793754057

All tests completed.