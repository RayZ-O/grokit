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
#include <sys/mman.h>
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "MmapAllocator.h"
#include "Errors.h"
#include "Numa.h"
#include "BuddyMemoryAllocator.h"

using namespace std;

void* mmap_alloc_imp(size_t noBytes, int node, const char* f, int l) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    void* rez = aloc.MmapAlloc(noBytes, node, f, l);
    return rez;
}

void mmap_prot_read_imp(void* ptr, const char* f, int l) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ);
}

void mmap_prot_readwrite_imp(void* ptr, const char* f, int l) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ | PROT_WRITE);
}

void mmap_free_imp(void* ptr, const char* f, int l) {
    if(!ptr) {
        WARNING("Warning: Attempted free of null pointer at %s:%d", f, l);
    }

    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    aloc.MmapFree(ptr);
}

off_t mmap_used(void) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    return PAGES_TO_BYTES(aloc.AllocatedPages());
}


BuddyMemoryAllocator::BuddyMemoryAllocator(void)
    : is_initialized_(false),  // google code stype constructor initializer lists
      allocated_pages_(0),
      free_pages_(0),
      kHashSegPageSize(BytesToPageSize(HASH_SEG_SIZE)),  // hash segment size in pages
      kHashSegAlignedSize(PageSizeToBytes(kHashSegPageSize)) // hash segment size in bytes
    {}

BuddyMemoryAllocator::~BuddyMemoryAllocator(void) {
    for (auto p : ptr_to_bstchunk) {
        SYS_MMAP_FREE(p.first, PageSizeToBytes(p.second->size));
    }
    for (auto s: reserved_hash_segs) {
        SYS_MMAP_FREE(s, kHashSegAlignedSize);
    }
    BSTreeChunk::FreeChunks();
}

void BuddyMemoryAllocator::HeapInit() {
    is_initialized_ = true;
    // if not define USE_NUMA, numaNodeCount() return 1
    int num_numa_nodes = numaNodeCount();
    unsigned long node_mask = 0;
    for (unsigned long long node = 0; node < num_numa_nodes; node++)
    {
        node_mask = 0;
        node_mask |= (1 << node);
        numa_num_to_node.emplace_back(new NumaNode);
        void *new_chunk = SYS_MMAP_ALLOC(PageSizeToBytes(INIT_HEAP_PAGE_SIZE));
        if (!SYS_MMAP_CHECK(new_chunk)) {
            perror("BuddyMemoryAllocator");
            FATAL("The memory allocator could not allocate memory");
        }
#ifdef USE_NUMA
#ifdef MMAP_TOUCH_PAGES
        // now bind it to the node and touch all the pages to make sure memory is bind to the node
        int retVal = mbind(new_chunk,                               // address
                           PageSizeToBytes(INIT_HEAP_PAGE_SIZE),    // length
                           MPOL_PREFERRED,                          // policy mode
                           &node_mask,                              // node mask
                           num_numa_nodes+1,                        // max number of nodes
                           MPOL_MF_MOVE);                           // policy mode flag
        ASSERT(retVal == 0);
        int* pInt = reinterpret_cast<int*>(new_chunk);
        for (unsigned int k = 0; k < PageSizeToBytes(INIT_HEAP_PAGE_SIZE)/4; k += (1 << (ALLOC_PAGE_SIZE_EXPONENT-2))) {
            pInt[k] = 0;
        }
#endif
#endif
        free_pages_ += INIT_HEAP_PAGE_SIZE;
        numa_num_to_node[node]->free_tree[INIT_HEAP_PAGE_SIZE].insert(new_chunk);

        BSTreeChunk* tree_chunk = BSTreeChunk::GetChunk(new_chunk, INIT_HEAP_PAGE_SIZE, node, false, nullptr, nullptr);
        ptr_to_bstchunk.emplace(new_chunk, tree_chunk);
    }
}

void* BuddyMemoryAllocator::MmapAlloc(size_t num_bytes, int node, const char* f, int l) {
    if (0 == num_bytes)
        return nullptr;

    lock_guard<mutex> lck(mtx_);

    if (!is_initialized_)
        HeapInit();

    int num_pages = BytesToPageSize(num_bytes);
    if (kHashSegPageSize == num_pages) {
        return HashSegAlloc();
    }
    void* res_ptr = BSTreeAlloc(num_pages, node);
#ifdef USE_NUMA
    if (!res_ptr) {
        // lookup other numa nodes
        for (int i = 0; i < numa_num_to_node.size(); i++) {
            if (i != node) {
                res_ptr = BSTreeAlloc(num_pages, i);
                if (res_ptr)
                    break;
            }
        }
    }
#endif
    if (!res_ptr) {
        GrowHeap(num_pages, node);
        res_ptr = BSTreeAlloc(num_pages, node);
    }
    return res_ptr;
}

