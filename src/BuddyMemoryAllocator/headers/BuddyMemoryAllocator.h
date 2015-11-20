//
//  Copyright 2015 Rui Zhang, 2012 Alin Dobra and Christopher Jermaine
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
#ifndef _BUDDY_MMAP_ALLOC_H_
#define _BUDDY_MMAP_ALLOC_H_

#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <list>
#include <vector>
#include <mutex>

#include <gtest/gtest.h>

#include "MmapAllocator.h"
#include "BSTChunk.h"
#include "BuddyChunk.h"
#include "AllocatorUtil.h"
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

// This is special size for hash segments and handled differently
#define HASH_SEG_SIZE (ABSOLUTE_HARD_CAP * sizeof(HashEntry))

// Touch the pages once retreived from mmap.
#define MMAP_TOUCH_PAGES 1

// Initial heap size for all NUMA nodes
#define INIT_HEAP_PAGE_SIZE 256*4

// Grow heap during run by this size if needed
#define HEAP_GROW_BY_SIZE 256*16

// Maximum order in the buddy system
#define MAX_ORDER 10

class BuddyMemoryAllocator {

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
    // STYLE google code style constant naming
    // page size of hash segment
    const int kHashSegPageSize;
    // page aligned hash segment size
    const size_t kHashSegAlignedSize;
    // Use buddy allocation when the request under threshold, otherwise use binary search tree
    const int kBuddyHeapSize;
    // buddy bin size look up table
    std::vector<int> buddy_bin_size_table;
    // point to the beginning of buddy memory
    void* buddy_base;
    // reserve fixed size chunk for hash entry
    std::vector<void*> reserved_hash_segs;
    // array of free list in buddy system
    // TODO vector + reserve may be faster than list
    std::vector<std::list<int>> free_area;
    // binary search tree of free list
    std::map<int, std::unordered_set<void*>> free_tree;
    // store chunk info in external data structure to avoid breaking DMA
    std::unordered_set<void*> occupied_hash_segs;
    std::unordered_map<void*, BuddyChunk*> ptr_to_budchunk;
    std::unordered_map<void*, BSTreeChunk*> ptr_to_bstchunk;

    int GetOrder(int page_size);
    // erase pointer in the given size set in free tree
    void EraseTreePtr(int size, void* ptr);
    // update number of allocated pages and free pages
    void UpdateStatus(int allocated_size);

    void HeapInit();

    void* HashSegAlloc();

    void* BuddyAlloc(int num_pages, int node);

    void* BSTreeAlloc(int num_pages, int node);

    void BuddyFree(void* ptr);

    void BSTreeFree(void* ptr);

public:
    BuddyMemoryAllocator(void);

    BuddyMemoryAllocator(const BuddyMemoryAllocator& rhs) = delete;

    BuddyMemoryAllocator& operator =(const BuddyMemoryAllocator& rhs) = delete;

    ~BuddyMemoryAllocator(void);

    static BuddyMemoryAllocator& GetAllocator(void);

    void* MmapAlloc(size_t noBytes, int node, const char* f, int l);

    void MmapChangeProt(void* ptr, int prot);

    void MmapFree(void* ptr);

    size_t AllocatedPages() const;

    size_t FreePages() const;
};


// To avoid static initialization order fiasco. This is needed only if our allocator
// is used to initialize some global or static objects, where the ordering of
// initialization is undefined. This will help to fix such issues. Besides, if we know
// we don't have any such usage, static object can be defined outside this function
// and can be used directly. But to be safe, it's good this way
inline
BuddyMemoryAllocator& BuddyMemoryAllocator::GetAllocator(void){
    static BuddyMemoryAllocator* singleton = new BuddyMemoryAllocator();
    return *singleton;
}

#endif
