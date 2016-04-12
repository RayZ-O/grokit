//
//  Copyright 2012 Alin Dobra and Christopher Jermaine
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
#ifndef _NUMA_MMAP_ALLOC_H_
#define _NUMA_MMAP_ALLOC_H_

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

#include "MmapAllocator.h"
// Below 3 headers need for constant used for defining fixed hash size HASH_SEG_SIZE
#include "HashTableMacros.h"
#include "Constants.h"
#include "HashEntry.h"

/* This enables to store header information in chunk itself. If not set, header
   info will be stored elsewhere. But it turns out, we can not use this flag
   because it will give us pointers which are not aligned to the page boundary
   and hence disk writing will fail.
   */
//#define STORE_HEADER_IN_CHUNK 1

// For debug purpose
#define ASSERT_ON 1

#ifndef ASSERT
#ifdef ASSERT_ON
#define ASSERT(expr) assert(expr)
#else
#define ASSERT(expr)
#endif
#endif

/** This header specifies the interface of the class NumaMemoryAllocator
 * and the implementation of mmap_alloc and mmap_free based on this class.

 * The class behaves like a singleton to ensure a single global allocator.

 * The strategy used is the following:
 * 1. We initialize the heap for all numa nodes
 * 2. We serve requests from allocated heap (smaller chunks are carved out
 *    from biggest available chunk) if requested chunk is not exact match in freelist
 * 3. We maintain a map from sizes (multiple of pages) to free lists per numa node
 * 5. When chunk is freed, it is immediately coalesced with neighbouring chunks to reduce fragmentation
 * 6. With each chunk, a header information is maintained which helps in coalescing of chunks
 *    If header information is maintained within chunk itself, super fast! because
 *    we dont need to lookup header chunk using pointer given from ptr->header map
 * 7. When requested chunk is not found in numa, we check other numa nodes
 * 8. When requested size is not found in freelist, we pick biggest chunk, it is
 *    no time operation as we take out last element of map (sorted by size) and
 *    if it has element bigger than requested size, it will have freelist too for sure.
 * 9. Coelasceing of chunks is just some assignment of pointers if header is stored in
 *    chunk itself.
 * 10. Splitting of chunks is also some assignment of pointers if header is stored in
 *     chunks. And if not stored within chunk, we have to pay little search penalty.
 * 11. Header is 40 bytes long (void*) aligned.

 This allocator is thread safe.

*/

// Up to this number, no merging of adjacent chunks
#define NO_COALESCE_MAXPAGESIZE 16

// This is special size for hash segments and handled differently
#define HASH_SEG_SIZE (ABSOLUTE_HARD_CAP * sizeof(HashEntry))

// Touch the pages once retreived from mmap.
#define MMAP_TOUCH_PAGES 1

// Initial heap size for all NUMA nodes
#define INIT_HEAP_PAGE_SIZE 256*4

// Grow heap during run by this size if needed
#define HEAP_GROW_BY_SIZE 256*16

#ifdef GUNIT_TEST
#include <gtest/gtest.h>
#endif

class ChunkInfo;

class NumaMemoryAllocator {
#ifdef GUNIT_TEST
    // declare friend to test private member of allocator class
    friend class AllocatorTest;
#endif
    // mutex used to make the allocater thread safe
    std::mutex mtx_;
    // mark if the internal data structures are initialized
    bool is_initialized_;
    // number of allocated pages
    int allocated_pages_;
    // number of free pages
    int free_pages_;
    // page size of hash segment
    const int kHashSegPageSize;
    // page aligned hash segment size
    const size_t kHashSegAlignedSize;
    // reserve fixed size chunk for hash entry
    std::vector<void*> reserved_hash_segs;

    struct NumaNode{
        // binary search tree of free list
        std::map<int, std::unordered_set<void*>> free_tree;
    };
    // store the relation between numa number and numa nodes
    std::vector<NumaNode*> numa_num_to_node;
    // store chunk info in external data structure to avoid breaking DMA
    std::unordered_set<void*> occupied_hash_segs;
    std::unordered_map<void*, ChunkInfo*> ptr_to_bstchunk;

    size_t PageSizeToBytes(int page_size);

    int BytesToPageSize(size_t bytes);
    // get pointer that point to num_pages(convert to bytes) behind ptr
    void* PtrSeek(void* ptr, int num_pages);
    // erase pointer in the given size set in free tree
    void EraseTreePtr(int size, void* ptr, int node);
    // update number of allocated pages and free pages
    void UpdateStatus(int allocated_size);

    void GrowHeap(int num_pages, int node);

    void HeapInit();

    void* HashSegAlloc();

    void* BSTreeAlloc(int num_pages, int node);

    void BSTreeFree(void* ptr);

public:
    NumaMemoryAllocator(void);

    NumaMemoryAllocator(const NumaMemoryAllocator& rhs) = delete;

    NumaMemoryAllocator& operator =(const NumaMemoryAllocator& rhs) = delete;

    ~NumaMemoryAllocator(void);

    static NumaMemoryAllocator& GetAllocator(void);

    void* MmapAlloc(size_t noBytes, int node, const char* f, int l);

    void MmapChangeProt(void* ptr, int prot);

    void MmapFree(void* ptr);

    size_t AllocatedPages() const;

    size_t FreePages() const;
};

class ChunkInfo {
public:
    void* mem_ptr;
    int size;
    int node;  // numa node number
    bool used;
    ChunkInfo* prev; // pointer to previous physical chunk
    ChunkInfo* next; // pointer to next physical chunk

    ChunkInfo(void* ptr, int s, int nd, bool u, ChunkInfo* p, ChunkInfo* n);
    // deletes cpoy constructor
    ChunkInfo(const ChunkInfo& other) = delete;
    // deletes copy assignment operator
    ChunkInfo& operator = (const ChunkInfo& other) = delete;
    // assigns values to the chunk
    void Assign(void* ptr, int s, int nd, bool u, ChunkInfo* p, ChunkInfo* n);
    // splits current chunk into two chunks of new_size and size - new_size
    ChunkInfo* Split(int new_size);
    // gets pointer that point to num_pages(convert to bytes) behind ptr
    void* PtrSeek(void* ptr, int num_pages);
    // coalesces with the previous chunk
    ChunkInfo* CoalescePrev();
    // coalesces with the next chunk
    ChunkInfo* CoalesceNext();
    // gets chunk from chunk pool
    static ChunkInfo* GetChunk(void* ptr, int s, int node, bool u, ChunkInfo* p, ChunkInfo* n);
    // puts tree chunk back to chunk pool
    static void PutChunk(ChunkInfo* chunk);
    // free all cached free chunks
    static void FreeChunks();

#ifdef GUNIT_TEST
    // override ostream operator for pretty print
    friend std::ostream& operator <<(std::ostream &output, ChunkInfo &chunk);
#endif

private:
    static std::vector<ChunkInfo*> bstchunk_pool;  // chunk pool
};


// To avoid static initialization order fiasco. This is needed only if our allocator
// is used to initialize some global or static objects, where the ordering of
// initialization is undefined. This will help to fix such issues. Besides, if we know
// we don't have any such usage, static object can be defined outside this function
// and can be used directly. But to be safe, it's good this way
inline
NumaMemoryAllocator& NumaMemoryAllocator::GetAllocator(void){
    static NumaMemoryAllocator* singleton = new NumaMemoryAllocator();
    return *singleton;
}

#endif