void BuddyMemoryAllocator::MmapChangeProt(void* ptr, int prot) {
    if (!ptr) {
        return;
    }

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        SYS_MMAP_PROT(ptr, kHashSegAlignedSize, prot);
    } else {
        // find the size and insert the freed memory in the
        auto it = ptr_to_bstchunk.find(ptr);
        FATALIF(it == ptr_to_bstchunk.end(), "Changing the protection of unallocated pointer %p.", ptr);
         // change protection
        WARNINGIF(-1 == SYS_MMAP_PROT(ptr, PageSizeToBytes(it->second->size), prot),
                  "Changing protection of page at address %p size %d failed with message %s",
                  ptr, it->second->size, strerror(errno));
    }
}

void BuddyMemoryAllocator::MmapFree(void* ptr) {
    if (!ptr)
        return;

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        reserved_hash_segs.push_back(ptr);
        occupied_hash_segs.erase(ptr);
        // UpdateStatus(-kHashSegPageSize);
    } else {
        FATALIF(ptr_to_bstchunk.find(ptr) == ptr_to_bstchunk.end(), "Freeing unallocated pointer %p.", ptr);
        BSTreeFree(ptr);
    }
}

void* BuddyMemoryAllocator::HashSegAlloc() {
    void* res_ptr = nullptr;
    if (reserved_hash_segs.empty()) {
        // may use page aligned size
        res_ptr = SYS_MMAP_ALLOC(kHashSegAlignedSize);
        if (!SYS_MMAP_CHECK(res_ptr)){
            perror("BuddyMemoryAllocator");
            FATAL("The memory allocator could not allocate memory");
        }
    } else {
        res_ptr = reserved_hash_segs.back();
        reserved_hash_segs.pop_back();
    }
    occupied_hash_segs.emplace(res_ptr);
    SYS_MMAP_PROT(res_ptr, kHashSegAlignedSize, PROT_READ | PROT_WRITE);
    // UpdateStatus(kHashSegPageSize);
    return res_ptr;
}

void BuddyMemoryAllocator::EraseTreePtr(int size, void* ptr, int node) {
    auto it = numa_num_to_node[node]->free_tree.find(size);
    if (it->second.size() == 1)
        numa_num_to_node[node]->free_tree.erase(it);
    else
        it->second.erase(ptr);
}

void BuddyMemoryAllocator::GrowHeap(int num_pages, int node) {
    int grow_pages = max(HEAP_GROW_BY_SIZE, num_pages);
    void* ptr = SYS_MMAP_ALLOC(PageSizeToBytes(grow_pages));
    FATALIF(!SYS_MMAP_CHECK(ptr),
            "Run out of memory in allocator. Request: %d MB", grow_pages / 2);
    BSTreeChunk* new_chunk = BSTreeChunk::GetChunk(ptr, grow_pages, node, false, nullptr, nullptr);
    ptr_to_bstchunk.emplace(ptr, new_chunk);
    numa_num_to_node[node]->free_tree[grow_pages].insert(ptr);
}

void* BuddyMemoryAllocator::BSTreeAlloc(int num_pages, int node) {
    auto& cur_free_tree = numa_num_to_node[node]->free_tree;
    auto it = cur_free_tree.lower_bound(num_pages);
    if (it == cur_free_tree.end()) {
        return nullptr;
    } else {
        int size = it->first;
        unordered_set<void*>& ptrs = it->second;
        void* fit_ptr = *ptrs.begin();
        EraseTreePtr(size, fit_ptr, node);
        BSTreeChunk* &alloc_chunk = ptr_to_bstchunk[fit_ptr];
        if (size > num_pages) {
            // if the selected free block is larger than the request size
            BSTreeChunk* remain_chunk = alloc_chunk->Split(num_pages);
            cur_free_tree[remain_chunk->size].insert(remain_chunk->mem_ptr);
            ptr_to_bstchunk.emplace(remain_chunk->mem_ptr, remain_chunk);
        }
        UpdateStatus(num_pages);
        return alloc_chunk->mem_ptr;
    }
}

void BuddyMemoryAllocator::BSTreeFree(void* ptr) {
    BSTreeChunk* cur_chunk = ptr_to_bstchunk[ptr];
    int cur_node = cur_chunk->node;
    ptr_to_bstchunk.erase(ptr);
    cur_chunk->used = false;
    UpdateStatus(-cur_chunk->size);
    // coalesce with next chunk if it is free
    BSTreeChunk* chunk = cur_chunk->CoalesceNext();
    if (chunk) {
        EraseTreePtr(chunk->size, chunk->mem_ptr, cur_node);
        ptr_to_bstchunk.erase(chunk->mem_ptr);
    }
    // coalesce with previous chunk if it is free
    chunk = cur_chunk->CoalescePrev();
    if (chunk) {
        EraseTreePtr(chunk->size, chunk->mem_ptr, cur_node);
        ptr_to_bstchunk.erase(chunk->mem_ptr);
    }
    numa_num_to_node[cur_node]->free_tree[cur_chunk->size].insert(cur_chunk->mem_ptr);
    ptr_to_bstchunk[cur_chunk->mem_ptr] = cur_chunk;
}

void BuddyMemoryAllocator::UpdateStatus(int allocated_size) {
    free_pages_ -= allocated_size;
    allocated_pages_ += allocated_size;
}

size_t BuddyMemoryAllocator::AllocatedPages() const {
    return allocated_pages_;
}

size_t BuddyMemoryAllocator::FreePages() const {
    return free_pages_;
}
