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
    : mHeapInitialized(false),  // google code stype constructor initializer lists
      kHashSegPageSize(BytesToPageSize(HASH_SEG_SIZE)),
      kHashSegAlignedSize(PageSizeToBytes(kHashSegPageSize)),
      kBuddyPageSize(1 << MAX_ORDER) {
    free_area.resize(MAX_ORDER);
}


BuddyMemoryAllocator::~BuddyMemoryAllocator(void) {

}

int BuddyMemoryAllocator::BytesToPageSize(size_t bytes) {
    // compute the size in pages
    int pSize = bytes >> ALLOC_PAGE_SIZE_EXPONENT;
    if (bytes != PageSizeToBytes(pSize))
        pSize++; // extra page to get the overflow

    return pSize;
}

size_t BuddyMemoryAllocator::PageSizeToBytes(int pSize) {
    return static_cast<size_t>(pSize) << ALLOC_PAGE_SIZE_EXPONENT;
}

int BuddyMemoryAllocator::BuddyBlockSize(int order) {
    return 1 << order;
}

void BuddyMemoryAllocator::HeapInit() {
    mHeapInitialized = true;

    void *new_chunk = SYS_MMAP_ALLOC(PageSizeToBytes(INIT_HEAP_PAGE_SIZE));
    if (!SYS_MMAP_CHECK(new_chunk)) {
        perror("BuddyMemoryAllocator");
        FATAL("The memory allocator could not allocate memory");
    }
    // initalized buddy system
    buddy_base = reinterpret_cast<char*>(new_chunk);
    free_area[MAX_ORDER - 1].emplace_back(new PageDescriptor(0, MAX_ORDER));

    void* tree_base = reinterpret_cast<void*>(buddy_base + PageSizeToBytes(BUDDY_PAGE_SIZE));
    int tree_size = INIT_HEAP_PAGE_SIZE - BUDDY_PAGE_SIZE;

    free_tree.emplace(tree_size, tree_base);

    BSTreeChunk* tree_chunk = new BSTreeChunk(tree_base, tree_size, false);
    ptr_to_bstchunk.emplace(tree_base, tree_chunk);
}

void* BuddyMemoryAllocator::MmapAlloc(size_t noBytes, int node, const char* f, int l) {
    if (0 == noBytes)
        return nullptr;

    lock_guard<mutex> lck(mtx);

    if (!mHeapInitialized)
        HeapInit();

    int num_pages = BytesToPageSize(noBytes);
    // TODO make hash seg page size a constexpr to avoid evaluate every time
    void* res_ptr = nullptr;
    if (kHashSegPageSize == num_pages) {
        return HashSegAlloc();
    }
    if (num_pages <= BUDDY_PAGE_SIZE) {
        res_ptr = BuddyAlloc(num_pages, node);
    }
    if (!res_ptr) {
        res_ptr = BSTreeAlloc(num_pages, node);
    }
    return res_ptr;
}

