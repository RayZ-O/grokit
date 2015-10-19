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
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    void* rez= aloc.MmapAlloc(noBytes, node, f, l);
    return rez;
}

void mmap_prot_read_imp(void* ptr, const char* f, int l) {
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ);
}

void mmap_prot_readwrite_imp(void* ptr, const char* f, int l) {
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ | PROT_WRITE);
}

void mmap_free_imp(void* ptr, const char* f, int l) {
    if( ptr == nullptr ) {
        WARNING("Warning: Attempted free of null pointer at %s:%d", f, l);
    }

    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    aloc.MmapFree(ptr);
}

off_t mmap_used(void) {
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    return PAGES_TO_BYTES(aloc.AllocatedPages());
}

BuddyMemoryAllocator::BuddyMemoryAllocator(void) : mHeapInitialized{false} {
    free_area = vector<list<BuddyChunk*>(MAX_ORDER);
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
    buddy_base = static_cast<char*>(new_chunk);
    free_area[MAX_ORDER - 1].emplace_back(new PageDescriptor(0, MAX_ORDER));

    void* tree_base = static_cast<void*>(buddy_base + PageSizeToBytes(BUDDY_PAGE_SIZE));
    int tree_size = INIT_HEAP_PAGE_SIZE - BUDDY_PAGE_SIZE;

    free_tree.insert({tree_size, tree_base});
}

void* BuddyMemoryAllocator::MmapAlloc(size_t noBytes, int node, const char* f, int l) {
    if (noBytes == 0)
        return nullptr;

    lock_guard<mutex> lck(mtx);

    if (!mHeapInitialized)
        HeapInit();

    int noPages = BytesToPageSize(noBytes);
    void* res_ptr = nullptr;
    if (numPages <= BUDDY_PAGE_SIZE) {
        res_ptr = BuddyAlloc(noPages, node);
    }
    if (!res_ptr) {
        res_ptr = BSTreeAlloc(noPages, node);
    }
    return res_ptr;
}

void* BuddyMemoryAllocator::BuddyAlloc(int noPages, int node) {
    int order = 0;
    for (int current_order = 0; current_order < MAX_ORDER; current_order++) {
        if (numPages < BuddyBlockSize(current_order)) {
            if (free_area[current_order].empty()) {
                order = current_order;
                continue;
            }
            size_t size = 1 << current_order;
            PageDescriptor* pd = free_area[current_order].front();
            free_area[current_order].pop_front();
            BuddyChunk* fit_chunk = new BuddyChunk;
            fit_chunk->mem_ptr = static_cast<void*>(buddy_base + PageSizeToBytes(size));
            fit_chunk->size = order == 0 ? size : 1 << order;
            fit_chunk->pd = pd;
            ptr_to_budchunk.insert({fit_chunk->mem_ptr, fit_chunk});

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

 void* BuddyMemoryAllocator::BSTreeAlloc(int noPages, int node) {
    auto it = free_tree.lower_bound(noPages);
    if (it == free_tree.end()) {
        return nullptr;
    } else {
        BSTreeChunk* tree_chunk = new BSTreeChunk;
        tree_chunk->mem_ptr = it->second;
        tree_chunk->size = noPages;
        tree_chunk->used = true;
        // tree_chunk->prev = TODO
        // tree_chunk->next = TODO
        if (it->first == noPages) {
            ptr_to_bstchunk(it->second, tree_chunk);
            free_tree.erase(it);
        } else {
            void* remain = static_cast<void*>(static_cast<char*>(tree_chunk->mem_ptr) + PageSizeToBytes(size));
            free_tree.insert(it->first - noPages, remain);
        }
        return tree_chunk->mem_ptr;
    }
 }

void BuddyMemoryAllocator::MmapChangeProt(void* ptr, int prot) {
    if (ptr == nullptr) {
        return;
    }

    lock_guard<mutex> lck(mtx);
}

void BuddyMemoryAllocator::MmapFree(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    lock_guard<mutex> lck(mtx);


}

size_t BuddyMemoryAllocator::AllocatedPages() {
    size_t size = 0;

    return size;
}

size_t BuddyMemoryAllocator::FreePages() {
    size_t totalFreelistSize = 0;

    return totalFreelistSize;
}

