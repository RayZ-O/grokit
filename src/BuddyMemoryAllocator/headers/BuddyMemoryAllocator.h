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
#include <set>
#include <list>
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
// Use buddy allocation when the request under threshold, otherwise use binary search tree
#define BUDDY_PAGE_SIZE 1 << MAX_ORDER


class BuddyMemoryAllocator {
private:
    std::mutex mtx;

    struct PageDescriptor {
        int page_index;
        int order;
        PageDescriptor(int idx, int o) : page_index(idx), order(o) { }
    };
    // buddy system chunk
    struct BuddyChunk {
        void* mem_ptr;
        int size;
        PageDescriptor* pd;
    };
    // binary search tree chunk
    struct BSTreeChunk {
        void* mem_ptr;
        int size;
        bool used;
        BSTreeChunk* prev; // pointer to previous physical chunk
        BSTreeChunk* next;
    };

    char* buddy_base;

    std::vector<std::list<PageDescriptor*> free_area;
    std::multimap<int, void*> free_tree;

    // not store info in chuck
    std::unordered_map<void*, BuddyChunk*> ptr_to_budchunk;
    std::unordered_map<void*, BSTreeChunk*> ptr_to_bstchunk;

    int BytesToPageSize(size_t bytes);

    size_t PageSizeToBytes(int pSize);

    int BuddyBlockSize(int order);

    void HeapInit();

    void* BuddyAlloc(int noPages, int node);

    void* BSTreeAlloc(int noPages, int node);

    size_t AllocatedPages();

    size_t FreePages();

public:
    BuddyMemoryAllocator(void);

    BuddyMemoryAllocator(const BuddyMemoryAllocator& rhs) = delete;

    BuddyMemoryAllocator& operator =(const BuddyMemoryAllocator& rhs) = delete;

    ~BuddyMemoryAllocator(void);

    void* MmapAlloc(size_t noBytes, int node, const char* f, int l);

    void MmapChangeProt(void* ptr, int prot);

    void MmapFree(void* ptr);
};


// To avoid static initialization order fiasco. This is needed only if our allocator
// is used to initialize some global or static objects, where the ordering of
// initialization is undefined. This will help to fix such issues. Besides, if we know
// we don't have any such usage, static object can be defined outside this function
// and can be used directly. But to be safe, it's good this way
inline
BuddyMemoryAllocator& GetAllocator(void){
    static BuddyMemoryAllocator* singleton = new BuddyMemoryAllocator();
    return *singleton;
}

#endif