void* BuddyMemoryAllocator::HashSegAlloc() {
    void* res_ptr = nullptr;
    if (reserved_hash_segs.empty()) {
        // may use page aligned size
        void* res_ptr = SYS_MMAP_ALLOC(kHashSegAlignedSize);
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
    return res_ptr;
}


void* BuddyMemoryAllocator::BuddyAlloc(int num_pages, int node) {
    int order = 0;
    for (int current_order = 0; current_order < MAX_ORDER; current_order++) {
        if (num_pages < BuddyBlockSize(current_order)) {
            if (free_area[current_order].empty()) {
                order = current_order;
                continue;
            }
            size_t size = 1 << current_order;
            PageDescriptor* pd = free_area[current_order].front();
            free_area[current_order].pop_front();

            void* mem_ptr = reinterpret_cast<void*>(buddy_base + PageSizeToBytes(size));
            int chunk_size = 0 == order ? size : 1 << order;
            BuddyChunk* fit_chunk = new BuddyChunk(mem_ptr, chunk_size, pd);
            ptr_to_budchunk.emplace(fit_chunk->mem_ptr, fit_chunk);

            while (current_order > order) {
                current_order--;
                size >>= 1;
                PageDescriptor* buddy = new PageDescriptor(pd->page_index + size, current_order);
                free_area[current_order].emplace_back(buddy);
            }
            return fit_chunk->mem_ptr;
        }
    }
    return nullptr;
}

 void* BuddyMemoryAllocator::BSTreeAlloc(int num_pages, int node) {
    auto it = free_tree.lower_bound(num_pages);
    if (it == free_tree.end()) {
        return nullptr;
    } else {
        BSTreeChunk* alloc_chunk = ptr_to_bstchunk[it->second];
        // if exactly same size free block is found
        if (it->first == num_pages) {
            alloc_chunk.used = true;
            free_tree.erase(it);
        } else {
            // if the selected free block is larger than the request size
            // get the pointer to the beginning of the remain free size and insert into free tree
            void* remain = reinterpret_cast<void*>(reinterpret_cast<char*>(alloc_chunk->mem_ptr) +
                                                   PageSizeToBytes(num_pages));
            int remain_size = it->first - num_pages;
            free_tree.emplace(remain_size, remain);

            BSTreeChunk* remain_chunk = new BSTreeChunk(remain, remain_size, false);
            alloc_chunk->next = remain_chunk;
            remain_chunk->prev = alloc_chunk;
            ptr_to_bstchunk.emplace(remain, remain_chunk);
        }
        return alloc_chunk->mem_ptr;
    }
 }

void BuddyMemoryAllocator::MmapChangeProt(void* ptr, int prot) {
    if (!ptr) {
        return;
    }

    lock_guard<mutex> lck(mtx);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        SYS_MMAP_PROT(ptr, kHashSegAlignedSize-, prot);
    } else if (ptr_to_budchunk.find(ptr) != ptr_to_budchunk.end()) {
        WARNINGIF(-1 == SYS_MMAP_PROT(ptr, PageSizeToBytes(ptr_to_budchunk[ptr]->size), prot),
                "Changing protection of page at address %p size %d failed with message %s",
                ptr, ptr_to_budchunk[ptr]->size, strerror(errno));
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
    if (!ptr) {
        return;
    }

    lock_guard<mutex> lck(mtx);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        reserved_hash_segs.push_back(ptr);
        occupied_hash_segs.erase(ptr);
    } else if (ptr_to_budchunk.find(ptr) != ptr_to_budchunk.end()) {
        BuddyFree(ptr);
    } else {
        FATALIF(ptr_to_bstchunk.find(ptr) == ptr_to_bstchunk.end(), "Freeing unallocated pointer %p.", ptr);
        BSTreeFree(ptr);
    }
}

void BuddyMemoryAllocator::BuddyFree(void* ptr) {
    BuddyChunk* cur_chunk = ptr_to_budchunk[ptr];
    // TODO Coalesce with buddy
}

void BuddyMemoryAllocator::BSTreeFree(void* ptr) {-
    BSTreeChunk* cur_chunk = ptr_to_bstchunk[ptr];
    cur_chunk.used = false;
    // total size of physical contiguous memory
    int size = cur_chunk->size;
    // point to the beginning of the contiguous memory
    void* beg_ptr = cur_chunk->mem_ptr;
    // iterative coalesce with previous chunk if it is free
    BSTreeChunk* prevChunk = cur_chunk->prev;
    while (prevChunk) {
        if (true == prevChunk->used)
            break;
        size += prevChunk->size;
        // update beginning pointer
        beg_ptr = prevChunk->mem_ptr;
        prevChunk = prevChunk->prev;
    }
    // iterative coalesce with next chunk if it is free
    BSTreeChunk* nextChunk = cur_chunk->next;
    while (nextChunk) {
        if (true == nextChunk->used)
            break;
        size += nextChunk->size;
        nextChunk = nextChunk->next;
    }

    free_tree.emplace(size, beg_ptr);
}

size_t BuddyMemoryAllocator::AllocatedPages() {
    size_t size = 0;

    return size;
}

size_t BuddyMemoryAllocator::FreePages() {
    size_t totalFreelistSize = 0;

    return totalFreelistSize;
}

